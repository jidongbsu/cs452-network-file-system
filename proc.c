/*
 * Process version 3 NFS requests.
 *
 * Copyright (C) 1996, 1997, 1998 Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/magic.h>

#include "xdr.h"
#include "vfs.h"

#define RETURN_STATUS(st)	{ resp->status = (st); return (st); }

/*
 * NULL call.
 */
static __be32
nfsd3_proc_null(struct svc_rqst *rqstp, void *argp, void *resp)
{
	return nfs_ok;
}

/*
 * Get a file's attributes
 */
static __be32
nfsd3_proc_getattr(struct svc_rqst *rqstp, struct nfsd_fhandle  *argp,
					   struct nfsd3_attrstat *resp)
{
	__be32	nfserr;

	printk(KERN_INFO "nfsd: GETATTR(3)  %s\n",
		SVCFH_fmt(&argp->fh));

	fh_copy(&resp->fh, &argp->fh);
	nfserr = fh_verify(rqstp, &resp->fh);
	if (nfserr)
		RETURN_STATUS(nfserr);

	nfserr = fh_getattr(&resp->fh, &resp->stat);

	RETURN_STATUS(nfserr);
}

/*
 * Set a file's attributes
 */
static __be32
nfsd3_proc_setattr(struct svc_rqst *rqstp, struct nfsd3_sattrargs *argp,
					   struct nfsd3_attrstat  *resp)
{
	__be32	nfserr;

	printk(KERN_INFO "nfsd: SETATTR(3)  %s\n",
				SVCFH_fmt(&argp->fh));

	fh_copy(&resp->fh, &argp->fh);
	nfserr = nfsd_setattr(rqstp, &resp->fh, &argp->attrs,
			      argp->check_guard, argp->guardtime);
	RETURN_STATUS(nfserr);
}

/*
 * Look up a path name component
 */
static __be32
nfsd3_proc_lookup(struct svc_rqst *rqstp, struct nfsd3_diropargs *argp,
					  struct nfsd3_diropres  *resp)
{
	__be32	nfserr;

	printk(KERN_INFO "nfsd: LOOKUP(3)   %s %.*s\n",
				SVCFH_fmt(&argp->fh),
				argp->len,
				argp->name);

	fh_copy(&resp->dirfh, &argp->fh);
	fh_init(&resp->fh, NFS3_FHSIZE);

	nfserr = nfsd_lookup(rqstp, &resp->dirfh,
				    argp->name,
				    argp->len,
				    &resp->fh);
	RETURN_STATUS(nfserr);
}

/*
 * Check file access
 */
static __be32
nfsd3_proc_access(struct svc_rqst *rqstp, struct nfsd3_accessargs *argp,
					  struct nfsd3_accessres *resp)
{
	__be32	nfserr;

	printk(KERN_INFO "nfsd: ACCESS(3)   %s 0x%x\n",
				SVCFH_fmt(&argp->fh),
				argp->access);

	fh_copy(&resp->fh, &argp->fh);
	resp->access = argp->access;
	nfserr = 0;
	RETURN_STATUS(nfserr);
}

/*
 * Read a portion of a file.
 */
static __be32
nfsd3_proc_read(struct svc_rqst *rqstp, struct nfsd3_readargs *argp,
				        struct nfsd3_readres  *resp)
{
	__be32	nfserr;
	u32	max_blocksize = svc_max_payload(rqstp);
	unsigned long cnt = min(argp->count, max_blocksize);

	printk(KERN_INFO "nfsd: READ(3) %s %lu bytes at %Lu\n",
				SVCFH_fmt(&argp->fh),
				(unsigned long) argp->count,
				(unsigned long long) argp->offset);

	/* Obtain buffer pointer for payload.
	 * 1 (status) + 22 (post_op_attr) + 1 (count) + 1 (eof)
	 * + 1 (xdr opaque byte count) = 26
	 */
	resp->count = cnt;
	svc_reserve_auth(rqstp, ((1 + NFS3_POST_OP_ATTR_WORDS + 3)<<2) + resp->count +4);

