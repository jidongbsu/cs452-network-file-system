/*
 * Central processing for nfsd.
 *
 * Authors:	Olaf Kirch (okir@monad.swb.de)
 *
 * Copyright (C) 1995, 1996, 1997 Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/sched.h>
#include <linux/freezer.h>
#include <linux/module.h>
#include <linux/fs_struct.h>
#include <linux/swap.h>

#include <linux/sunrpc/stats.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/lockd/bind.h>
#include <linux/nfsacl.h>
#include <linux/seq_file.h>
#include <linux/inetdevice.h>
#include <net/addrconf.h>
#include <net/ipv6.h>
#include <net/net_namespace.h>
#include "nfsd.h"
#include "vfs.h"
#include "netns.h"

extern struct svc_program	nfsd_program;
struct svc_stat         nfsd_svcstats = {
        .program        = &nfsd_program,
};

static int			nfsd(void *vrqstp);

static struct svc_version *	nfsd_version[] = {
	[3] = &nfsd_version3,
};

#define NFSD_MINVERS    	2
#define NFSD_NRVERS		ARRAY_SIZE(nfsd_version)
static struct svc_version *nfsd_versions[NFSD_NRVERS];

struct svc_program		nfsd_program = {
	.pg_prog		= NFS_PROGRAM,		/* program number */
	.pg_nvers		= NFSD_NRVERS,		/* nr of entries in nfsd_version */
	.pg_vers		= nfsd_versions,	/* version table */
	.pg_name		= "nfsd",		/* program name */
	.pg_class		= "nfsd",		/* authentication class */
	.pg_stats		= &nfsd_svcstats,	/* version table */
	.pg_authenticate	= &svc_set_client,	/* export authentication */

};

/*
 * Maximum number of nfsd processes
 */
#define	NFSD_MAXSERVS		8192

static int nfsd_init_socks(struct net *net)
{
	int error;
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	if (!list_empty(&nn->nfsd_serv->sv_permsocks))
		return 0;

	error = svc_create_xprt(nn->nfsd_serv, "udp", net, PF_INET, NFS_PORT,
					SVC_SOCK_DEFAULTS);
	if (error < 0)
		return error;

	error = svc_create_xprt(nn->nfsd_serv, "tcp", net, PF_INET, NFS_PORT,
					SVC_SOCK_DEFAULTS);
	if (error < 0)
		return error;

	return 0;
}

static int nfsd_startup_net(int nrservs, struct net *net)
{
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	int ret;

	if (nn->nfsd_net_up)
		return 0;

	ret = nfsd_init_socks(net);
	if (ret)
		goto out_socks;

	nn->nfsd_net_up = true;
	return 0;

out_socks:
	return ret;
}

static void nfsd_shutdown_net(struct net *net)
{
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	nn->nfsd_net_up = false;
}

static int nfsd_inetaddr_event(struct notifier_block *this, unsigned long event,
	void *ptr)
{
	struct in_ifaddr *ifa = (struct in_ifaddr *)ptr;
	struct net_device *dev = ifa->ifa_dev->dev;
	struct net *net = dev_net(dev);
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	struct sockaddr_in sin;

	if ((event != NETDEV_DOWN) ||
	    !atomic_inc_not_zero(&nn->ntf_refcnt))
		goto out;

	if (nn->nfsd_serv) {
		printk(KERN_INFO "nfsd_inetaddr_event: removed %pI4\n", &ifa->ifa_local);
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = ifa->ifa_local;
		svc_age_temp_xprts_now(nn->nfsd_serv, (struct sockaddr *)&sin);
	}
	atomic_dec(&nn->ntf_refcnt);
	wake_up(&nn->ntf_wq);

out:
	return NOTIFY_DONE;
}

static struct notifier_block nfsd_inetaddr_notifier = {
	.notifier_call = nfsd_inetaddr_event,
};

/* Only used under nfsd_mutex, so this atomic may be overkill: */
static atomic_t nfsd_notifier_refcount = ATOMIC_INIT(0);

