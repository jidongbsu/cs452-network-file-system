/*
 * XDR support for nfsd/protocol version 3.
 *
 * Copyright (C) 1995, 1996, 1997 Olaf Kirch <okir@monad.swb.de>
 *
 * 2003-08-09 Jamie Lokier: Use htonl() for nanoseconds, not htons()!
 */

#include <linux/namei.h>
#include <linux/sunrpc/svc_xprt.h>
#include "xdr3.h"
#include "netns.h"
#include "vfs.h"

/*
 * XDR functions for basic NFS types
 */

static __be32 *
decode_time3(__be32 *p, struct timespec *time)
{
	time->tv_sec = ntohl(*p++);
	time->tv_nsec = ntohl(*p++);
	return p;
}

/* the file handle is a variable-length opaque object, and according to the XDR standard, 
 * a variable-length opaque object usually has a prepended integer containing the byte count, i.e., the length.
 * this byte count itself is 4 bytes, the byte count does not include any pad bytes, but in XDR, any data item that is not
 * a multiple of 4 bytes in lengths must be padded with zero bytes. With these padding bytes, the whole object will now contain an integral number of 4-byte units. */
static __be32 *
decode_fh(__be32 *p, struct svc_fh *fhp)
{
	return p + XDR_QUADLEN(size);
}

static __be32 *
encode_fh(__be32 *p, struct svc_fh *fhp)
{
	unsigned int size = fhp->fh_handle.fh_size;
	*p++ = htonl(size);
	if (size) p[XDR_QUADLEN(size)-1]=0;
	memcpy(p, &fhp->fh_handle.fh_base, size);
	return p + XDR_QUADLEN(size);
}

/*
 * Decode a file name and make sure that the path contains
 * p represents the stream of data, and at the moment this function gets called, p is pointing to the beginning of the file name.
 * the file name's data type is string, and in XDR, a string is encoded as variable-length opaque data.
 */
static __be32 *
decode_filename(__be32 *p, char **namp, unsigned int *lenp)
{
        return p + XDR_QUADLEN(len);
}

static __be32 *
decode_sattr3(__be32 *p, struct iattr *iap)
{
	u32	tmp;

	iap->ia_valid = 0;

	if (*p++) {
		iap->ia_valid |= ATTR_MODE;
		iap->ia_mode = ntohl(*p++);
	}
	if (*p++) {
		iap->ia_uid = make_kuid(&init_user_ns, ntohl(*p++));
		if (uid_valid(iap->ia_uid))
			iap->ia_valid |= ATTR_UID;
	}
	if (*p++) {
		iap->ia_gid = make_kgid(&init_user_ns, ntohl(*p++));
		if (gid_valid(iap->ia_gid))
			iap->ia_valid |= ATTR_GID;
	}
	if (*p++) {
		u64	newsize;

		iap->ia_valid |= ATTR_SIZE;
		p = xdr_decode_hyper(p, &newsize);
		iap->ia_size = min_t(u64, newsize, NFS_OFFSET_MAX);
	}
	if ((tmp = ntohl(*p++)) == 1) {	/* set to server time */
		iap->ia_valid |= ATTR_ATIME;
	} else if (tmp == 2) {		/* set to client time */
		iap->ia_valid |= ATTR_ATIME | ATTR_ATIME_SET;
		iap->ia_atime.tv_sec = ntohl(*p++);
		iap->ia_atime.tv_nsec = ntohl(*p++);
	}
	if ((tmp = ntohl(*p++)) == 1) {	/* set to server time */
		iap->ia_valid |= ATTR_MTIME;
	} else if (tmp == 2) {		/* set to client time */
		iap->ia_valid |= ATTR_MTIME | ATTR_MTIME_SET;
		iap->ia_mtime.tv_sec = ntohl(*p++);
		iap->ia_mtime.tv_nsec = ntohl(*p++);
	}
	return p;
}

