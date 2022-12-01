/*
 * File operations used by nfsd. Some of these have been ripped from
 * other parts of the kernel because they weren't exported, others
 * are partial duplicates with added or changed functionality.
 *
 * Note that several functions dget() the dentry upon which they want
 * to act, most notably those that create directory entries. Response
 * dentry's are dput()'d if necessary in the release callback.
 * So if you notice code paths that apparently fail to dput() the
 * dentry, don't worry--they have been taken care of.
 *
 * Copyright (C) 1995-1999 Olaf Kirch <okir@monad.swb.de>
 * Zerocpy NFS support (C) 2002 Hirokazu Takahashi <taka@valinux.co.jp>
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>
#include <linux/fcntl.h>
#include <linux/namei.h>
#include <linux/delay.h>
#include <linux/fsnotify.h>
#include <linux/posix_acl_xattr.h>
#include <linux/xattr.h>
#include <linux/ima.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/exportfs.h>
#include <linux/writeback.h>
#include <linux/security.h>

#include "xdr.h"
#include "nfsd.h"
#include "vfs.h"

__be32
nfsd_lookup_dentry(struct svc_rqst *rqstp, struct svc_fh *fhp,
		   const char *name, unsigned int len,
		   struct svc_export **exp_ret, struct dentry **dentry_ret)
{
	struct svc_export	*exp;
	struct dentry		*dparent;
	struct dentry		*dentry = NULL;
	int			host_err;

	printk(KERN_INFO "nfsd: nfsd_lookup(fh %s, %.*s)\n", SVCFH_fmt(fhp), len,name);

	dparent = fhp->fh_dentry;
	exp = exp_get(fhp->fh_export);

	/* Lookup the name, but don't follow links */
	if (isdotent(name, len)) {
		if (len==1)
			dentry = dget(dparent);
		else if (dparent != exp->ex_path.dentry)
			dentry = dget_parent(dparent);
		else {
			dentry = dget(dparent); /* .. == . just like at / */
		}
	} else {
		/*
		 * In the nfsd4_open() case, this may be held across
		 * subsequent open and delegation acquisition which may
		 * need to take the child's i_mutex:
		 */
		dentry = lookup_one_len(name, dparent, len);
		host_err = PTR_ERR(dentry);
		if (IS_ERR(dentry))
			goto out_nfserr;
	}
	*dentry_ret = dentry;
	*exp_ret = exp;
	return 0;

out_nfserr:
	exp_put(exp);
	return nfserrno(host_err);
}

/*
 * Look up one component of a pathname.
 * N.B. After this call _both_ fhp and resfh need an fh_put
 *
 * If the lookup would cross a mountpoint, and the mounted filesystem
 * is exported to the client with NFSEXP_NOHIDE, then the lookup is
 * accepted as it stands and the mounted directory is
 * returned. Otherwise the covered directory is returned.
 * NOTE: this mountpoint crossing is not supported properly by all
 *   clients and is explicitly disallowed for NFSv3
 *      NeilBrown <neilb@cse.unsw.edu.au>
 */
__be32
nfsd_lookup(struct svc_rqst *rqstp, struct svc_fh *fhp, const char *name,
				unsigned int len, struct svc_fh *resfh)
{
	struct svc_export	*exp;
	struct dentry		*dentry;
	__be32 err;

	err = fh_verify(rqstp, fhp);
	if (err)
		return err;
	err = nfsd_lookup_dentry(rqstp, fhp, name, len, &exp, &dentry);
	if (err)
		return err;
	/*
	 * Note: we compose the file handle now, but as the
	 * dentry may be negative, it may need to be updated.
	 */
	err = fh_compose(resfh, exp, dentry, fhp);
	if (!err && !dentry->d_inode)
		err = nfserr_noent;
	dput(dentry);
	exp_put(exp);
	return err;
}

/*
 * Commit metadata changes to stable storage.
 */
static int
commit_metadata(struct svc_fh *fhp)
{
	struct inode *inode = fhp->fh_dentry->d_inode;
	const struct export_operations *export_ops = inode->i_sb->s_export_op;

	if (!EX_ISSYNC(fhp->fh_export))
		return 0;

	if (export_ops->commit_metadata){
		/* xfs has its own commit metadata function. */
		printk(KERN_INFO "this file system does have its own commit metadata function.\n");
		return export_ops->commit_metadata(inode);
	}
	return sync_inode_metadata(inode, 1);
}

