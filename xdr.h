/*
 * XDR types for NFSv3 in nfsd.
 *
 * Copyright (C) 1996-1998, Olaf Kirch <okir@monad.swb.de>
 */

#ifndef _LINUX_NFSD_XDR3_H
#define _LINUX_NFSD_XDR3_H

#include <linux/vfs.h>
#include "nfsd.h"
#include "nfsfh.h"

struct nfsd_fhandle {
      struct svc_fh           fh;
};

struct nfsd3_sattrargs {
	struct svc_fh		fh;
	struct iattr		attrs;
	int			check_guard;
	time_t			guardtime;
};

struct nfsd3_diropargs {
	struct svc_fh		fh;
	char *			name;
	unsigned int		len;
};

struct nfsd3_accessargs {
	struct svc_fh		fh;
	unsigned int		access;
};

struct nfsd3_readargs {
	struct svc_fh		fh;
	__u64			offset;
	__u32			count;
	int			vlen;
};

struct nfsd3_writeargs {
	svc_fh			fh;
	__u64			offset;
	__u32			count;
	int			stable;
	__u32			len;
	struct kvec		first;
};

struct nfsd3_createargs {
	struct svc_fh		fh;
	char *			name;
	unsigned int		len;
	int			createmode;
	struct iattr		attrs;
	__be32 *		verf;
};

struct nfsd3_readdirargs {
	struct svc_fh		fh;
	__u64			cookie;
	__u32			dircount;
	__u32			count;
	__be32 *		verf;
	__be32 *		buffer;
};

struct nfsd3_attrstat {
	__be32			status;
	struct svc_fh		fh;
	struct kstat            stat;
};

/* LOOKUP, CREATE, MKDIR, SYMLINK, MKNOD */
struct nfsd3_diropres  {
	__be32			status;
	struct svc_fh		dirfh;
	struct svc_fh		fh;
};

struct nfsd3_accessres {
	__be32			status;
	struct svc_fh		fh;
	__u32			access;
	struct kstat		stat;
};

struct nfsd3_readres {
	__be32			status;
	struct svc_fh		fh;
	unsigned long		count;
	int			eof;
};

struct nfsd3_writeres {
	__be32			status;
	struct svc_fh		fh;
	unsigned long		count;
	int			committed;
};

struct nfsd3_readdirres {
	__be32			status;
	struct svc_fh		fh;
	/* Just to save kmalloc on every readdirplus entry (svc_fh is a
	 * little large for the stack): */
	struct svc_fh		scratch;
	int			count;
	__be32			verf[2];

	struct readdir_cd	common;
	__be32 *		buffer;
	int			buflen;
	__be32 *		offset;
	__be32 *		offset1;
	struct svc_rqst *	rqstp;

};

struct nfsd3_fsstatres {
	__be32			status;
	struct kstatfs		stats;
	__u32			invarsec;
};

struct nfsd3_fsinfores {
	__be32			status;
	__u32			f_rtmax;
	__u32			f_rtpref;
	__u32			f_rtmult;
	__u32			f_wtmax;
	__u32			f_wtpref;
	__u32			f_wtmult;
	__u32			f_dtpref;
	__u64			f_maxfilesize;
	__u32			f_properties;
};

/* dummy type for release */
struct nfsd3_fhandle_pair {
	__u32			dummy;
	struct svc_fh		fh1;
	struct svc_fh		fh2;
};

/*
 * Storage requirements for XDR arguments and results.
 */
union nfsd3_xdrstore {
	struct nfsd3_sattrargs		sattrargs;
	struct nfsd3_diropargs		diropargs;
	struct nfsd3_readargs		readargs;
	struct nfsd3_writeargs		writeargs;
	struct nfsd3_createargs		createargs;
	struct nfsd3_readdirargs	readdirargs;
	struct nfsd3_diropres 		diropres;
	struct nfsd3_accessres		accessres;
	struct nfsd3_readres		readres;
	struct nfsd3_writeres		writeres;
	struct nfsd3_readdirres		readdirres;
	struct nfsd3_fsstatres		fsstatres;
	struct nfsd3_fsinfores		fsinfores;
};

#define NFS3_SVC_XDRSIZE		sizeof(union nfsd3_xdrstore)

int nfs3svc_decode_fhandle(struct svc_rqst *, __be32 *, struct nfsd_fhandle *);
int nfs3svc_decode_sattrargs(struct svc_rqst *, __be32 *,
				struct nfsd3_sattrargs *);
int nfs3svc_decode_diropargs(struct svc_rqst *, __be32 *,
				struct nfsd3_diropargs *);
int nfs3svc_decode_accessargs(struct svc_rqst *, __be32 *,
				struct nfsd3_accessargs *);
int nfs3svc_decode_readargs(struct svc_rqst *, __be32 *,
				struct nfsd3_readargs *);
int nfs3svc_decode_writeargs(struct svc_rqst *, __be32 *,
				struct nfsd3_writeargs *);
int nfs3svc_decode_createargs(struct svc_rqst *, __be32 *,
				struct nfsd3_createargs *);
int nfs3svc_decode_mkdirargs(struct svc_rqst *, __be32 *,
				struct nfsd3_createargs *);
int nfs3svc_decode_readdirargs(struct svc_rqst *, __be32 *,
				struct nfsd3_readdirargs *);
int nfs3svc_decode_readdirplusargs(struct svc_rqst *, __be32 *,
				struct nfsd3_readdirargs *);
int nfs3svc_encode_voidres(struct svc_rqst *, __be32 *, void *);
int nfs3svc_encode_attrstat(struct svc_rqst *, __be32 *,
				struct nfsd3_attrstat *);
int nfs3svc_encode_wccstat(struct svc_rqst *, __be32 *,
				struct nfsd3_attrstat *);
int nfs3svc_encode_diropres(struct svc_rqst *, __be32 *,
				struct nfsd3_diropres *);
int nfs3svc_encode_accessres(struct svc_rqst *, __be32 *,
				struct nfsd3_accessres *);
int nfs3svc_encode_readres(struct svc_rqst *, __be32 *, struct nfsd3_readres *);
int nfs3svc_encode_writeres(struct svc_rqst *, __be32 *, struct nfsd3_writeres *);
int nfs3svc_encode_createres(struct svc_rqst *, __be32 *,
				struct nfsd3_diropres *);
int nfs3svc_encode_readdirres(struct svc_rqst *, __be32 *,
				struct nfsd3_readdirres *);
int nfs3svc_encode_fsstatres(struct svc_rqst *, __be32 *,
				struct nfsd3_fsstatres *);
int nfs3svc_encode_fsinfores(struct svc_rqst *, __be32 *,
				struct nfsd3_fsinfores *);
int nfs3svc_release_fhandle(struct svc_rqst *, __be32 *,
				struct nfsd3_attrstat *);
int nfs3svc_release_fhandle2(struct svc_rqst *, __be32 *,
				struct nfsd3_fhandle_pair *);
int nfs3svc_encode_entry(void *, const char *name,
				int namlen, loff_t offset, u64 ino,
				unsigned int);
int nfs3svc_encode_entry_plus(void *, const char *name,
				int namlen, loff_t offset, u64 ino,
				unsigned int);

#endif /* _LINUX_NFSD_XDR3_H */
