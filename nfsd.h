/*
 * Hodge-podge collection of knfsd-related stuff.
 * I will sort this out later.
 *
 * Copyright (C) 1995-1997 Olaf Kirch <okir@monad.swb.de>
 */

#ifndef LINUX_NFSD_NFSD_H
#define LINUX_NFSD_NFSD_H

#include <linux/types.h>
#include <linux/mount.h>

#include <linux/nfs.h>
#include <linux/nfs2.h>
#include <linux/nfs3.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/msg_prot.h>

#include <uapi/linux/nfsd/debug.h>

#include "export.h"

/*
 * nfsd version
 */
#define NFSD_SUPPORTED_MINOR_VERSION	2
/*
 * Maximum blocksizes supported by daemon under various circumstances.
 */
#define NFSSVC_MAXBLKSIZE       RPCSVC_MAXPAYLOAD
/* NFSv2 is limited by the protocol specification, see RFC 1094 */
#define NFSSVC_MAXBLKSIZE_V2    (8*1024)


/*
 * Largest number of bytes we need to allocate for an NFS
 * call or reply.  Used to control buffer sizes.  We use
 * the length of v3 WRITE, READDIR and READDIR replies
 * which are an RPC header, up to 26 XDR units of reply
 * data, and some page data.
 *
 * Note that accuracy here doesn't matter too much as the
 * size is rounded up to a page size when allocating space.
 */
#define NFSD_BUFSIZE            ((RPC_MAX_HEADER_WITH_AUTH+26)*XDR_UNIT + NFSSVC_MAXBLKSIZE)

struct readdir_cd {
	__be32			err;	/* 0, nfserr, or nfserr_eof */
};


extern struct svc_program	nfsd_program;
extern struct svc_version	nfsd_version3;
/*
 * Function prototypes.
 */
int		nfsd_svc(int nrservs, struct net *net);
int		nfsd_dispatch(struct svc_rqst *rqstp, __be32 *statp);

int		nfsd_nrpools(struct net *);

void		nfsd_destroy(struct net *net);

enum vers_op {NFSD_SET, NFSD_CLEAR, NFSD_TEST, NFSD_AVAIL };
int nfsd_vers(int vers, enum vers_op change);
void nfsd_reset_versions(void);
int nfsd_create_serv(struct net *net);

extern int nfsd_max_blksize;

/*
 * These macros provide pre-xdr'ed values for faster operation.
 */