/* struct fattr3 is not actually defined in our code, but rather is defined in the NFS RFC. It has the following fields:
 * struct fattr3 {
 *	ftype3          type;
 * 	mode32          mode;
 * 	uint32          nlink;
 *	uid32           uid;
 *	gid32           gid;
 *	uint64          size;
 *	uint64          used; // disk space used: number of bytes of disk space that the file actually uses, which can be smaller than the size because the file may have holes or it may be larger due to fragmentation.
 *	specdata3       rdev;
 *	uint64          fsid;
 *	uint64          fileid; // file id basically is the inode number.
 *	nfstime3        atime;
 *	nfstime3        mtime;
 *	nfstime3        ctime;
 * };
 * */
static __be32 * encode_fattr3(struct svc_rqst *rqstp, __be32 *p, struct svc_fh *fhp, struct kstat *stat)
{
	return p;
}

/*
 * Encode post-operation attributes.
 * The inode may be NULL if the call failed because of a stale file
 * handle. In this case, no attributes are returned.
 */
static __be32 *
encode_post_op_attr(struct svc_rqst *rqstp, __be32 *p, struct svc_fh *fhp)
{
	struct dentry *dentry = fhp->fh_dentry;
	if (dentry && dentry->d_inode) {
		__be32 err;
		struct kstat stat;
        	struct path path = {.mnt = fhp->fh_export->ex_path.mnt, .dentry = fhp->fh_dentry};

		/* stat stores the attributes of the parent */
		err = nfserrno(vfs_getattr(&path, &stat));
		if (!err) {
			printk(KERN_INFO "vfs getattr success");
			*p++ = xdr_one;		/* attributes follow. post_op_attr starts with a boolean value, if it's true, then the next few bytes are the attributes. */
			/* get the last modified time of an inode, the second parameter here is a pointer to a timespec which will contain the last modified time */
			lease_get_mtime(dentry->d_inode, &stat.mtime);
			return encode_fattr3(rqstp, p, fhp, &stat);
		}
	}
	/* we should not be reaching here, if we reach here, it means something is wrong, and we just return no attributes. */
	*p++ = xdr_zero;
	return p;
}

/*
 * XDR decode functions
 */
int
nfs3svc_decode_fhandle(struct svc_rqst *rqstp, __be32 *p, struct nfsd_fhandle *args)
{
	p = decode_fh(p, &args->fh);
	if (!p)
		return 0;
	return xdr_argsize_check(rqstp, p);
}

int
nfs3svc_decode_sattrargs(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_sattrargs *args)
{
	p = decode_fh(p, &args->fh);
	if (!p)
		return 0;
	p = decode_sattr3(p, &args->attrs);

	if ((args->check_guard = ntohl(*p++)) != 0) { 
		struct timespec time; 
		p = decode_time3(p, &time);
		args->guardtime = time.tv_sec;
	}

	return xdr_argsize_check(rqstp, p);
}

/* the client who calls the REMOVE RPC call sends to the server two arguments:
 * 1: the file handle for the directory from which the entry is to be removed.
 * 2. the name of the entry to be removed.
 * both arguments are passed as a stream of data, which is pointed to by the pointer p here.
 * what we want to do is decode p, so as to get the two arguments, and save it in a struct nfsd3_diropargs pointed to by args. 
 * and struct nfsd3_diropargs is defined as:
 * struct nfsd3_diropargs {
 * 	struct svc_fh fh;
 * 	char * name;
 *	unsigned int len;
 * };
 * so basically when we decode p, we want the file handle to be stored in args->fh, and the name to be stored in args->name.
 * */
int nfs3svc_decode_diropargs(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_diropargs *args)
{
	if (!(p = decode_fh(p, &args->fh)) || !(p = decode_filename(p, &args->name, &args->len))){
		/* if p is NULL, something is wrong, return 0. */
		return 0;
	}

	//return xdr_argsize_check(rqstp, p);
	/* if everything goes right, return 1. */
	return 1;
}

int
nfs3svc_decode_accessargs(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_accessargs *args)
{
	p = decode_fh(p, &args->fh);
	if (!p)
		return 0;
	args->access = ntohl(*p++);

	return xdr_argsize_check(rqstp, p);
}