	fh_copy(&resp->fh, &argp->fh);
	nfserr = nfsd_read(rqstp, &resp->fh,
				  argp->offset,
			   	  rqstp->rq_vec, argp->vlen,
				  &resp->count);
	if (nfserr == 0) {
		struct inode	*inode = resp->fh.fh_dentry->d_inode;
		resp->eof = nfsd_eof_on_read(cnt, resp->count, argp->offset,
							inode->i_size);
	}

	RETURN_STATUS(nfserr);
}

/*
 * Write data to a file
 */
static __be32
nfsd3_proc_write(struct svc_rqst *rqstp, struct nfsd3_writeargs *argp,
					 struct nfsd3_writeres  *resp)
{
	__be32	nfserr;
	unsigned long cnt = argp->len;
	unsigned int nvecs;

	printk(KERN_INFO "nfsd: WRITE(3)    %s %d bytes at %Lu%s\n",
				SVCFH_fmt(&argp->fh),
				argp->len,
				(unsigned long long) argp->offset,
				argp->stable? " stable" : "");

	fh_copy(&resp->fh, &argp->fh);
	resp->committed = argp->stable;
	nvecs = svc_fill_write_vector(rqstp, &argp->first, cnt);
	if (!nvecs)
		RETURN_STATUS(nfserr_io);
	nfserr = nfsd_write(rqstp, &resp->fh, argp->offset,
			    rqstp->rq_vec, nvecs, &cnt,
			    resp->committed);
	resp->count = cnt;
	RETURN_STATUS(nfserr);
}

/*
 * With NFSv3, CREATE processing is a lot easier than with NFSv2.
 * At least in theory; we'll see how it fares in practice when the
 * first reports about SunOS compatibility problems start to pour in...
 */
static __be32
nfsd3_proc_create(struct svc_rqst *rqstp, struct nfsd3_createargs *argp,
					  struct nfsd3_diropres   *resp)
{
	svc_fh		*dirfhp, *newfhp = NULL;
	struct iattr	*attr;
	__be32		nfserr;

	printk(KERN_INFO "nfsd: CREATE(3)   %s %.*s\n",
				SVCFH_fmt(&argp->fh),
				argp->len,
				argp->name);

	dirfhp = fh_copy(&resp->dirfh, &argp->fh);
	newfhp = fh_init(&resp->fh, NFS3_FHSIZE);
	attr   = &argp->attrs;

	/* Unfudge the mode bits */
	attr->ia_mode &= ~S_IFMT;
	if (!(attr->ia_valid & ATTR_MODE)) { 
		attr->ia_valid |= ATTR_MODE;
		attr->ia_mode = S_IFREG;
	} else {
		attr->ia_mode = (attr->ia_mode & ~S_IFMT) | S_IFREG;
	}

	/* Now create the file and set attributes */
	nfserr = do_nfsd_create(rqstp, dirfhp, argp->name, argp->len,
				attr, newfhp,
				argp->createmode, (u32 *)argp->verf, NULL, NULL);

	RETURN_STATUS(nfserr);
}

/*
 * Make directory. This operation is not idempotent.
 */
static __be32
nfsd3_proc_mkdir(struct svc_rqst *rqstp, struct nfsd3_createargs *argp,
					 struct nfsd3_diropres   *resp)
{
	__be32	nfserr;

	printk(KERN_INFO "nfsd: MKDIR(3)    %s %.*s\n",
				SVCFH_fmt(&argp->fh),
				argp->len,
				argp->name);

	argp->attrs.ia_valid &= ~ATTR_SIZE;
	fh_copy(&resp->dirfh, &argp->fh);
	fh_init(&resp->fh, NFS3_FHSIZE);
	nfserr = nfsd_create(rqstp, &resp->dirfh, argp->name, argp->len,
				    &argp->attrs, S_IFDIR, 0, &resp->fh);
	RETURN_STATUS(nfserr);
}

/*
 * Remove file/fifo/socket etc. argp->name is the name of the file which we want to delete, argp->len is the length of that file name.
 */
static __be32
nfsd3_proc_remove(struct svc_rqst *rqstp, struct nfsd3_diropargs *argp,
					  struct nfsd3_attrstat  *resp)
{
	__be32	nfserr;

	printk(KERN_INFO "nfsd: REMOVE(3)   %s %.*s\n",
				SVCFH_fmt(&argp->fh),
				argp->len,
				argp->name);