#define	nfs_ok			cpu_to_be32(NFS_OK)
#define	nfserr_perm		cpu_to_be32(NFSERR_PERM)
#define	nfserr_noent		cpu_to_be32(NFSERR_NOENT)
#define	nfserr_io		cpu_to_be32(NFSERR_IO)
#define	nfserr_nxio		cpu_to_be32(NFSERR_NXIO)
#define	nfserr_eagain		cpu_to_be32(NFSERR_EAGAIN)
#define	nfserr_acces		cpu_to_be32(NFSERR_ACCES)
#define	nfserr_exist		cpu_to_be32(NFSERR_EXIST)
#define	nfserr_xdev		cpu_to_be32(NFSERR_XDEV)
#define	nfserr_nodev		cpu_to_be32(NFSERR_NODEV)
#define	nfserr_notdir		cpu_to_be32(NFSERR_NOTDIR)
#define	nfserr_isdir		cpu_to_be32(NFSERR_ISDIR)
#define	nfserr_inval		cpu_to_be32(NFSERR_INVAL)
#define	nfserr_fbig		cpu_to_be32(NFSERR_FBIG)
#define	nfserr_nospc		cpu_to_be32(NFSERR_NOSPC)
#define	nfserr_rofs		cpu_to_be32(NFSERR_ROFS)
#define	nfserr_mlink		cpu_to_be32(NFSERR_MLINK)
#define	nfserr_opnotsupp	cpu_to_be32(NFSERR_OPNOTSUPP)
#define	nfserr_nametoolong	cpu_to_be32(NFSERR_NAMETOOLONG)
#define	nfserr_notempty		cpu_to_be32(NFSERR_NOTEMPTY)
#define	nfserr_dquot		cpu_to_be32(NFSERR_DQUOT)
#define	nfserr_stale		cpu_to_be32(NFSERR_STALE)
#define	nfserr_remote		cpu_to_be32(NFSERR_REMOTE)
#define	nfserr_wflush		cpu_to_be32(NFSERR_WFLUSH)
#define	nfserr_badhandle	cpu_to_be32(NFSERR_BADHANDLE)
#define	nfserr_notsync		cpu_to_be32(NFSERR_NOT_SYNC)
#define	nfserr_notsupp		cpu_to_be32(NFSERR_NOTSUPP)
#define	nfserr_toosmall		cpu_to_be32(NFSERR_TOOSMALL)
#define	nfserr_serverfault	cpu_to_be32(NFSERR_SERVERFAULT)
#define	nfserr_badtype		cpu_to_be32(NFSERR_BADTYPE)
#define	nfserr_jukebox		cpu_to_be32(NFSERR_JUKEBOX)
#define	nfserr_denied		cpu_to_be32(NFSERR_DENIED)
#define nfserr_expired          cpu_to_be32(NFSERR_EXPIRED)
#define	nfserr_same		cpu_to_be32(NFSERR_SAME)
#define	nfserr_clid_inuse	cpu_to_be32(NFSERR_CLID_INUSE)
#define	nfserr_stale_clientid	cpu_to_be32(NFSERR_STALE_CLIENTID)
#define	nfserr_resource		cpu_to_be32(NFSERR_RESOURCE)
#define	nfserr_moved		cpu_to_be32(NFSERR_MOVED)
#define	nfserr_nofilehandle	cpu_to_be32(NFSERR_NOFILEHANDLE)
#define	nfserr_minor_vers_mismatch	cpu_to_be32(NFSERR_MINOR_VERS_MISMATCH)
#define nfserr_share_denied	cpu_to_be32(NFSERR_SHARE_DENIED)
#define nfserr_stale_stateid	cpu_to_be32(NFSERR_STALE_STATEID)
#define nfserr_old_stateid	cpu_to_be32(NFSERR_OLD_STATEID)
#define nfserr_bad_stateid	cpu_to_be32(NFSERR_BAD_STATEID)
#define nfserr_bad_seqid	cpu_to_be32(NFSERR_BAD_SEQID)
#define	nfserr_not_same		cpu_to_be32(NFSERR_NOT_SAME)
#define nfserr_lock_range	cpu_to_be32(NFSERR_LOCK_RANGE)
#define	nfserr_restorefh	cpu_to_be32(NFSERR_RESTOREFH)
#define	nfserr_attrnotsupp	cpu_to_be32(NFSERR_ATTRNOTSUPP)
#define	nfserr_bad_xdr		cpu_to_be32(NFSERR_BAD_XDR)
#define	nfserr_openmode		cpu_to_be32(NFSERR_OPENMODE)
#define	nfserr_badowner		cpu_to_be32(NFSERR_BADOWNER)
#define	nfserr_locks_held	cpu_to_be32(NFSERR_LOCKS_HELD)
#define	nfserr_op_illegal	cpu_to_be32(NFSERR_OP_ILLEGAL)
#define	nfserr_grace		cpu_to_be32(NFSERR_GRACE)
#define	nfserr_no_grace		cpu_to_be32(NFSERR_NO_GRACE)
#define	nfserr_reclaim_bad	cpu_to_be32(NFSERR_RECLAIM_BAD)
#define	nfserr_badname		cpu_to_be32(NFSERR_BADNAME)
#define	nfserr_cb_path_down	cpu_to_be32(NFSERR_CB_PATH_DOWN)
#define	nfserr_wrongsec		cpu_to_be32(NFSERR_WRONGSEC)

/* error codes for internal use */
/* if a request fails due to kmalloc failure, it gets dropped.
 *  Client should resend eventually
 */
#define	nfserr_dropit		cpu_to_be32(30000)
/* end-of-file indicator in readdir */
#define	nfserr_eof		cpu_to_be32(30001)
/* replay detected */
#define	nfserr_replay_me	cpu_to_be32(11001)

/* Check for dir entries '.' and '..' */
#define isdotent(n, l)	(l < 3 && n[0] == '.' && (l == 1 || n[1] == '.'))

#define register_cld_notifier() 0
#define unregister_cld_notifier() do { } while(0)

#endif /* LINUX_NFSD_NFSD_H */
