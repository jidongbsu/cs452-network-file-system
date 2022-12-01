/*
 * Copyright (C) 1995, 1996, 1997 Olaf Kirch <okir@monad.swb.de>
 *
 * This file describes the layout of the file handles as passed
 * over the wire.
 */
#ifndef _LINUX_NFSD_NFSFH_H
#define _LINUX_NFSD_NFSFH_H

#include <linux/crc32.h>
#include <linux/sunrpc/svc.h>
#include <uapi/linux/nfsd/nfsfh.h>

static inline __u32 ino_t_to_u32(ino_t ino)
{
	return (__u32) ino;
}

static inline ino_t u32_to_ino_t(__u32 uino)
{
	return (ino_t) uino;
}

/*
 * This is the internal representation of an NFS handle used in knfsd.
 * pre_mtime/post_version will be used to support wcc_attr's in NFSv3.
 */
typedef struct svc_fh {
	struct knfsd_fh		fh_handle;	/* FH data */
	int			fh_maxsize;	/* max size for fh_handle */
	struct dentry *		fh_dentry;	/* validated dentry */
	struct svc_export *	fh_export;	/* export pointer */

	bool			fh_want_write;	/* remount protection taken */
} svc_fh;

enum nfsd_fsid {
	FSID_DEV = 0,
	FSID_NUM,
	FSID_MAJOR_MINOR,
	FSID_ENCODE_DEV,
};

/*
 * This might look a little large to "inline" but in all calls except
 * one, 'vers' is constant so moste of the function disappears.
 *
 * In some cases the values are considered to be host endian and in
 * others, net endian. fsidv is always considered to be u32 as the
 * callers don't know which it will be. So we must use __force to keep
 * sparse from complaining. Since these values are opaque to the
 * client, that shouldn't be a problem.
 */
static inline void mk_fsid(int vers, u32 *fsidv, dev_t dev, ino_t ino,
			   u32 fsid)
{
	switch(vers) {
	case FSID_DEV:
		fsidv[0] = (__force __u32)htonl((MAJOR(dev)<<16) |
				 MINOR(dev));
		fsidv[1] = ino_t_to_u32(ino);
		break;
	case FSID_NUM:
		fsidv[0] = fsid;
		break;
	default: BUG();
	}
}

static inline int key_len(int type)
{
	switch(type) {
	case FSID_DEV:		return 8;
	case FSID_NUM: 		return 4;
	default: return 0;
	}
}

/*
 * Shorthand for dprintk()'s
 */
extern char * SVCFH_fmt(struct svc_fh *fhp);

/*
 * Function prototypes
 */
__be32	fh_verify(struct svc_rqst *, struct svc_fh *);
__be32	fh_compose(struct svc_fh *, struct svc_export *, struct dentry *, struct svc_fh *);
__be32	fh_update(struct svc_fh *);
void	fh_put(struct svc_fh *);

static __inline__ struct svc_fh *
fh_copy(struct svc_fh *dst, struct svc_fh *src)
{
	WARN_ON(src->fh_dentry);
			
	*dst = *src;
	return dst;
}

static inline void
fh_copy_shallow(struct knfsd_fh *dst, struct knfsd_fh *src)
{
	dst->fh_size = src->fh_size;
	memcpy(&dst->fh_base, &src->fh_base, src->fh_size);
}

static __inline__ struct svc_fh *
fh_init(struct svc_fh *fhp, int maxsize)
{
	memset(fhp, 0, sizeof(*fhp));
	fhp->fh_maxsize = maxsize;
	return fhp;
}

#endif /* _LINUX_NFSD_NFSFH_H */