/*
 * Go over the attributes and take care of the small differences between
 * NFS semantics and what Linux expects.
 */
static void
nfsd_sanitize_attrs(struct inode *inode, struct iattr *iap)
{
	/* sanitize the mode change */
	if (iap->ia_valid & ATTR_MODE) {
		iap->ia_mode &= S_IALLUGO;
		iap->ia_mode |= (inode->i_mode & ~S_IALLUGO);
	}

	/* Revoke setuid/setgid on chown */
	if (!S_ISDIR(inode->i_mode) &&
	    ((iap->ia_valid & ATTR_UID) || (iap->ia_valid & ATTR_GID))) {
		iap->ia_valid |= ATTR_KILL_PRIV;
		if (iap->ia_valid & ATTR_MODE) {
			/* we're setting mode too, just clear the s*id bits */
			iap->ia_mode &= ~S_ISUID;
			if (iap->ia_mode & S_IXGRP)
				iap->ia_mode &= ~S_ISGID;
		} else {
			/* set ATTR_KILL_* bits and let VFS handle it */
			iap->ia_valid |= (ATTR_KILL_SUID | ATTR_KILL_SGID);
		}
	}
}

/*
 * Set various file attributes.  After this call fhp needs an fh_put.
 */
__be32
nfsd_setattr(struct svc_rqst *rqstp, struct svc_fh *fhp, struct iattr *iap,
	     int check_guard, time_t guardtime)
{
	struct dentry	*dentry;
	struct inode	*inode;
	umode_t		ftype = 0;
	__be32		err;
	int		host_err;
	bool		get_write_count;
	bool		size_change = (iap->ia_valid & ATTR_SIZE);

	if (iap->ia_valid & ATTR_SIZE)
		ftype = S_IFREG;

	/* Callers that do fh_verify should do the fh_want_write: */
	get_write_count = !fhp->fh_dentry;

	/* Get inode */
	err = fh_verify(rqstp, fhp);
	if (err)
		return err;
	if (get_write_count) {
		host_err = fh_want_write(fhp);
		if (host_err)
			goto out;
	}

	dentry = fhp->fh_dentry;
	inode = dentry->d_inode;

	/* Ignore any mode updates on symlinks */
	if (S_ISLNK(inode->i_mode))
		iap->ia_valid &= ~ATTR_MODE;

	if (!iap->ia_valid)
		return 0;

	nfsd_sanitize_attrs(inode, iap);

	if (check_guard && guardtime != inode->i_ctime.tv_sec)
		return nfserr_notsync;

	if (size_change) {
		/*
		 * RFC5661, Section 18.30.4:
		 *   Changing the size of a file with SETATTR indirectly
		 *   changes the time_modify and change attributes.
		 *
		 * (and similar for the older RFCs)
		 */
		struct iattr size_attr = {
			.ia_valid	= ATTR_SIZE | ATTR_CTIME | ATTR_MTIME,
			.ia_size	= iap->ia_size,
		};

		host_err = notify_change(dentry, &size_attr, NULL);
		if (host_err)
			goto out;
		iap->ia_valid &= ~ATTR_SIZE;

		/*
		 * Avoid the additional setattr call below if the only other
		 * attribute that the client sends is the mtime, as we update
		 * it as part of the size change above.
		 */
		if ((iap->ia_valid & ~ATTR_MTIME) == 0)
			goto out;
	}

	iap->ia_valid |= ATTR_CTIME;
	host_err = notify_change(dentry, iap, NULL);

out:
	if (!host_err)
		host_err = commit_metadata(fhp);
	return nfserrno(host_err);
}

static int nfsd_open_break_lease(struct inode *inode, int access)
{
	unsigned int mode;

	mode = (access & NFSD_MAY_WRITE) ? O_WRONLY : O_RDONLY;
	return break_lease(inode, mode | O_NONBLOCK);
}

/*
 * Open an existing file or directory.
 * The may_flags argument indicates the type of open (read/write/lock)
 * and additional flags.
 * N.B. After this call fhp needs an fh_put
 */