static void nfsd_last_thread(struct svc_serv *serv, struct net *net)
{
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	atomic_dec(&nn->ntf_refcnt);
	/* check if the notifier still has clients */
	if (atomic_dec_return(&nfsd_notifier_refcount) == 0) {
		unregister_inetaddr_notifier(&nfsd_inetaddr_notifier);
	}
	wait_event(nn->ntf_wq, atomic_read(&nn->ntf_refcnt) == 0);

	/*
	 * write_ports can create the server without actually starting
	 * any threads--if we get shut down before any threads are
	 * started, then nfsd_last_thread will be run before any of this
	 * other initialization has been done except the rpcb information.
	 */
	svc_rpcb_cleanup(serv, net);
	if (!nn->nfsd_net_up)
		return;

	nfsd_shutdown_net(net);
	printk(KERN_WARNING "nfsd: last server has exited, flushing export "
			    "cache\n");
	nfsd_export_flush(net);
}

void nfsd_reset_versions(void)
{
	nfsd_program.pg_vers[3] = nfsd_version[3];
}

static int nfsd_get_default_max_blksize(void)
{
	struct sysinfo i;
	unsigned long long target;
	unsigned long ret;

	si_meminfo(&i);
	target = (i.totalram - i.totalhigh) << PAGE_SHIFT;
	/*
	 * Aim for 1/4096 of memory per thread This gives 1MB on 4Gig
	 * machines, but only uses 32K on 128M machines.  Bottom out at
	 * 8K on 32M and smaller.  Of course, this is only a default.
	 */
	target >>= 12;

	ret = NFSSVC_MAXBLKSIZE;
	while (ret > target && ret >= 8*1024*2)
		ret /= 2;
	return ret;
}

static struct svc_serv_ops nfsd_thread_sv_ops = {
	.svo_shutdown		= nfsd_last_thread,
	.svo_function		= nfsd,
	.svo_enqueue_xprt	= svc_xprt_do_enqueue,
	.svo_setup		= svc_set_num_threads,
	.svo_module		= THIS_MODULE,
};

int nfsd_create_serv(struct net *net)
{
	int error;
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	if (nn->nfsd_serv) {
		svc_get(nn->nfsd_serv);
		return 0;
	}
	if (nfsd_max_blksize == 0)
		nfsd_max_blksize = nfsd_get_default_max_blksize();
	nfsd_reset_versions();
	nn->nfsd_serv = svc_create_pooled(&nfsd_program, nfsd_max_blksize,
						&nfsd_thread_sv_ops);
	if (nn->nfsd_serv == NULL)
		return -ENOMEM;

	error = svc_bind(nn->nfsd_serv, net);
	if (error < 0) {
		svc_destroy(nn->nfsd_serv);
		return error;
	}

	/* check if the notifier is already set */
	if (atomic_inc_return(&nfsd_notifier_refcount) == 1) {
		register_inetaddr_notifier(&nfsd_inetaddr_notifier);
	}
	atomic_inc(&nn->ntf_refcnt);
	ktime_get_real_ts64(&nn->nfssvc_boot); /* record boot time */
	return 0;
}

int nfsd_nrpools(struct net *net)
{
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	if (nn->nfsd_serv == NULL)
		return 0;
	else
		return nn->nfsd_serv->sv_nrpools;
}

void nfsd_destroy(struct net *net)
{
        struct nfsd_net *nn = net_generic(net, nfsd_net_id);
        int destroy = (nn->nfsd_serv->sv_nrthreads == 1);

	printk(KERN_INFO "nfsd: destroying service\n");
        if (destroy)
                svc_shutdown_net(nn->nfsd_serv, net);
        svc_destroy(nn->nfsd_serv);
        if (destroy)
                nn->nfsd_serv = NULL;
}

/*
 * Adjust the number of threads and return the new number of threads.
 * This is also the function that starts the server if necessary, if
 * this is the first time nrservs is nonzero.
 */
int
nfsd_svc(int nrservs, struct net *net)
{
	int	error;
	bool	nfsd_up_before;
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	printk(KERN_INFO "nfsd: creating service\n");

	nrservs = max(nrservs, 0);
	nrservs = min(nrservs, NFSD_MAXSERVS);
	error = 0;

	if (nrservs == 0 && nn->nfsd_serv == NULL)
		goto out;

	error = nfsd_create_serv(net);
	if (error)
		goto out;

	nfsd_up_before = nn->nfsd_net_up;

	error = nfsd_startup_net(nrservs, net);
	if (error)
		goto out_destroy;
	error = nn->nfsd_serv->sv_ops->svo_setup(nn->nfsd_serv,
			NULL, nrservs);
	if (error)
		goto out_shutdown;
	/* We are holding a reference to nn->nfsd_serv which
	 * we don't want to count in the return value,
	 * so subtract 1
	 */
	error = nn->nfsd_serv->sv_nrthreads - 1;
out_shutdown:
	if (error < 0 && !nfsd_up_before)
		nfsd_shutdown_net(net);
out_destroy:
	nfsd_destroy(net);		/* Release server */
out:
	return error;
}


