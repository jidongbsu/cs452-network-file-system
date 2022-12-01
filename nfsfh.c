/*
 * NFS server file handle treatment.
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 * Portions Copyright (C) 1999 G. Allen Morris III <gam3@acm.org>
 * Extensive rewrite by Neil Brown <neilb@cse.unsw.edu.au> Southern-Spring 1999
 * ... and again Southern-Winter 2001 to support export_operations
 */

#include <linux/exportfs.h>
#include <linux/sched.h>

#include "nfsd.h"
#include "vfs.h"

/*
 * our acceptability function.
 */
static int nfsd_acceptable(void *expv, struct dentry *dentry)
{
	return 1;	// always acceptable, authentication is not important for us.
}

/* set the current process's fsuid/fsgid etc to those of the NFS
 * client user */
int nfsd_setuser(struct svc_rqst *rqstp, struct svc_export *exp)
{
	struct cred *new;

	validate_process_creds();

	/* discard any old override before preparing the new set */
	revert_creds(get_cred(current_real_cred()));
	new = prepare_creds();
	if (!new)
		return -ENOMEM;

	new->fsuid = rqstp->rq_cred.cr_uid;
	new->fsgid = rqstp->rq_cred.cr_gid;
	new->cap_effective = cap_drop_nfsd_set(new->cap_effective);
	validate_process_creds();
	put_cred(override_creds(new));
	put_cred(new);
	validate_process_creds();
	return 0;
}

/*
 * Use the given filehandle to look up the corresponding export and
 * dentry.  On success, the results are used to set fh_export and
 * fh_dentry.
 */
static __be32 nfsd_set_fh_dentry(struct svc_rqst *rqstp, struct svc_fh *fhp)
{
	struct knfsd_fh	*fh = &fhp->fh_handle;
	struct fid *fid = NULL;
	struct svc_export *exp;
	struct dentry *dentry;
	int fileid_type;
	int data_left = fh->fh_size/4;
	__be32 error;
	int len;

	error = nfserr_badhandle;

	if (--data_left < 0)
		return error;
	if (fh->fh_auth_type != 0)
		return error;
	len = key_len(fh->fh_fsid_type) / 4;
	if (len == 0)
		return error;
	data_left -= len;
	if (data_left < 0)
		return error;
	exp = rqst_exp_find(rqstp, fh->fh_fsid_type, fh->fh_fsid);
	fid = (struct fid *)(fh->fh_fsid + len);

	error = nfserr_stale;
	if (PTR_ERR(exp) == -ENOENT)
		return error;

	if (IS_ERR(exp))
		return nfserrno(PTR_ERR(exp));

	/* Set user creds for this exportpoint */
	error = nfsd_setuser(rqstp, exp);
	if (error)
		goto out;

	/*
	 * Look up the dentry using the NFS file handle.
	 */
	error = nfserr_badhandle;

	fileid_type = fh->fh_fileid_type;
	/* it seems that for xfs, this fileid_type is either 0, or 129, or 130 (i.e., 0x81, 0x82), 
	 * and it is defined in fs/xfs/xfs_export.h, which says there are 5 possibilities:
	 * 0x00, 0x01, 0x02, 0x81, 0x82. Here FILEID_ROOT is also 0. The fileid_type identifies how the file within the filesystem is encoded. */
	printk(KERN_INFO "file id type is %d\n", fileid_type);

	if (fileid_type == FILEID_ROOT)
		dentry = dget(exp->ex_path.dentry);
	else {
		dentry = exportfs_decode_fh(exp->ex_path.mnt, fid,
				data_left, fileid_type,
				nfsd_acceptable, exp);
	}
	if (dentry == NULL)
		goto out;
	if (IS_ERR(dentry)) {
		if (PTR_ERR(dentry) != -EINVAL)
			error = nfserrno(PTR_ERR(dentry));
		goto out;
	}

	if (S_ISDIR(dentry->d_inode->i_mode) &&
			(dentry->d_flags & DCACHE_DISCONNECTED)) {
		printk("nfsd: find_fh_dentry returned a DISCONNECTED directory: %pd2\n",
				dentry);
	}

	fhp->fh_dentry = dentry;
	fhp->fh_export = exp;
	return 0;
out:
	exp_put(exp);
	return error;
}

/**
 * fh_verify - filehandle lookup
 * @rqstp: pointer to current rpc request
 * @fhp: filehandle to be verified
 *
 * Look up a dentry from the on-the-wire filehandle, check the client's
 * access to the export, and set the current task's credentials.
 *
 * Regardless of success or failure of fh_verify(), fh_put() should be
 * called on @fhp when the caller is finished with the filehandle.
 *
 * fh_verify() may be called multiple times on a given filehandle, for
 * example, when processing an NFSv4 compound.  The first call will look
 * up a dentry using the on-the-wire filehandle.  Subsequent calls will
 * skip the lookup and just perform the other checks and possibly change
 * the current task's credentials.
 *
 * @type specifies the type of object expected using one of the S_IF*
 * constants defined in include/linux/stat.h.  The caller may use zero
 * to indicate that it doesn't care, or a negative integer to indicate
 * that it expects something not of the given type.
 *
 */
__be32
fh_verify(struct svc_rqst *rqstp, struct svc_fh *fhp)
{
	__be32		error=0;

	printk(KERN_INFO "nfsd: fh_verify(%s)\n", SVCFH_fmt(fhp));

	if (!fhp->fh_dentry) {
		printk(KERN_INFO "fhp->fh_dentry is still NULL, let's set it to the right value.\n");
		/* setting fhp->fh_dentry and fhp->fh_export */
		error = nfsd_set_fh_dentry(rqstp, fhp);
	}
	return error;
}