__be32
nfsd_open(struct svc_rqst *rqstp, struct svc_fh *fhp, umode_t type,
			int may_flags, struct file **filp)
{
	struct path	path;
	struct inode	*inode;
	struct file	*file;
	int		flags = O_RDONLY|O_LARGEFILE;
	__be32		err;
	int		host_err = 0;

	validate_process_creds();

	/*
	 * If we get here, then the client has already done an "open",
	 * and (hopefully) checked permission - so allow OWNER_OVERRIDE
	 * in case a chmod has now revoked permission.
	 *
	 * Arguably we should also allow the owner override for
	 * directories, but we never have and it doesn't seem to have
	 * caused anyone a problem.  If we were to change this, note
	 * also that our filldir callbacks would need a variant of
	 * lookup_one_len that doesn't check permissions.
	 */
	err = fh_verify(rqstp, fhp);
	if (err)
		goto out;

	path.mnt = fhp->fh_export->ex_path.mnt;
	path.dentry = fhp->fh_dentry;
	inode = path.dentry->d_inode;

	/* Disallow write access to files with the append-only bit set
	 * or any access when mandatory locking enabled
	 */
	err = nfserr_perm;

	if (!inode->i_fop)
		goto out;

	host_err = nfsd_open_break_lease(inode, may_flags);
	if (host_err) /* NOMEM or WOULDBLOCK */
		goto out_nfserr;

	if (may_flags & NFSD_MAY_WRITE) {
		if (may_flags & NFSD_MAY_READ)
			flags = O_RDWR|O_LARGEFILE;
		else
			flags = O_WRONLY|O_LARGEFILE;
	}

	file = dentry_open(&path, flags, current_cred());
	if (IS_ERR(file)) {
		host_err = PTR_ERR(file);
		goto out_nfserr;
	}

	host_err = ima_file_check(file, may_flags);
	if (host_err) {
		fput(file);
		goto out_nfserr;
	}

	*filp = file;
out_nfserr:
	err = nfserrno(host_err);
out:
	validate_process_creds();
	return err;
}

static __be32
nfsd_finish_read(struct file *file, unsigned long *count, int host_err)
{
	if (host_err >= 0) {
		*count = host_err;
		fsnotify_access(file);
		return 0;
	} else 
		return nfserrno(host_err);
}

__be32 nfsd_readv(struct file *file, loff_t offset, struct kvec *vec, int vlen,
		unsigned long *count)
{
	mm_segment_t oldfs;
	int host_err;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	host_err = vfs_readv(file, (struct iovec __user *)vec, vlen, &offset);
	set_fs(oldfs);
	return nfsd_finish_read(file, count, host_err);
}

static __be32
nfsd_vfs_read(struct svc_rqst *rqstp, struct file *file,
	      loff_t offset, struct kvec *vec, int vlen, unsigned long *count)
{
	return nfsd_readv(file, offset, vec, vlen, count);
}

__be32
nfsd_vfs_write(struct svc_rqst *rqstp, struct svc_fh *fhp, struct file *file,
				loff_t offset, struct kvec *vec, int vlen,
				unsigned long *cnt, int stable)
{
	struct svc_export	*exp;
	struct inode		*inode;
	mm_segment_t		oldfs;
	__be32			err = 0;
	int			host_err;
	int			use_wgather;
	loff_t			pos = offset;
	loff_t			end = LLONG_MAX;
	unsigned int		pflags = current->flags;

	if (test_bit(RQ_LOCAL, &rqstp->rq_flags))
		/*
		 * We want less throttling in balance_dirty_pages()
		 * and shrink_inactive_list() so that nfs to
		 * localhost doesn't cause nfsd to lock up due to all
		 * the client's dirty pages or its congested queue.
		 */
		current->flags |= PF_LESS_THROTTLE;

	inode = file_inode(file);
	exp   = fhp->fh_export;

	use_wgather = 0;

	if (!EX_ISSYNC(exp))
		stable = NFS_UNSTABLE;

	/* Write the data. */
	oldfs = get_fs(); set_fs(KERNEL_DS);
	host_err = vfs_writev(file, (struct iovec __user *)vec, vlen, &pos);
	set_fs(oldfs);
	if (host_err < 0)
		goto out_nfserr;
	*cnt = host_err;
	fsnotify_modify(file);

	if (stable) {
		if (*cnt)
			end = offset + *cnt - 1;
		host_err = vfs_fsync_range(file, offset, end, 0);
	}

out_nfserr:
	printk(KERN_INFO "nfsd: write complete host_err=%d\n", host_err);
	if (host_err >= 0)
		err = 0;
	else
		err = nfserrno(host_err);
	if (test_bit(RQ_LOCAL, &rqstp->rq_flags))
		tsk_restore_flags(current, pflags, PF_LESS_THROTTLE);
	return err;
}

