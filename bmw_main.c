/*
 * Syscall interface to knfsd.
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/ctype.h>

#include <linux/sunrpc/svcsock.h>
#include <linux/lockd/lockd.h>
#include <linux/sunrpc/addr.h>
#include <linux/sunrpc/rpc_pipe_fs.h>
#include <linux/module.h>

#include "nfsd.h"
#include "nfsfh.h"
#include "netns.h"

/*
 *	We have a single directory with several nodes in it.
 */
enum {
	NFSD_Root = 1,
	NFSD_Fh,
	NFSD_Threads,
	/*
	 * The below MUST come last.  Otherwise we leave a hole in nfsd_files[]
	 * with !CONFIG_NFSD_V4 and simple_fill_super() goes oops
	 */
};

int nfsd_max_blksize;

/*
 * write() for these nodes.
 */
static ssize_t write_filehandle(struct file *file, char *buf, size_t size);
static ssize_t write_threads(struct file *file, char *buf, size_t size);

static ssize_t (*write_op[])(struct file *, char *, size_t) = {
	[NFSD_Fh] = write_filehandle,
	[NFSD_Threads] = write_threads,
};

static ssize_t nfsctl_transaction_write(struct file *file, const char __user *buf, size_t size, loff_t *pos)
{
	ino_t ino =  file_inode(file)->i_ino;
	char *data;
	ssize_t rv;

	if (ino >= ARRAY_SIZE(write_op) || !write_op[ino])
		return -EINVAL;

	data = simple_transaction_get(file, buf, size);
	if (IS_ERR(data))
		return PTR_ERR(data);

	rv =  write_op[ino](file, data, size);
	if (rv >= 0) {
		simple_transaction_set(file, rv);
		rv = size;
	}
	return rv;
}

static ssize_t nfsctl_transaction_read(struct file *file, char __user *buf, size_t size, loff_t *pos)
{
	if (! file->private_data) {
		/* An attempt to read a transaction file without writing
		 * causes a 0-byte write so that the file can return
		 * state information
		 */
		ssize_t rv = nfsctl_transaction_write(file, buf, 0, pos);
		if (rv < 0)
			return rv;
	}
	return simple_transaction_read(file, buf, size, pos);
}

static const struct file_operations transaction_ops = {
	.write		= nfsctl_transaction_write,
	.read		= nfsctl_transaction_read,
	.release	= simple_transaction_release,
	.llseek		= default_llseek,
};

/*----------------------------------------------------------------------------*/
/*
 * payload - write methods
 */

static inline struct net *netns(struct file *file)
{
	return file_inode(file)->i_sb->s_fs_info;
}

/**
 * write_filehandle - Get a variable-length NFS file handle by path
 *
 * On input, the buffer contains a '\n'-terminated C string comprised of
 * three alphanumeric words separated by whitespace.  The string may
 * contain escape sequences.
 *
 * Input:
 *			buf:
 *				domain:		client domain name
 *				path:		export pathname
 *				maxsize:	numeric maximum size of
 *						@buf
 *			size:	length of C string in @buf
 * Output:
 *	On success:	passed-in buffer filled with '\n'-terminated C
 *			string containing a ASCII hex text version
 *			of the NFS file handle;
 *			return code is the size in bytes of the string
 *	On error:	return code is negative errno value
 */
static ssize_t write_filehandle(struct file *file, char *buf, size_t size)
{
	char *dname, *path;
	int uninitialized_var(maxsize);
	char *mesg = buf;
	int len;
	struct auth_domain *dom;
	struct knfsd_fh fh;

	if (size == 0)
		return -EINVAL;

	if (buf[size-1] != '\n')
		return -EINVAL;
	buf[size-1] = 0;

	dname = mesg;
	len = qword_get(&mesg, dname, size);
	if (len <= 0)
		return -EINVAL;
	
	path = dname+len+1;
	len = qword_get(&mesg, path, size);
	if (len <= 0)
		return -EINVAL;

	len = get_int(&mesg, &maxsize);
	if (len)
		return len;

	if (maxsize < NFS_FHSIZE)
		return -EINVAL;
	maxsize = min(maxsize, NFS3_FHSIZE);

	if (qword_get(&mesg, mesg, size)>0)
		return -EINVAL;

	/* we have all the words, they are in buf.. */
	dom = unix_domain_find(dname);
	if (!dom)
		return -ENOMEM;

	len = exp_rootfh(netns(file), dom, path, &fh,  maxsize);
	auth_domain_put(dom);
	if (len)
		return len;
	
	mesg = buf;
	len = SIMPLE_TRANSACTION_LIMIT;
	qword_addhex(&mesg, &len, (char*)&fh.fh_base, fh.fh_size);
	mesg[-1] = '\n';
	return mesg - buf;	
}