	/* after decoding, the arguments are now in argp, and now we copy &argp->fh to &resp->fh, we use resp->fh to store the parent's file handle. */
	fh_copy(&resp->fh, &argp->fh);
	/* unlink. -S_IFDIR means file must not be a directory, we can delete regular files, but we do not delete directories with this remove() function. */
	nfserr = nfsd_unlink(rqstp, &resp->fh, -S_IFDIR, argp->name, argp->len);
	RETURN_STATUS(nfserr);
}

/*
 * Remove a directory
 */
static __be32
nfsd3_proc_rmdir(struct svc_rqst *rqstp, struct nfsd3_diropargs *argp,
					 struct nfsd3_attrstat  *resp)
{
	__be32	nfserr;

	printk(KERN_INFO "nfsd: RMDIR(3)    %s %.*s\n",
				SVCFH_fmt(&argp->fh),
				argp->len,
				argp->name);

	fh_copy(&resp->fh, &argp->fh);
	nfserr = nfsd_unlink(rqstp, &resp->fh, S_IFDIR, argp->name, argp->len);
	RETURN_STATUS(nfserr);
}

/*
 * Read a portion of a directory.
 */
static __be32
nfsd3_proc_readdir(struct svc_rqst *rqstp, struct nfsd3_readdirargs *argp,
					   struct nfsd3_readdirres  *resp)
{
	__be32		nfserr;
	int		count;

	printk(KERN_INFO "nfsd: READDIR(3)  %s %d bytes at %d\n",
				SVCFH_fmt(&argp->fh),
				argp->count, (u32) argp->cookie);

	/* Make sure we've room for the NULL ptr & eof flag, and shrink to
	 * client read size */
	count = (argp->count >> 2) - 2;

	/* Read directory and encode entries on the fly */
	fh_copy(&resp->fh, &argp->fh);

	resp->buflen = count;
	resp->common.err = nfs_ok;
	resp->buffer = argp->buffer;
	resp->rqstp = rqstp;
	nfserr = nfsd_readdir(rqstp, &resp->fh, (loff_t*) &argp->cookie, 
					&resp->common, nfs3svc_encode_entry);
	memcpy(resp->verf, argp->verf, 8);
	resp->count = resp->buffer - argp->buffer;
	if (resp->offset)
		xdr_encode_hyper(resp->offset, argp->cookie);

	RETURN_STATUS(nfserr);
}

/*
 * Read a portion of a directory, including file handles and attrs.
 * For now, we choose to ignore the dircount parameter.
 */
static __be32
nfsd3_proc_readdirplus(struct svc_rqst *rqstp, struct nfsd3_readdirargs *argp,
					       struct nfsd3_readdirres  *resp)
{
	__be32	nfserr;
	int	count = 0;
	loff_t	offset;
	struct page **p;
	caddr_t	page_addr = NULL;

	printk(KERN_INFO "nfsd: READDIR+(3) %s %d bytes at %d\n",
				SVCFH_fmt(&argp->fh),
				argp->count, (u32) argp->cookie);

	/* Convert byte count to number of words (i.e. >> 2),
	 * and reserve room for the NULL ptr & eof flag (-2 words) */
	resp->count = (argp->count >> 2) - 2;

	/* Read directory and encode entries on the fly */
	fh_copy(&resp->fh, &argp->fh);

	resp->common.err = nfs_ok;
	resp->buffer = argp->buffer;
	resp->buflen = resp->count;
	resp->rqstp = rqstp;
	offset = argp->cookie;

	nfserr = fh_verify(rqstp, &resp->fh);
	if (nfserr)
		RETURN_STATUS(nfserr);

	nfserr = nfsd_readdir(rqstp, &resp->fh,
				     &offset,
				     &resp->common,
				     nfs3svc_encode_entry_plus);
	memcpy(resp->verf, argp->verf, 8);
	for (p = rqstp->rq_respages + 1; p < rqstp->rq_next_page; p++) {
		page_addr = page_address(*p);

		if (((caddr_t)resp->buffer >= page_addr) &&
		    ((caddr_t)resp->buffer < page_addr + PAGE_SIZE)) {
			count += (caddr_t)resp->buffer - page_addr;
			break;
		}
		count += PAGE_SIZE;
	}
	resp->count = count >> 2;
	if (resp->offset) {
		if (unlikely(resp->offset1)) {
			/* we ended up with offset on a page boundary */
			*resp->offset = htonl(offset >> 32);
			*resp->offset1 = htonl(offset & 0xffffffff);
			resp->offset1 = NULL;
		} else {
			xdr_encode_hyper(resp->offset, offset);
		}
	}