/*
 * Read data from a file. count must contain the requested read count
 * on entry. On return, *count contains the number of bytes actually read.
 * N.B. After this call fhp needs an fh_put
 */
__be32 nfsd_read(struct svc_rqst *rqstp, struct svc_fh *fhp,
	loff_t offset, struct kvec *vec, int vlen, unsigned long *count)
{
	struct file *file;
	__be32 err;

	err = nfsd_open(rqstp, fhp, S_IFREG, NFSD_MAY_READ, &file);
	if (err)
		return err;

	err = nfsd_vfs_read(rqstp, file, offset, vec, vlen, count);

	fput(file);

	return err;
}

/*
 * Write data to a file.
 * The stable flag requests synchronous writes.
 * N.B. After this call fhp needs an fh_put
 */
__be32
nfsd_write(struct svc_rqst *rqstp, struct svc_fh *fhp, loff_t offset,
	   struct kvec *vec, int vlen, unsigned long *cnt, int stable)
{
	struct file *file = NULL;
	__be32 err = 0;

	err = nfsd_open(rqstp, fhp, S_IFREG, NFSD_MAY_WRITE, &file);
	if (err)
		goto out;

	err = nfsd_vfs_write(rqstp, fhp, file, offset, vec, vlen, cnt, stable);
	fput(file);
out:
	return err;
}

static __be32
nfsd_create_setattr(struct svc_rqst *rqstp, struct svc_fh *resfhp,
			struct iattr *iap)
{
	/*
	 * Mode has already been set earlier in create:
	 */
	iap->ia_valid &= ~ATTR_MODE;
	/*
	 * Setting uid/gid works only for root.  Irix appears to
	 * send along the gid on create when it tries to implement
	 * setgid directories via NFS:
	 */
	if (!uid_eq(current_fsuid(), GLOBAL_ROOT_UID))
		iap->ia_valid &= ~(ATTR_UID|ATTR_GID);
	if (iap->ia_valid)
		return nfsd_setattr(rqstp, resfhp, iap, 0, (time_t)0);
	/* Callers expect file metadata to be committed here */
	return nfserrno(commit_metadata(resfhp));
}

/*
 * Create a file (regular, directory, device, fifo); UNIX sockets 
 * not yet implemented.
 * If the response fh has been verified, the parent directory should
 * already be locked. Note that the parent directory is left locked.
 *
 * N.B. Every call to nfsd_create needs an fh_put for _both_ fhp and resfhp
 */
