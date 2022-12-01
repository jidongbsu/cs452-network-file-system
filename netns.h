/*
 * per net namespace data structures for nfsd
 *
 * Copyright (C) 2012, Jeff Layton <jlayton@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef __NFSD_NETNS_H__
#define __NFSD_NETNS_H__

#include <net/net_namespace.h>
#include <net/netns/generic.h>

/*
 * Represents a nfsd "container". With respect to nfsv4 state tracking, the
 * fields of interest are the *_id_hashtbls and the *_name_tree. These track
 * the nfs4_client objects by either short or long form clientid.
 *
 */
struct nfsd_net {

	struct cache_detail *svc_expkey_cache;
	struct cache_detail *svc_export_cache;

	bool nfsd_net_up;

	/* Time of server startup */
	struct timespec64 nfssvc_boot;

	struct svc_serv *nfsd_serv;

	wait_queue_head_t ntf_wq;
	atomic_t ntf_refcnt;
};

extern int nfsd_net_id;
#endif /* __NFSD_NETNS_H__ */