	RETURN_STATUS(nfserr);
}

/*
 * get file system stats. this will get called when the client runs the "df" command.
 */
static __be32
nfsd3_proc_fsstat(struct svc_rqst * rqstp, struct nfsd_fhandle    *argp,
					   struct nfsd3_fsstatres *resp)
{
	__be32	nfserr;

	printk(KERN_INFO "nfsd: FSSTAT(3)   %s\n",
				SVCFH_fmt(&argp->fh));

	nfserr = nfsd_statfs(rqstp, &argp->fh, &resp->stats, 0);
	fh_put(&argp->fh);
	RETURN_STATUS(nfserr);
}

/*
 * get file system info. this will get called when the client mounts this nfs file system.
 */
static __be32
nfsd3_proc_fsinfo(struct svc_rqst * rqstp, struct nfsd_fhandle    *argp,
					   struct nfsd3_fsinfores *resp)
{
	__be32	nfserr;
	u32	max_blocksize = svc_max_payload(rqstp);

	printk(KERN_INFO "nfsd: FSINFO(3)   %s\n",
				SVCFH_fmt(&argp->fh));

	resp->f_rtmax  = max_blocksize;
	resp->f_rtpref = max_blocksize;
	resp->f_rtmult = PAGE_SIZE;
	resp->f_wtmax  = max_blocksize;
	resp->f_wtpref = max_blocksize;
	resp->f_wtmult = PAGE_SIZE;
	resp->f_dtpref = PAGE_SIZE;
	resp->f_maxfilesize = ~(u32) 0;
	resp->f_properties = NFS3_FSF_DEFAULT;

	nfserr = fh_verify(rqstp, &argp->fh);

	/* Check special features of the file system. May request
	 * different read/write sizes for file systems known to have
	 * problems with large blocks */
	if (nfserr == 0) {
		struct super_block *sb = argp->fh.fh_dentry->d_inode->i_sb;

		/* Note that we don't care for remote fs's here */
		if (sb->s_magic == MSDOS_SUPER_MAGIC) {
			resp->f_properties = NFS3_FSF_BILLYBOY;
		}
		resp->f_maxfilesize = sb->s_maxbytes;
	}

	fh_put(&argp->fh);
	RETURN_STATUS(nfserr);
}

/*
 * NFSv3 Server procedures.
 * Only the results of non-idempotent operations are cached.
 */
#define nfs3svc_decode_fhandleargs	nfs3svc_decode_fhandle
#define nfs3svc_encode_attrstatres	nfs3svc_encode_attrstat
#define nfs3svc_encode_wccstatres	nfs3svc_encode_wccstat
#define nfsd3_mkdirargs			nfsd3_createargs
#define nfsd3_readdirplusargs		nfsd3_readdirargs
#define nfsd3_fhandleargs		nfsd_fhandle
#define nfsd3_fhandleres		nfsd3_attrstat
#define nfsd3_attrstatres		nfsd3_attrstat
#define nfsd3_wccstatres		nfsd3_attrstat
#define nfsd3_createres			nfsd3_diropres
#define nfsd3_voidres			nfsd3_voidargs
struct nfsd3_voidargs { int dummy; };

#define ST 1		/* status*/
#define FH 17		/* filehandle with length */
#define AT 21		/* attributes */
#define pAT (1+AT)	/* post attributes - conditional */
#define WC (7+pAT)	/* WCC attributes */