int
nfs3svc_decode_readargs(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_readargs *args)
{
	unsigned int len;
	int v;
	u32 max_blocksize = svc_max_payload(rqstp);

	p = decode_fh(p, &args->fh);
	if (!p)
		return 0;
	p = xdr_decode_hyper(p, &args->offset);

	args->count = ntohl(*p++);
	len = min(args->count, max_blocksize);

	/* set up the kvec */
	v=0;
	while (len > 0) {
		struct page *p = *(rqstp->rq_next_page++);

		rqstp->rq_vec[v].iov_base = page_address(p);
		rqstp->rq_vec[v].iov_len = min_t(unsigned int, len, PAGE_SIZE);
		len -= rqstp->rq_vec[v].iov_len;
		v++;
	}
	args->vlen = v;
	return xdr_argsize_check(rqstp, p);
}

int
nfs3svc_decode_writeargs(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_writeargs *args)
{
	unsigned int len, hdr, dlen;
	u32 max_blocksize = svc_max_payload(rqstp);
	struct kvec *head = rqstp->rq_arg.head;
	struct kvec *tail = rqstp->rq_arg.tail;

	p = decode_fh(p, &args->fh);
	if (!p)
		return 0;
	p = xdr_decode_hyper(p, &args->offset);

	args->count = ntohl(*p++);
	args->stable = ntohl(*p++);
	len = args->len = ntohl(*p++);
	if ((void *)p > head->iov_base + head->iov_len)
		return 0;
	/*
	 * The count must equal the amount of data passed.
	 */
	if (args->count != args->len)
		return 0;

	/*
	 * Check to make sure that we got the right number of
	 * bytes.
	 */
	hdr = (void*)p - head->iov_base;
	dlen = head->iov_len + rqstp->rq_arg.page_len + tail->iov_len - hdr;
	/*
	 * Round the length of the data which was specified up to
	 * the next multiple of XDR units and then compare that
	 * against the length which was actually received.
	 * Note that when RPCSEC/GSS (for example) is used, the
	 * data buffer can be padded so dlen might be larger
	 * than required.  It must never be smaller.
	 */
	if (dlen < XDR_QUADLEN(len)*4)
		return 0;

	if (args->count > max_blocksize) {
		args->count = max_blocksize;
		len = args->len = max_blocksize;
	}

	args->first.iov_base = (void *)p;
	args->first.iov_len = head->iov_len - hdr;
	return 1;
}

int
nfs3svc_decode_createargs(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_createargs *args)
{
	if (!(p = decode_fh(p, &args->fh))
	 || !(p = decode_filename(p, &args->name, &args->len)))
		return 0;

	switch (args->createmode = ntohl(*p++)) {
	case NFS3_CREATE_UNCHECKED:
	case NFS3_CREATE_GUARDED:
		p = decode_sattr3(p, &args->attrs);
		break;
	case NFS3_CREATE_EXCLUSIVE:
		args->verf = p;
		p += 2;
		break;
	default:
		return 0;
	}

	return xdr_argsize_check(rqstp, p);
}
int
nfs3svc_decode_mkdirargs(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_createargs *args)
{
	if (!(p = decode_fh(p, &args->fh)) ||
	    !(p = decode_filename(p, &args->name, &args->len)))
		return 0;
	p = decode_sattr3(p, &args->attrs);

	return xdr_argsize_check(rqstp, p);
}

int
nfs3svc_decode_readdirargs(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_readdirargs *args)
{
	p = decode_fh(p, &args->fh);
	if (!p)
		return 0;
	p = xdr_decode_hyper(p, &args->cookie);
	args->verf   = p; p += 2;
	args->dircount = ~0;
	args->count  = ntohl(*p++);
	args->count  = min_t(u32, args->count, PAGE_SIZE);
	args->buffer = page_address(*(rqstp->rq_next_page++));

	return xdr_argsize_check(rqstp, p);
}