__be32
nfsd_create(struct svc_rqst *rqstp, struct svc_fh *fhp,
		char *fname, int flen, struct iattr *iap,
		int type, dev_t rdev, struct svc_fh *resfhp)
{
	struct dentry	*dentry, *dchild = NULL;
	struct inode	*dirp;
	__be32		err;
	__be32		err2;
	int		host_err;

	err = nfserr_perm;
	if (!flen)
		goto out;
	err = nfserr_exist;
	if (isdotent(fname, flen))
		goto out;

	err = fh_verify(rqstp, fhp);
	if (err)
		goto out;

	dentry = fhp->fh_dentry;
	dirp = dentry->d_inode;

	err = nfserr_notdir;
	if (!dirp->i_op->lookup)
		goto out;
	/*
	 * Check whether the response file handle has been verified yet.
	 * If it has, the parent directory should already be locked.
	 */
	if (!resfhp->fh_dentry) {
		host_err = fh_want_write(fhp);
		if (host_err)
			goto out_nfserr;

		/* called from nfsd_proc_mkdir, or possibly nfsd3_proc_create */
		dchild = lookup_one_len(fname, dentry, flen);
		host_err = PTR_ERR(dchild);
		if (IS_ERR(dchild))
			goto out_nfserr;
		err = fh_compose(resfhp, fhp->fh_export, dchild, fhp);
		if (err)
			goto out;
	} else {
		/* called from nfsd_proc_create */
		dchild = dget(resfhp->fh_dentry);
	}
	/*
	 * Make sure the child dentry is still negative ...
	 */
	err = nfserr_exist;
	if (dchild->d_inode) {
		printk(KERN_INFO "nfsd_create: dentry %pd/%pd not negative!\n",
			dentry, dchild);
		goto out; 
	}

	if (!(iap->ia_valid & ATTR_MODE))
		iap->ia_mode = 0;
	iap->ia_mode = (iap->ia_mode & S_IALLUGO) | type;

	err = nfserr_inval;
	if (!S_ISREG(type) && !S_ISDIR(type) && !special_file(type)) {
		printk(KERN_WARNING "nfsd: bad file type %o in nfsd_create\n",
		       type);
		goto out;
	}

	/*
	 * Get the dir op function pointer.
	 */
	err = 0;
	host_err = 0;
	switch (type) {
	case S_IFREG:
		host_err = vfs_create(dirp, dchild, iap->ia_mode, true);
		break;
	case S_IFDIR:
		host_err = vfs_mkdir(dirp, dchild, iap->ia_mode);
		break;
	}
	if (host_err < 0)
		goto out_nfserr;

	err = nfsd_create_setattr(rqstp, resfhp, iap);

	/*
	 * nfsd_create_setattr already committed the child.  Transactional
	 * filesystems had a chance to commit changes for both parent and
	 * child * simultaneously making the following commit_metadata a
	 * noop.
	 */
	err2 = nfserrno(commit_metadata(fhp));
	if (err2)
		err = err2;
	/*
	 * Update the file handle to get the new inode info.
	 */
	if (!err)
		err = fh_update(resfhp);
out:
	if (dchild && !IS_ERR(dchild))
		dput(dchild);
	return err;

out_nfserr:
	err = nfserrno(host_err);
	goto out;
}

/*
 * NFSv3 and NFSv4 version of nfsd_create
 */
__be32
do_nfsd_create(struct svc_rqst *rqstp, struct svc_fh *fhp,
		char *fname, int flen, struct iattr *iap,
		struct svc_fh *resfhp, int createmode, u32 *verifier,
	        bool *truncp, bool *created)
{
	struct dentry	*dentry, *dchild = NULL;
	struct inode	*dirp;
	__be32		err;
	int		host_err;
	__u32		v_mtime=0, v_atime=0;

	err = nfserr_perm;
	if (!flen)
		goto out;
	err = nfserr_exist;
	if (isdotent(fname, flen))
		goto out;
	if (!(iap->ia_valid & ATTR_MODE))
		iap->ia_mode = 0;
	err = fh_verify(rqstp, fhp);
	if (err)
		goto out;

	dentry = fhp->fh_dentry;
	dirp = dentry->d_inode;

	/* Get all the sanity checks out of the way before
	 * we lock the parent. */
	err = nfserr_notdir;
	if (!dirp->i_op->lookup)
		goto out;

	host_err = fh_want_write(fhp);
	if (host_err)
		goto out_nfserr;

	/*
	 * Compose the response file handle.
	 */
	dchild = lookup_one_len(fname, dentry, flen);
	host_err = PTR_ERR(dchild);
	if (IS_ERR(dchild))
		goto out_nfserr;

	/* If file doesn't exist, check for permissions to create one */
	if (!dchild->d_inode) {
		err = fh_verify(rqstp, fhp);
		if (err)
			goto out;
	}

	err = fh_compose(resfhp, fhp->fh_export, dchild, fhp);
	if (err)
		goto out;

	if (dchild->d_inode) {
		err = 0;

		switch (createmode) {
		case NFS3_CREATE_UNCHECKED:
			if (! S_ISREG(dchild->d_inode->i_mode))
				goto out;
			else if (truncp) {
				/* in nfsv4, we need to treat this case a little
				 * differently.  we don't want to truncate the
				 * file now; this would be wrong if the OPEN
				 * fails for some other reason.  furthermore,
				 * if the size is nonzero, we should ignore it
				 * according to spec!
				 */
				*truncp = (iap->ia_valid & ATTR_SIZE) && !iap->ia_size;
			}
			else {
				iap->ia_valid &= ATTR_SIZE;
				goto set_attr;
			}
			break;
		case NFS3_CREATE_EXCLUSIVE:
			if (   dchild->d_inode->i_mtime.tv_sec == v_mtime
			    && dchild->d_inode->i_atime.tv_sec == v_atime
			    && dchild->d_inode->i_size  == 0 ) {
				if (created)
					*created = 1;
				break;
			}
			 /* fallthru */
		case NFS3_CREATE_GUARDED:
			err = nfserr_exist;
		}
		fh_drop_write(fhp);
		goto out;
	}

	host_err = vfs_create(dirp, dchild, iap->ia_mode, true);
	if (host_err < 0) {
		fh_drop_write(fhp);
		goto out_nfserr;
	}
	if (created)
		*created = 1;

 set_attr:
	err = nfsd_create_setattr(rqstp, resfhp, iap);

	/*
	 * nfsd_create_setattr already committed the child
	 * (and possibly also the parent).
	 */
	if (!err)
		err = nfserrno(commit_metadata(fhp));

	/*
	 * Update the filehandle to get the new inode info.
	 */
	if (!err)
		err = fh_update(resfhp);

 out:
	if (dchild && !IS_ERR(dchild))
		dput(dchild);
	fh_drop_write(fhp);
 	return err;
 
 out_nfserr:
	err = nfserrno(host_err);
	goto out;
}