static struct svc_procedure		nfsd_procedures3[22] = {
	[NFS3PROC_NULL] = {
		.pc_func = (svc_procfunc) nfsd3_proc_null,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_voidres,
		.pc_argsize = sizeof(struct nfsd3_voidargs),
		.pc_ressize = sizeof(struct nfsd3_voidres),
		.pc_xdrressize = ST,
	},
	[NFS3PROC_GETATTR] = {
		.pc_func = (svc_procfunc) nfsd3_proc_getattr,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_fhandleargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_attrstatres,
		.pc_release = (kxdrproc_t) nfs3svc_release_fhandle,
		.pc_argsize = sizeof(struct nfsd3_fhandleargs),
		.pc_ressize = sizeof(struct nfsd3_attrstatres),
		.pc_xdrressize = ST+AT,
	},
	[NFS3PROC_SETATTR] = {
		.pc_func = (svc_procfunc) nfsd3_proc_setattr,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_sattrargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_wccstatres,
		.pc_release = (kxdrproc_t) nfs3svc_release_fhandle,
		.pc_argsize = sizeof(struct nfsd3_sattrargs),
		.pc_ressize = sizeof(struct nfsd3_wccstatres),
		.pc_xdrressize = ST+WC,
	},
	[NFS3PROC_LOOKUP] = {
		.pc_func = (svc_procfunc) nfsd3_proc_lookup,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_diropargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_diropres,
		.pc_release = (kxdrproc_t) nfs3svc_release_fhandle2,
		.pc_argsize = sizeof(struct nfsd3_diropargs),
		.pc_ressize = sizeof(struct nfsd3_diropres),
		.pc_xdrressize = ST+FH+pAT+pAT,
	},
	[NFS3PROC_ACCESS] = {
		.pc_func = (svc_procfunc) nfsd3_proc_access,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_accessargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_accessres,
		.pc_release = (kxdrproc_t) nfs3svc_release_fhandle,
		.pc_argsize = sizeof(struct nfsd3_accessargs),
		.pc_ressize = sizeof(struct nfsd3_accessres),
		.pc_xdrressize = ST+pAT+1,
	},
	[NFS3PROC_READ] = {
		.pc_func = (svc_procfunc) nfsd3_proc_read,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_readargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_readres,
		.pc_release = (kxdrproc_t) nfs3svc_release_fhandle,
		.pc_argsize = sizeof(struct nfsd3_readargs),
		.pc_ressize = sizeof(struct nfsd3_readres),
		.pc_xdrressize = ST+pAT+4+NFSSVC_MAXBLKSIZE/4,
	},
	[NFS3PROC_WRITE] = {
		.pc_func = (svc_procfunc) nfsd3_proc_write,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_writeargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_writeres,
		.pc_release = (kxdrproc_t) nfs3svc_release_fhandle,
		.pc_argsize = sizeof(struct nfsd3_writeargs),
		.pc_ressize = sizeof(struct nfsd3_writeres),
		.pc_xdrressize = ST+WC+4,
	},
	[NFS3PROC_CREATE] = {
		.pc_func = (svc_procfunc) nfsd3_proc_create,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_createargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_createres,
		.pc_release = (kxdrproc_t) nfs3svc_release_fhandle2,
		.pc_argsize = sizeof(struct nfsd3_createargs),
		.pc_ressize = sizeof(struct nfsd3_createres),
		.pc_xdrressize = ST+(1+FH+pAT)+WC,
	},
	[NFS3PROC_MKDIR] = {
		.pc_func = (svc_procfunc) nfsd3_proc_mkdir,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_mkdirargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_createres,
		.pc_release = (kxdrproc_t) nfs3svc_release_fhandle2,
		.pc_argsize = sizeof(struct nfsd3_mkdirargs),
		.pc_ressize = sizeof(struct nfsd3_createres),
		.pc_xdrressize = ST+(1+FH+pAT)+WC,
	},
	[NFS3PROC_REMOVE] = {
		.pc_func = (svc_procfunc) nfsd3_proc_remove,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_diropargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_wccstatres,
		.pc_release = (kxdrproc_t) nfs3svc_release_fhandle,
		.pc_argsize = sizeof(struct nfsd3_diropargs),
		.pc_ressize = sizeof(struct nfsd3_wccstatres),
		.pc_xdrressize = ST+WC,
	},
	[NFS3PROC_RMDIR] = {
		.pc_func = (svc_procfunc) nfsd3_proc_rmdir,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_diropargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_wccstatres,
		.pc_release = (kxdrproc_t) nfs3svc_release_fhandle,
		.pc_argsize = sizeof(struct nfsd3_diropargs),
		.pc_ressize = sizeof(struct nfsd3_wccstatres),
		.pc_xdrressize = ST+WC,
	},
	[NFS3PROC_READDIR] = {
		.pc_func = (svc_procfunc) nfsd3_proc_readdir,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_readdirargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_readdirres,
		.pc_release = (kxdrproc_t) nfs3svc_release_fhandle,
		.pc_argsize = sizeof(struct nfsd3_readdirargs),
		.pc_ressize = sizeof(struct nfsd3_readdirres),
	},
	[NFS3PROC_READDIRPLUS] = {
		.pc_func = (svc_procfunc) nfsd3_proc_readdirplus,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_readdirplusargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_readdirres,
		.pc_release = (kxdrproc_t) nfs3svc_release_fhandle,
		.pc_argsize = sizeof(struct nfsd3_readdirplusargs),
		.pc_ressize = sizeof(struct nfsd3_readdirres),
	},
	[NFS3PROC_FSSTAT] = {
		.pc_func = (svc_procfunc) nfsd3_proc_fsstat,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_fhandleargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_fsstatres,
		.pc_argsize = sizeof(struct nfsd3_fhandleargs),
		.pc_ressize = sizeof(struct nfsd3_fsstatres),
		.pc_xdrressize = ST+pAT+2*6+1,
	},
	[NFS3PROC_FSINFO] = {
		.pc_func = (svc_procfunc) nfsd3_proc_fsinfo,
		.pc_decode = (kxdrproc_t) nfs3svc_decode_fhandleargs,
		.pc_encode = (kxdrproc_t) nfs3svc_encode_fsinfores,
		.pc_argsize = sizeof(struct nfsd3_fhandleargs),
		.pc_ressize = sizeof(struct nfsd3_fsinfores),
		.pc_xdrressize = ST+pAT+12,
	},
};