int
nfs3svc_decode_readdirplusargs(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_readdirargs *args)
{
	int len;
	u32 max_blocksize = svc_max_payload(rqstp);

	p = decode_fh(p, &args->fh);
	if (!p)
		return 0;
	p = xdr_decode_hyper(p, &args->cookie);
	args->verf     = p; p += 2;
	args->dircount = ntohl(*p++);
	args->count    = ntohl(*p++);

	len = args->count = min(args->count, max_blocksize);
	while (len > 0) {
		struct page *p = *(rqstp->rq_next_page++);
		if (!args->buffer)
			args->buffer = page_address(p);
		len -= PAGE_SIZE;
	}

	return xdr_argsize_check(rqstp, p);
}

/*
 * XDR encode functions
 */
/*
 * There must be an encoding function for void results so svc_process
 * will work properly.
 */
int
nfs3svc_encode_voidres(struct svc_rqst *rqstp, __be32 *p, void *dummy)
{
	return xdr_ressize_check(rqstp, p);
}

/* GETATTR */
int
nfs3svc_encode_attrstat(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_attrstat *resp)
{
	if (resp->status == 0) {
		lease_get_mtime(resp->fh.fh_dentry->d_inode,
				&resp->stat.mtime);
		p = encode_fattr3(rqstp, p, &resp->fh, &resp->stat);
	}
	return xdr_ressize_check(rqstp, p);
}

/* SETATTR, REMOVE, RMDIR */
/* if something goes wrong, return 0. */
int
nfs3svc_encode_wccstat(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_attrstat *resp)
{
	// commenting this part because we do not want to return pre op attributes.
	//if (fhp->fh_pre_saved) {
                       /* if true, return these attributes before the operation: size, mtime, and ctime. */
                //        *p++ = xdr_one;
                //        p = xdr_encode_hyper(p, (u64) fhp->fh_pre_size);
                //        p = encode_time3(p, &fhp->fh_pre_mtime);
                //        p = encode_time3(p, &fhp->fh_pre_ctime);
                //} else {
			/* if false, do not return any pre op attributes. setting this to 0 means we do not return any pre op attributes. */
                        *p++ = xdr_zero;
                //}

	p = encode_post_op_attr(rqstp, p, &(resp->fh));
	/* this should return a non-zero value to indicate the check is okay, meaning p doesn't cross the buffer boundary. */
	return xdr_ressize_check(rqstp, p);
}

/* LOOKUP */
int
nfs3svc_encode_diropres(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_diropres *resp)
{
	if (resp->status == 0) {
		p = encode_fh(p, &resp->fh);
		p = encode_post_op_attr(rqstp, p, &resp->fh);
	}
	p = encode_post_op_attr(rqstp, p, &resp->dirfh);
	return xdr_ressize_check(rqstp, p);
}

/* ACCESS */
int
nfs3svc_encode_accessres(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_accessres *resp)
{
	p = encode_post_op_attr(rqstp, p, &resp->fh);
	if (resp->status == 0)
		*p++ = htonl(resp->access);
	return xdr_ressize_check(rqstp, p);
}

/* READ */
int
nfs3svc_encode_readres(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_readres *resp)
{
	p = encode_post_op_attr(rqstp, p, &resp->fh);
	if (resp->status == 0) {
		*p++ = htonl(resp->count);
		*p++ = htonl(resp->eof);
		*p++ = htonl(resp->count);	/* xdr opaque count */
		xdr_ressize_check(rqstp, p);
		/* now update rqstp->rq_res to reflect data as well */
		rqstp->rq_res.page_len = resp->count;
		if (resp->count & 3) {
			/* need to pad the tail */
			rqstp->rq_res.tail[0].iov_base = p;
			*p = 0;
			rqstp->rq_res.tail[0].iov_len = 4 - (resp->count & 3);
		}
		return 1;
	} else
		return xdr_ressize_check(rqstp, p);
}