/**
 * write_threads - Start NFSD, or report the current number of running threads
 *
 * Input:
 *			buf:		ignored
 *			size:		zero
 * Output:
 *	On success:	passed-in buffer filled with '\n'-terminated C
 *			string numeric value representing the number of
 *			running NFSD threads;
 *			return code is the size in bytes of the string
 *	On error:	return code is zero
 *
 * OR
 *
 * Input:
 *			buf:		C string containing an unsigned
 *					integer value representing the
 *					number of NFSD threads to start
 *			size:		non-zero length of C string in @buf
 * Output:
 *	On success:	NFS service is started;
 *			passed-in buffer filled with '\n'-terminated C
 *			string numeric value representing the number of
 *			running NFSD threads;
 *			return code is the size in bytes of the string
 *	On error:	return code is zero or a negative errno value
 */
static ssize_t write_threads(struct file *file, char *buf, size_t size)
{
	char *mesg = buf;
	int rv;
	struct net *net = netns(file);

	if (size > 0) {
		int newthreads;
		rv = get_int(&mesg, &newthreads);
		if (rv)
			return rv;
		if (newthreads < 0)
			return -EINVAL;
		rv = nfsd_svc(newthreads, net);
		if (rv < 0)
			return rv;
	} else
		rv = 1;

	return scnprintf(buf, SIMPLE_TRANSACTION_LIMIT, "%d\n", rv);
}

/*----------------------------------------------------------------------------*/
/*
 *	populating the filesystem.
 */

static int nfsd_fill_super(struct super_block * sb, void * data, int silent)
{
	static struct tree_descr nfsd_files[] = {
		[NFSD_Fh] = {"filehandle", &transaction_ops, S_IWUSR|S_IRUSR},
		[NFSD_Threads] = {"threads", &transaction_ops, S_IWUSR|S_IRUSR},
		/* last one */ {""}
	};
	get_net(sb->s_fs_info);
	return simple_fill_super(sb, 0x6e667364, nfsd_files);
}

static struct dentry *nfsd_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	struct net *net = current->nsproxy->net_ns;
	return mount_ns(fs_type, flags, data, net, net->user_ns, nfsd_fill_super);
}

static void nfsd_umount(struct super_block *sb)
{
	struct net *net = sb->s_fs_info;

	kill_litter_super(sb);
	put_net(net);
}

static struct file_system_type nfsd_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "nfsd",
	.mount		= nfsd_mount,
	.kill_sb	= nfsd_umount,
};
MODULE_ALIAS_FS("nfsd");

int nfsd_net_id;

static __net_init int nfsd_init_net(struct net *net)
{
	int retval;
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	retval = nfsd_export_init(net);
	if (retval)
		goto out_export_error;

	atomic_set(&nn->ntf_refcnt, 0);
	init_waitqueue_head(&nn->ntf_wq);
	return 0;

out_export_error:
	return retval;
}

static __net_exit void nfsd_exit_net(struct net *net)
{
	nfsd_export_shutdown(net);
}

static struct pernet_operations nfsd_net_ops = {
	.init = nfsd_init_net,
	.exit = nfsd_exit_net,
	.id   = &nfsd_net_id,
	.size = sizeof(struct nfsd_net),
};

static int __init init_nfsd(void)
{
	int retval;
	printk(KERN_INFO "Installing knfsd (copyright (C) 1996 okir@monad.swb.de).\n");

	retval = register_pernet_subsys(&nfsd_net_ops);
	if (retval < 0)
		return retval;
	retval = register_cld_notifier();
	if (retval)
		goto out_unregister_pernet;
	retval = register_filesystem(&nfsd_fs_type);
	if (retval)
		goto out_unregister_notifier;
	return 0;
out_unregister_notifier:
	unregister_cld_notifier();
out_unregister_pernet:
	unregister_pernet_subsys(&nfsd_net_ops);
	return retval;
}

static void __exit exit_nfsd(void)
{
	unregister_filesystem(&nfsd_fs_type);
	unregister_cld_notifier();
	unregister_pernet_subsys(&nfsd_net_ops);
}

MODULE_AUTHOR("Olaf Kirch <okir@monad.swb.de>");
MODULE_LICENSE("GPL");
module_init(init_nfsd)
module_exit(exit_nfsd)