struct svc_version	nfsd_version3 = {
		.vs_vers	= 3,
		.vs_nproc	= 22,
		.vs_proc	= nfsd_procedures3,
		.vs_dispatch	= nfsd_dispatch,
		.vs_xdrsize	= NFS3_SVC_XDRSIZE,
};

/*
 * Map errnos to NFS errnos.
 */
__be32
nfserrno (int errno)
{
	static struct {
		__be32	nfserr;
		int	syserr;
	} nfs_errtbl[] = {
		{ nfs_ok, 0 },
		{ nfserr_perm, -EPERM },
		{ nfserr_noent, -ENOENT },
		{ nfserr_io, -EIO },
		{ nfserr_nxio, -ENXIO },
		{ nfserr_fbig, -E2BIG },
		{ nfserr_acces, -EACCES },
		{ nfserr_exist, -EEXIST },
		{ nfserr_xdev, -EXDEV },
		{ nfserr_mlink, -EMLINK },
		{ nfserr_nodev, -ENODEV },
		{ nfserr_notdir, -ENOTDIR },
		{ nfserr_isdir, -EISDIR },
		{ nfserr_inval, -EINVAL },
		{ nfserr_fbig, -EFBIG },
		{ nfserr_nospc, -ENOSPC },
		{ nfserr_rofs, -EROFS },
		{ nfserr_mlink, -EMLINK },
		{ nfserr_nametoolong, -ENAMETOOLONG },
		{ nfserr_notempty, -ENOTEMPTY },
#ifdef EDQUOT
		{ nfserr_dquot, -EDQUOT },
#endif
		{ nfserr_stale, -ESTALE },
		{ nfserr_jukebox, -ETIMEDOUT },
		{ nfserr_jukebox, -ERESTARTSYS },
		{ nfserr_jukebox, -EAGAIN },
		{ nfserr_jukebox, -EWOULDBLOCK },
		{ nfserr_jukebox, -ENOMEM },
		{ nfserr_io, -ETXTBSY },
		{ nfserr_notsupp, -EOPNOTSUPP },
		{ nfserr_toosmall, -ETOOSMALL },
		{ nfserr_serverfault, -ESERVERFAULT },
		{ nfserr_serverfault, -ENFILE },
	};
	int	i;

	for (i = 0; i < ARRAY_SIZE(nfs_errtbl); i++) {
		if (nfs_errtbl[i].syserr == errno)
			return nfs_errtbl[i].nfserr;
	}
	WARN_ONCE(1, "nfsd: non-standard errno: %d\n", errno);
	return nfserr_io;
}