/* WRITE */
int
nfs3svc_encode_writeres(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_writeres *resp)
{
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	/* because we do not provide pre-op attributs. */
	*p++ = xdr_zero;
	p = encode_post_op_attr(rqstp, p, &resp->fh);
	if (resp->status == 0) {
		*p++ = htonl(resp->count);
		*p++ = htonl(resp->committed);
		/* unique identifier, y2038 overflow can be ignored */
		*p++ = htonl((u32)nn->nfssvc_boot.tv_sec);
		*p++ = htonl(nn->nfssvc_boot.tv_nsec);
	}
	return xdr_ressize_check(rqstp, p);
}

/* CREATE, MKDIR, SYMLINK, MKNOD */
int
nfs3svc_encode_createres(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_diropres *resp)
{
	if (resp->status == 0) {
		*p++ = xdr_one;
		p = encode_fh(p, &resp->fh);
		p = encode_post_op_attr(rqstp, p, &resp->fh);
	}
	/* because we do not provide pre-op attributs. */
	*p++ = xdr_zero;
	p = encode_post_op_attr(rqstp, p, &resp->dirfh);
	return xdr_ressize_check(rqstp, p);
}

/* READDIR */
int
nfs3svc_encode_readdirres(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_readdirres *resp)
{
	p = encode_post_op_attr(rqstp, p, &resp->fh);

	if (resp->status == 0) {
		/* stupid readdir cookie */
		memcpy(p, resp->verf, 8); p += 2;
		xdr_ressize_check(rqstp, p);
		if (rqstp->rq_res.head[0].iov_len + (2<<2) > PAGE_SIZE)
			return 1; /*No room for trailer */
		rqstp->rq_res.page_len = (resp->count) << 2;

		/* add the 'tail' to the end of the 'head' page - page 0. */
		rqstp->rq_res.tail[0].iov_base = p;
		*p++ = 0;		/* no more entries */
		*p++ = htonl(resp->common.err == nfserr_eof);
		rqstp->rq_res.tail[0].iov_len = 2<<2;
		return 1;
	} else
		return xdr_ressize_check(rqstp, p);
}

static __be32 *
encode_entry_baggage(struct nfsd3_readdirres *cd, __be32 *p, const char *name,
	     int namlen, u64 ino)
{
	*p++ = xdr_one;				 /* mark entry present */
	p    = xdr_encode_hyper(p, ino);	 /* file id */
	p    = xdr_encode_array(p, name, namlen);/* name length & name */

	cd->offset = p;				/* remember pointer */
	p = xdr_encode_hyper(p, NFS_OFFSET_MAX);/* offset of next entry */

	return p;
}

static __be32
compose_entry_fh(struct nfsd3_readdirres *cd, struct svc_fh *fhp,
		 const char *name, int namlen, u64 ino)
{
	struct svc_export	*exp;
	struct dentry		*dparent, *dchild;
	__be32 rv = nfserr_noent;

	dparent = cd->fh.fh_dentry;
	exp  = cd->fh.fh_export;

	if (isdotent(name, namlen)) {
		if (namlen == 2) {
			dchild = dget_parent(dparent);
			/* filesystem root - cannot return filehandle for ".." */
			if (dchild == dparent)
				goto out;
		} else
			dchild = dget(dparent);
	} else
		dchild = lookup_one_len_unlocked(name, dparent, namlen);
	if (IS_ERR(dchild))
		return rv;
	if (d_mountpoint(dchild))
		goto out;
	if (!dchild->d_inode)
		goto out;
	if (dchild->d_inode->i_ino != ino)
		goto out;
	rv = fh_compose(fhp, exp, dchild, &cd->fh);
out:
	dput(dchild);
	return rv;
}

static __be32 *encode_entryplus_baggage(struct nfsd3_readdirres *cd, __be32 *p, const char *name, int namlen, u64 ino)
{
	struct svc_fh	*fh = &cd->scratch;
	__be32 err;

	fh_init(fh, NFS3_FHSIZE);
	err = compose_entry_fh(cd, fh, name, namlen, ino);
	if (err) {
		*p++ = 0;
		*p++ = 0;
		goto out;
	}
	p = encode_post_op_attr(cd->rqstp, p, fh);
	*p++ = xdr_one;			/* yes, a file handle follows */
	p = encode_fh(p, fh);
out:
	fh_put(fh);
	return p;
}