/*
 * unlink a file or directory. fhp is the file handle representing the parent.
 * type will be either S_IFDIR, or -S_IFDIR.
 * N.B. After this call fhp needs an fh_put
 */
__be32
nfsd_unlink(struct svc_rqst *rqstp, struct svc_fh *fhp, int type,
				char *fname, int flen)
{
	struct dentry	*dentry, *rdentry;
	struct inode	*dirp;
	__be32		err;
	int		host_err;

	err = nfserr_acces;
	/* file name lenth can't be 0, maybe we should use <=0? and you can't delete '.' or '..'. */
	if (!flen || isdotent(fname, flen))
		goto out;
	err = fh_verify(rqstp, fhp);
	if (err)
		goto out;

	/* tells the low-level filesystem that a write is about to be performed to it, and makes sure that writes are allowed.
	 * later on, we need to call fh_drop_write() which tells the low-level filesystem that we are done performing writes to it and
	 * also allows filesystem to be frozen again. */
	host_err = fh_want_write(fhp);
	if (host_err)
		goto out_nfserr;

	/* parent's dentry */
	dentry = fhp->fh_dentry;
	/* parent's inode */
	dirp = dentry->d_inode;

	/* the dentry which represents the child that we are going to delete. this function looks up fname inside the directory which is represented by dentry, 
	 * if found, this function returns the corresponding dentry. */
	rdentry = lookup_one_len(fname, dentry, flen);
	/* if rdentry is an invalid pointer, goto out_nfserr. */
	host_err = PTR_ERR(rdentry);
	if (IS_ERR(rdentry))
		goto out_nfserr;

	/* if this dentry doesn't even have a corresponding inode, then just release the dentry. */
	if (!rdentry->d_inode) {
		/* dput(): release a dentry. this will drop the usage count and if appropriate
		 * call the dentry unlink method as well as removing it from the queues and
		 * releasing its resources. */
		dput(rdentry);
		err = nfserr_noent;
		goto out;
	}

	/* not a directory */
	if (type != S_IFDIR)
		host_err = vfs_unlink(dirp, rdentry, NULL);
	/* if it is a directory */
	else
		host_err = vfs_rmdir(dirp, rdentry);
	if (!host_err)
		host_err = commit_metadata(fhp);
	/* now it's the right time to release the dentry. */
	dput(rdentry);

out_nfserr:
	err = nfserrno(host_err);
out:
	return err;
}

/*
 * We do this buffering because we must not call back into the file
 * system's ->lookup() method from the filldir callback. That may well
 * deadlock a number of file systems.
 *
 * This is based heavily on the implementation of same in XFS.
 */