/*
 * Compose a file handle for an NFS reply.
 *
 * Note that when first composed, the dentry may not yet have
 * an inode.  In this case a call to fh_update should be made
 * before the fh goes out on the wire ...
 */
static void _fh_update(struct svc_fh *fhp, struct svc_export *exp,
		struct dentry *dentry)
{
	if (dentry != exp->ex_path.dentry) {
		struct fid *fid = (struct fid *)
			(fhp->fh_handle.fh_fsid + fhp->fh_handle.fh_size/4 - 1);
		int maxsize = (fhp->fh_maxsize - fhp->fh_handle.fh_size)/4;
		int subtreecheck = 1;

		fhp->fh_handle.fh_fileid_type =
			exportfs_encode_fh(dentry, fid, &maxsize, subtreecheck);
		fhp->fh_handle.fh_size += maxsize * 4;
	} else {
		fhp->fh_handle.fh_fileid_type = FILEID_ROOT;
	}
}

static struct super_block *exp_sb(struct svc_export *exp)
{
	return exp->ex_path.dentry->d_inode->i_sb;
}

__be32
fh_compose(struct svc_fh *fhp, struct svc_export *exp, struct dentry *dentry,
	   struct svc_fh *ref_fh)
{
	/* ref_fh is a reference file handle.
	 * if it is non-null and for the same filesystem, then we should compose
	 * a filehandle which is of the same version, where possible.
	 * Currently, that means that if ref_fh->fh_handle.fh_version == 0xca
	 * Then create a 32byte filehandle using nfs_fhbase_old
	 *
	 */

	struct inode * inode = dentry->d_inode;
	dev_t ex_dev = exp_sb(exp)->s_dev;

	printk(KERN_INFO "nfsd: fh_compose(exp %02x:%02x/%ld %pd2, ino=%ld)\n",
		MAJOR(ex_dev), MINOR(ex_dev),
		(long) exp->ex_path.dentry->d_inode->i_ino,
		dentry,
		(inode ? inode->i_ino : 0));

	/* for NFSv3, this seems to be 1, for NFSv2, this is 0xca. */
	fhp->fh_handle.fh_version = 1;

	if (ref_fh == fhp)
		fh_put(ref_fh);

	if (fhp->fh_dentry) {
		printk(KERN_ERR "fh_compose: fh %pd2 not initialized!\n",
		       dentry);
	}
	if (fhp->fh_maxsize < NFS_FHSIZE)
		printk(KERN_ERR "fh_compose: called with maxsize %d! %pd2\n",
		       fhp->fh_maxsize,
		       dentry);

	fhp->fh_dentry = dget(dentry); /* our internal copy */
	fhp->fh_export = exp_get(exp);

	fhp->fh_handle.fh_size =
		key_len(fhp->fh_handle.fh_fsid_type) + 4;
	fhp->fh_handle.fh_auth_type = 0;

	/* setting fsid */
	mk_fsid(fhp->fh_handle.fh_fsid_type,
		fhp->fh_handle.fh_fsid,
		ex_dev,
		exp->ex_path.dentry->d_inode->i_ino,
		exp->ex_fsid);

	if (inode)
		_fh_update(fhp, exp, dentry);
	if (fhp->fh_handle.fh_fileid_type == FILEID_INVALID) {
		fh_put(fhp);
		return nfserr_opnotsupp;
	}

	return 0;
}

/*
 * Update file handle information after changing a dentry.
 * This is only called by nfsd_create, nfsd_create_v3 and nfsd_proc_create
 */
__be32
fh_update(struct svc_fh *fhp)
{
	struct dentry *dentry;

	if (!fhp->fh_dentry)
		goto out_bad;

	dentry = fhp->fh_dentry;
	if (!dentry->d_inode)
		goto out_negative;
	if (fhp->fh_handle.fh_fileid_type != FILEID_ROOT)
		return 0;

	_fh_update(fhp, fhp->fh_export, dentry);
	if (fhp->fh_handle.fh_fileid_type == FILEID_INVALID)
		return nfserr_opnotsupp;
	return 0;
out_bad:
	printk(KERN_ERR "fh_update: fh not verified!\n");
	return nfserr_serverfault;
out_negative:
	printk(KERN_ERR "fh_update: %pd2 still negative!\n",
		dentry);
	return nfserr_serverfault;
}

/*
 * Release a file handle.
 */
void
fh_put(struct svc_fh *fhp)
{
	struct dentry * dentry = fhp->fh_dentry;
	struct svc_export * exp = fhp->fh_export;
	if (dentry) {
		fhp->fh_dentry = NULL;
		dput(dentry);
	}
	fh_drop_write(fhp);
	if (exp) {
		exp_put(exp);
		fhp->fh_export = NULL;
	}
	return;
}

/*
 * Shorthand for dprintk()'s
 */
char * SVCFH_fmt(struct svc_fh *fhp)
{
	struct knfsd_fh *fh = &fhp->fh_handle;

	static char buf[80];
	sprintf(buf, "%d: %08x %08x %08x %08x %08x %08x",
		fh->fh_size,
		fh->fh_base.fh_pad[0],
		fh->fh_base.fh_pad[1],
		fh->fh_base.fh_pad[2],
		fh->fh_base.fh_pad[3],
		fh->fh_base.fh_pad[4],
		fh->fh_base.fh_pad[5]);
	return buf;
}