/*
 * Encode a directory entry. This one works for both normal readdir
 * and readdirplus.
 * The normal readdir reply requires 2 (fileid) + 1 (stringlen)
 * + string + 2 (cookie) + 1 (next) words, i.e. 6 + strlen.
 * 
 * The readdirplus baggage is 1+21 words for post_op_attr, plus the
 * file handle.
 */

#define NFS3_ENTRY_BAGGAGE	(2 + 1 + 2 + 1)
#define NFS3_ENTRYPLUS_BAGGAGE	(1 + 21 + 1 + (NFS3_FHSIZE >> 2))
static int
encode_entry(struct readdir_cd *ccd, const char *name, int namlen,
	     loff_t offset, u64 ino, unsigned int d_type, int plus)
{
	struct nfsd3_readdirres *cd = container_of(ccd, struct nfsd3_readdirres,
		       					common);
	__be32		*p = cd->buffer;
	caddr_t		curr_page_addr = NULL;
	struct page **	page;
	int		slen;		/* string (name) length */
	int		elen;		/* estimated entry length in words */
	int		num_entry_words = 0;	/* actual number of words */

	if (cd->offset) {
		u64 offset64 = offset;

		if (unlikely(cd->offset1)) {
			/* we ended up with offset on a page boundary */
			*cd->offset = htonl(offset64 >> 32);
			*cd->offset1 = htonl(offset64 & 0xffffffff);
			cd->offset1 = NULL;
		} else {
			xdr_encode_hyper(cd->offset, offset64);
		}
	}

	/*
	dprintk("encode_entry(%.*s @%ld%s)\n",
		namlen, name, (long) offset, plus? " plus" : "");
	 */

	/* truncate filename if too long */
	namlen = min(namlen, NFS3_MAXNAMLEN);

	slen = XDR_QUADLEN(namlen);
	elen = slen + NFS3_ENTRY_BAGGAGE
		+ (plus? NFS3_ENTRYPLUS_BAGGAGE : 0);

	if (cd->buflen < elen) {
		cd->common.err = nfserr_toosmall;
		return -EINVAL;
	}

	/* determine which page in rq_respages[] we are currently filling */
	for (page = cd->rqstp->rq_respages + 1;
				page < cd->rqstp->rq_next_page; page++) {
		curr_page_addr = page_address(*page);

		if (((caddr_t)cd->buffer >= curr_page_addr) &&
		    ((caddr_t)cd->buffer <  curr_page_addr + PAGE_SIZE))
			break;
	}

	if ((caddr_t)(cd->buffer + elen) < (curr_page_addr + PAGE_SIZE)) {
		/* encode entry in current page */

		p = encode_entry_baggage(cd, p, name, namlen, ino);

		if (plus)
			p = encode_entryplus_baggage(cd, p, name, namlen, ino);
		num_entry_words = p - cd->buffer;
	} else if (*(page+1) != NULL) {
		/* temporarily encode entry into next page, then move back to
		 * current and next page in rq_respages[] */
		__be32 *p1, *tmp;
		int len1, len2;

		/* grab next page for temporary storage of entry */
		p1 = tmp = page_address(*(page+1));

		p1 = encode_entry_baggage(cd, p1, name, namlen, ino);

		if (plus)
			p1 = encode_entryplus_baggage(cd, p1, name, namlen, ino);

		/* determine entry word length and lengths to go in pages */
		num_entry_words = p1 - tmp;
		len1 = curr_page_addr + PAGE_SIZE - (caddr_t)cd->buffer;
		if ((num_entry_words << 2) < len1) {
			/* the actual number of words in the entry is less
			 * than elen and can still fit in the current page
			 */
			memmove(p, tmp, num_entry_words << 2);
			p += num_entry_words;

			/* update offset */
			cd->offset = cd->buffer + (cd->offset - tmp);
		} else {
			unsigned int offset_r = (cd->offset - tmp) << 2;

			/* update pointer to offset location.
			 * This is a 64bit quantity, so we need to
			 * deal with 3 cases:
			 *  -	entirely in first page
			 *  -	entirely in second page
			 *  -	4 bytes in each page
			 */
			if (offset_r + 8 <= len1) {
				cd->offset = p + (cd->offset - tmp);
			} else if (offset_r >= len1) {
				cd->offset -= len1 >> 2;
			} else {
				/* sitting on the fence */
				BUG_ON(offset_r != len1 - 4);
				cd->offset = p + (cd->offset - tmp);
				cd->offset1 = tmp;
			}

			len2 = (num_entry_words << 2) - len1;

			/* move from temp page to current and next pages */
			memmove(p, tmp, len1);
			memmove(tmp, (caddr_t)tmp+len1, len2);

			p = tmp + (len2 >> 2);
		}
	}
	else {
		cd->common.err = nfserr_toosmall;
		return -EINVAL;
	}

	cd->buflen -= num_entry_words;
	cd->buffer = p;
	cd->common.err = nfs_ok;
	return 0;

}