/*
 * This is the NFS server kernel thread
 */
static int
nfsd(void *vrqstp)
{
	struct svc_rqst *rqstp = (struct svc_rqst *) vrqstp;
	struct svc_xprt *perm_sock = list_entry(rqstp->rq_server->sv_permsocks.next, typeof(struct svc_xprt), xpt_list);
	struct net *net = perm_sock->xpt_net;
	int err;

	/* Lock module and set up kernel thread */

	/* At this point, the thread shares current->fs
	 * with the init process. We need to create files with the
	 * umask as defined by the client instead of init's umask. */
	if (unshare_fs_struct() < 0) {
		printk("Unable to start nfsd thread: out of memory\n");
		goto out;
	}

	current->fs->umask = 0;

	/*
	 * thread is spawned with all signals set to SIG_IGN, re-enable
	 * the ones that will bring down the thread
	 */
	allow_signal(SIGKILL);
	allow_signal(SIGHUP);
	allow_signal(SIGINT);
	allow_signal(SIGQUIT);

	set_freezable();

	/*
	 * The main request loop
	 */
	for (;;) {
		/*
		 * Find a socket with data available and call its
		 * recvfrom routine.
		 */
		while ((err = svc_recv(rqstp, 60*60*HZ)) == -EAGAIN)
			;
		if (err == -EINTR)
			break;
		validate_process_creds();
		svc_process(rqstp);
		validate_process_creds();
	}

	/* Clear signals before calling svc_exit_thread() */
	flush_signals(current);

out:
	rqstp->rq_server = NULL;

	/* Release the thread */
	svc_exit_thread(rqstp);

	nfsd_destroy(net);

	/* Release module */
	module_put_and_exit(0);
	return 0;
}

static __be32 map_new_errors(__be32 nfserr)
{
	if (nfserr == nfserr_wrongsec)
		return nfserr_acces;
	return nfserr;
}

int
nfsd_dispatch(struct svc_rqst *rqstp, __be32 *statp)
{
	struct svc_procedure	*proc;
	kxdrproc_t		xdr;
	__be32			nfserr;
	__be32			*nfserrp;

	printk(KERN_INFO "nfsd_dispatch: vers %d proc %d\n",
				rqstp->rq_vers, rqstp->rq_proc);
	proc = rqstp->rq_procinfo;

	/* Decode arguments */
	xdr = proc->pc_decode;
	if (xdr && !xdr(rqstp, (__be32*)rqstp->rq_arg.head[0].iov_base,
			rqstp->rq_argp)) {
		printk(KERN_INFO "nfsd: failed to decode arguments!\n");
		*statp = rpc_garbage_args;
		return 1;
	}

	/* need to grab the location to store the status, as
	 * nfsv4 does some encoding while processing 
	 */
	nfserrp = rqstp->rq_res.head[0].iov_base
		+ rqstp->rq_res.head[0].iov_len;
	rqstp->rq_res.head[0].iov_len += sizeof(__be32);

	/* Now call the procedure handler, and encode NFS status. */
	nfserr = proc->pc_func(rqstp, rqstp->rq_argp, rqstp->rq_resp);
	nfserr = map_new_errors(nfserr);
	if (nfserr == nfserr_dropit || test_bit(RQ_DROPME, &rqstp->rq_flags)) {
		printk(KERN_INFO "nfsd: Dropping request; may be revisited later\n");
		return 0;
	}

	if (rqstp->rq_proc != 0)
		*nfserrp++ = nfserr;

	/* Encode result.
	 * For NFSv2, additional info is never returned in case of an error.
	 */
	xdr = proc->pc_encode;
	if (xdr && !xdr(rqstp, nfserrp, rqstp->rq_resp)) {
		/* Failed to encode result. Release cache entry */
		printk(KERN_INFO "nfsd: failed to encode result!\n");
		*statp = rpc_system_err;
		return 1;
	}

	return 1;
}