struct buffered_dirent {
	u64		ino;
	loff_t		offset;
	int		namlen;
	unsigned int	d_type;
	char		name[];
};

struct readdir_data {
	struct dir_context ctx;
	char		*dirent;
	size_t		used;
	int		full;
};

static int nfsd_buffered_filldir(void *__buf, const char *name, int namlen,
				 loff_t offset, u64 ino, unsigned int d_type)
{
	struct readdir_data *buf = __buf;
	struct buffered_dirent *de = (void *)(buf->dirent + buf->used);
	unsigned int reclen;

	reclen = ALIGN(sizeof(struct buffered_dirent) + namlen, sizeof(u64));
	if (buf->used + reclen > PAGE_SIZE) {
		buf->full = 1;
		return -EINVAL;
	}

	de->namlen = namlen;
	de->offset = offset;
	de->ino = ino;
	de->d_type = d_type;
	memcpy(de->name, name, namlen);
	buf->used += reclen;

	return 0;
}

static __be32 nfsd_buffered_readdir(struct file *file, filldir_t func,
				    struct readdir_cd *cdp, loff_t *offsetp)
{
	struct readdir_data buf;
	struct buffered_dirent *de;
	int host_err;
	int size;
	loff_t offset;

	buf.ctx.actor = nfsd_buffered_filldir;
	buf.dirent = (void *)__get_free_page(GFP_KERNEL);
	if (!buf.dirent)
		return nfserrno(-ENOMEM);

	offset = *offsetp;

	while (1) {
		unsigned int reclen;

		cdp->err = nfserr_eof; /* will be cleared on successful read */
		buf.used = 0;
		buf.full = 0;

		host_err = iterate_dir(file, &buf.ctx);
		if (buf.full)
			host_err = 0;

		if (host_err < 0)
			break;

		size = buf.used;

		if (!size)
			break;

		de = (struct buffered_dirent *)buf.dirent;
		while (size > 0) {
			offset = de->offset;

			if (func(cdp, de->name, de->namlen, de->offset,
				 de->ino, de->d_type))
				break;

			if (cdp->err != nfs_ok)
				break;

			reclen = ALIGN(sizeof(*de) + de->namlen,
				       sizeof(u64));
			size -= reclen;
			de = (struct buffered_dirent *)((char *)de + reclen);
		}
		if (size > 0) /* We bailed out early */
			break;

		offset = vfs_llseek(file, 0, SEEK_CUR);
	}

	free_page((unsigned long)(buf.dirent));

	if (host_err)
		return nfserrno(host_err);

	*offsetp = offset;
	return cdp->err;
}

/*
 * Read entries from a directory.
 * The  NFSv3/4 verifier we ignore for now.
 */
__be32
nfsd_readdir(struct svc_rqst *rqstp, struct svc_fh *fhp, loff_t *offsetp, 
	     struct readdir_cd *cdp, filldir_t func)
{
	__be32		err;
	struct file	*file;
	loff_t		offset = *offsetp;
	int             may_flags = NFSD_MAY_READ;

	err = nfsd_open(rqstp, fhp, S_IFDIR, may_flags, &file);
	if (err)
		goto out;

	offset = vfs_llseek(file, offset, SEEK_SET);
	if (offset < 0) {
		err = nfserrno((int)offset);
		goto out_close;
	}

	err = nfsd_buffered_readdir(file, func, cdp, offsetp);

	if (err == nfserr_eof || err == nfserr_toosmall)
		err = nfs_ok; /* can still be found in ->err */
out_close:
	fput(file);
out:
	return err;
}

/*
 * Get file system stats
 * N.B. After this call fhp needs an fh_put
 */
__be32
nfsd_statfs(struct svc_rqst *rqstp, struct svc_fh *fhp, struct kstatfs *stat, int access)
{
	__be32 err;

	err = fh_verify(rqstp, fhp);
	if (!err) {
		struct path path = {
			.mnt	= fhp->fh_export->ex_path.mnt,
			.dentry	= fhp->fh_dentry,
		};
		if (vfs_statfs(&path, stat))
			err = nfserr_io;
	}
	return err;
}