int
nfs3svc_encode_entry(void *cd, const char *name,
		     int namlen, loff_t offset, u64 ino, unsigned int d_type)
{
	return encode_entry(cd, name, namlen, offset, ino, d_type, 0);
}

int
nfs3svc_encode_entry_plus(void *cd, const char *name,
			  int namlen, loff_t offset, u64 ino,
			  unsigned int d_type)
{
	return encode_entry(cd, name, namlen, offset, ino, d_type, 1);
}

/* FSSTAT */
int
nfs3svc_encode_fsstatres(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_fsstatres *resp)
{
	struct kstatfs	*s = &resp->stats;
	u64		bs = s->f_bsize;

	*p++ = xdr_zero;	/* no post_op_attr */

	if (resp->status == 0) {
		p = xdr_encode_hyper(p, bs * s->f_blocks);	/* total bytes */
		p = xdr_encode_hyper(p, bs * s->f_bfree);	/* free bytes */
		p = xdr_encode_hyper(p, bs * s->f_bavail);	/* user available bytes */
		p = xdr_encode_hyper(p, s->f_files);	/* total inodes */
		p = xdr_encode_hyper(p, s->f_ffree);	/* free inodes */
		p = xdr_encode_hyper(p, s->f_ffree);	/* user available inodes */
		*p++ = htonl(resp->invarsec);	/* mean unchanged time */
	}
	return xdr_ressize_check(rqstp, p);
}

/* FSINFO */
int
nfs3svc_encode_fsinfores(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_fsinfores *resp)
{
	*p++ = xdr_zero;	/* no post_op_attr */

	if (resp->status == 0) {
		*p++ = htonl(resp->f_rtmax);
		*p++ = htonl(resp->f_rtpref);
		*p++ = htonl(resp->f_rtmult);
		*p++ = htonl(resp->f_wtmax);
		*p++ = htonl(resp->f_wtpref);
		*p++ = htonl(resp->f_wtmult);
		*p++ = htonl(resp->f_dtpref);
		p = xdr_encode_hyper(p, resp->f_maxfilesize);
		*p++ = xdr_one;
		*p++ = xdr_zero;
		*p++ = htonl(resp->f_properties);
	}

	return xdr_ressize_check(rqstp, p);
}

/*
 * XDR release functions
 */
int
nfs3svc_release_fhandle(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_attrstat *resp)
{
	fh_put(&resp->fh);
	return 1;
}

int
nfs3svc_release_fhandle2(struct svc_rqst *rqstp, __be32 *p,
					struct nfsd3_fhandle_pair *resp)
{
	fh_put(&resp->fh1);
	fh_put(&resp->fh2);
	return 1;
}
