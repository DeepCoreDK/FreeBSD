/*-
 * Copyright (c) 1980, 1986, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)if.c	8.5 (Berkeley) 1/9/95
 * $FreeBSD$
 */

#include "opt_compat.h"
#include "opt_device_polling.h"
#include "opt_inet6.h"
#include "opt_inet.h"

#include <sys/param.h>
#include <sys/types.h>
#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/sbuf.h>
#include <sys/bus.h>
#include <sys/mbuf.h>
#include <sys/systm.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/protosw.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/refcount.h>
#include <sys/module.h>
#include <sys/rwlock.h>
#include <sys/sockio.h>
#include <sys/syslog.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/domain.h>
#include <sys/jail.h>
#include <sys/priv.h>

#include <machine/stdarg.h>
#include <vm/uma.h>

#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_clone.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/if_vlan_var.h>
#include <net/radix.h>
#include <net/route.h>
#include <net/vnet.h>

#if defined(INET) || defined(INET6)
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/ip_carp.h>
#ifdef INET
#include <netinet/if_ether.h>
#endif /* INET */
#ifdef INET6
#include <netinet6/in6_var.h>
#include <netinet6/in6_ifattach.h>
#endif /* INET6 */
#endif /* INET || INET6 */

#include <security/mac/mac_framework.h>

#ifdef COMPAT_FREEBSD32
#include <sys/mount.h>
#include <compat/freebsd32/freebsd32.h>
#endif

/* Interface media functions, living in if_media.c. */
extern void 	ifmedia_alloc(struct ifnet *, struct if_attach_args *);
extern void	ifmedia_free(struct ifnet *);
extern int	ifmedia_ioctl(struct ifnet *, struct ifreq *, u_long);

SYSCTL_NODE(_net, PF_LINK, link, CTLFLAG_RW, 0, "Link layers");
SYSCTL_NODE(_net_link, 0, generic, CTLFLAG_RW, 0, "Generic link-management");

/* Log link state change events */
static int log_link_state_change = 1;

SYSCTL_INT(_net_link, OID_AUTO, log_link_state_change, CTLFLAG_RW,
	&log_link_state_change, 0,
	"log interface link state change events");

/* Interface description */
static unsigned int ifdescr_maxlen = 1024;
SYSCTL_UINT(_net, OID_AUTO, ifdescr_maxlen, CTLFLAG_RW,
	&ifdescr_maxlen, 0,
	"administrative maximum length for interface description");

static MALLOC_DEFINE(M_IFDESCR, "ifdescr", "ifnet descriptions");

/* global sx for non-critical path ifdescr */
static struct sx ifdescr_sx;
SX_SYSINIT(ifdescr_sx, &ifdescr_sx, "ifnet descr");

void	(*bridge_linkstate_p)(struct ifnet *ifp);
void	(*ng_ether_link_state_p)(struct ifnet *ifp, int state);
void	(*lagg_linkstate_p)(struct ifnet *ifp, int state);
/* These are external hooks for CARP. */
void	(*carp_linkstate_p)(struct ifnet *ifp);
void	(*carp_demote_adj_p)(int, char *);
int	(*carp_master_p)(struct ifaddr *);
#if defined(INET) || defined(INET6)
int	(*carp_forus_p)(struct ifnet *ifp, u_char *dhost);
int	(*carp_output_p)(struct ifnet *ifp, struct mbuf *m,
    const struct sockaddr *sa);
int	(*carp_ioctl_p)(struct ifreq *, u_long, struct thread *);   
int	(*carp_attach_p)(struct ifaddr *, int);
void	(*carp_detach_p)(struct ifaddr *);
#endif
#ifdef INET
int	(*carp_iamatch_p)(struct ifaddr *, uint8_t **);
#endif
#ifdef INET6
struct ifaddr *(*carp_iamatch6_p)(struct ifnet *ifp, struct in6_addr *taddr6);
caddr_t	(*carp_macmatch6_p)(struct ifnet *ifp, struct mbuf *m,
    const struct in6_addr *taddr);
#endif

struct mbuf *(*tbr_dequeue_ptr)(struct ifaltq *, int) = NULL;

/*
 * XXX: Style; these should be sorted alphabetically, and unprototyped
 * static functions should be prototyped. Currently they are sorted by
 * declaration order.
 */
static void	if_attachdomain(void *);
static void	if_attachdomain1(struct ifnet *);
static int	ifconf(u_long, caddr_t);
static void	if_freemulti(struct ifmultiaddr *);
static void	if_grow(void);
static int	if_setflag(struct ifnet *, int, int, int *, int);
static void	link_rtrequest(int, struct rtentry *, struct rt_addrinfo *);
static int	if_delmulti_locked(struct ifnet *, struct ifmultiaddr *, int);
static void	do_link_state_change(void *, int);
static int	if_getgroup(struct ifgroupreq *, struct ifnet *);
static int	if_getgroupmembers(struct ifgroupreq *);
static void	if_delgroups(struct ifnet *);
static void	if_attach_internal(struct ifnet *, int, struct if_clone *);
static void	if_detach_internal(struct ifnet *, int, struct if_clone **);
static struct ifqueue * if_snd_alloc(int);
static void	if_snd_free(struct ifqueue *);
static void	if_snd_qflush(if_t);

#ifdef INET6
/*
 * XXX: declare here to avoid to include many inet6 related files..
 * should be more generalized?
 */
extern void	nd6_setmtu(struct ifnet *);
#endif

VNET_DEFINE(int, if_index);
VNET_DEFINE(struct ifnethead, ifnet);	/* depend on static init XXX */
VNET_DEFINE(struct ifgrouphead, ifg_head);

static VNET_DEFINE(int, if_indexlim) = 8;

/* Table of ifnet by index. */
VNET_DEFINE(struct ifnet **, ifindex_table);

#define	V_if_indexlim		VNET(if_indexlim)
#define	V_ifindex_table		VNET(ifindex_table)

static struct iftsomax default_tsomax = {
	/*
	 * The TSO defaults need to be such that an NFS mbuf list of 35
	 * mbufs totalling just below 64K works and that a chain of mbufs
	 * can be defragged into at most 32 segments.
	 */
	.tsomax_bytes = MIN(IP_MAXPACKET, (32 * MCLBYTES) - (ETHER_HDR_LEN +
	    ETHER_VLAN_ENCAP_LEN)),
	.tsomax_segcount = 35,
	.tsomax_segsize = 2048,
};

/*
 * The global network interface list (V_ifnet) and related state (such as
 * if_index, if_indexlim, and ifindex_table) are protected by an sxlock and
 * an rwlock.  Either may be acquired shared to stablize the list, but both
 * must be acquired writable to modify the list.  This model allows us to
 * both stablize the interface list during interrupt thread processing, but
 * also to stablize it over long-running ioctls, without introducing priority
 * inversions and deadlocks.
 */
struct rwlock ifnet_rwlock;
RW_SYSINIT_FLAGS(ifnet_rw, &ifnet_rwlock, "ifnet_rw", RW_RECURSE);
struct sx ifnet_sxlock;
SX_SYSINIT_FLAGS(ifnet_sx, &ifnet_sxlock, "ifnet_sx", SX_RECURSE);

/*
 * The allocation of network interfaces is a rather non-atomic affair; we
 * need to select an index before we are ready to expose the interface for
 * use, so will use this pointer value to indicate reservation.
 */
#define	IFNET_HOLD	(void *)(uintptr_t)(-1)

static MALLOC_DEFINE(M_IFNET, "ifnet", "interface internals");
MALLOC_DEFINE(M_IFADDR, "ifaddr", "interface address");
MALLOC_DEFINE(M_IFMADDR, "ether_multi", "link-level multicast address");

static struct ifops ifdead_ops;

struct ifnet *
ifnet_byindex_locked(u_short idx)
{

	if (idx > V_if_index)
		return (NULL);
	if (V_ifindex_table[idx] == IFNET_HOLD)
		return (NULL);
	return (V_ifindex_table[idx]);
}

struct ifnet *
ifnet_byindex(u_short idx)
{
	struct ifnet *ifp;

	IFNET_RLOCK_NOSLEEP();
	ifp = ifnet_byindex_locked(idx);
	IFNET_RUNLOCK_NOSLEEP();
	return (ifp);
}

struct ifnet *
ifnet_byindex_ref(u_short idx)
{
	struct ifnet *ifp;

	IFNET_RLOCK_NOSLEEP();
	ifp = ifnet_byindex_locked(idx);
	if (ifp == NULL || (ifp->if_flags & IFF_DYING)) {
		IFNET_RUNLOCK_NOSLEEP();
		return (NULL);
	}
	if_ref(ifp);
	IFNET_RUNLOCK_NOSLEEP();
	return (ifp);
}

/*
 * Allocate an ifindex array entry.
 */
static void
ifindex_alloc(struct ifnet *ifp)
{
	u_short idx;

	IFNET_WLOCK();
retry:
	/*
	 * Try to find an empty slot below V_if_index.  If we fail, take the
	 * next slot.
	 */
	for (idx = 1; idx <= V_if_index; idx++) {
		if (V_ifindex_table[idx] == NULL)
			break;
	}

	/* Catch if_index overflow. */
	if (idx >= V_if_indexlim) {
		if_grow();
		goto retry;
	}
	if (idx > V_if_index)
		V_if_index = idx;
	V_ifindex_table[idx] = ifp;
	ifp->if_index = idx;
	IFNET_WUNLOCK();
}

static void
ifindex_free(u_short idx)
{

	IFNET_WLOCK_ASSERT();

	V_ifindex_table[idx] = NULL;
	while (V_if_index > 0 &&
	    V_ifindex_table[V_if_index] == NULL)
		V_if_index--;
}

struct ifaddr *
ifaddr_byindex(u_short idx)
{
	struct ifnet *ifp;
	struct ifaddr *ifa = NULL;

	IFNET_RLOCK_NOSLEEP();
	ifp = ifnet_byindex_locked(idx);
	if (ifp != NULL && (ifa = ifp->if_addr) != NULL)
		ifa_ref(ifa);
	IFNET_RUNLOCK_NOSLEEP();
	return (ifa);
}

/*
 * Network interface utility routines.
 *
 * Routines with ifa_ifwith* names take sockaddr *'s as
 * parameters.
 */

static void
vnet_if_init(const void *unused __unused)
{

	TAILQ_INIT(&V_ifnet);
	TAILQ_INIT(&V_ifg_head);
	IFNET_WLOCK();
	if_grow();				/* create initial table */
	IFNET_WUNLOCK();
	vnet_if_clone_init();
}
VNET_SYSINIT(vnet_if_init, SI_SUB_INIT_IF, SI_ORDER_SECOND, vnet_if_init,
    NULL);

#ifdef VIMAGE
static void
vnet_if_uninit(const void *unused __unused)
{

	VNET_ASSERT(TAILQ_EMPTY(&V_ifnet), ("%s:%d tailq &V_ifnet=%p "
	    "not empty", __func__, __LINE__, &V_ifnet));
	VNET_ASSERT(TAILQ_EMPTY(&V_ifg_head), ("%s:%d tailq &V_ifg_head=%p "
	    "not empty", __func__, __LINE__, &V_ifg_head));

	free((caddr_t)V_ifindex_table, M_IFNET);
}
VNET_SYSUNINIT(vnet_if_uninit, SI_SUB_INIT_IF, SI_ORDER_FIRST,
    vnet_if_uninit, NULL);
#endif

static void
if_grow(void)
{
	int oldlim;
	u_int n;
	struct ifnet **e;

	IFNET_WLOCK_ASSERT();
	oldlim = V_if_indexlim;
	IFNET_WUNLOCK();
	n = (oldlim << 1) * sizeof(*e);
	e = malloc(n, M_IFNET, M_WAITOK | M_ZERO);
	IFNET_WLOCK();
	if (V_if_indexlim != oldlim) {
		free(e, M_IFNET);
		return;
	}
	if (V_ifindex_table != NULL) {
		memcpy((caddr_t)e, (caddr_t)V_ifindex_table, n/2);
		free((caddr_t)V_ifindex_table, M_IFNET);
	}
	V_if_indexlim <<= 1;
	V_ifindex_table = e;
}

/*
 * Registration/deregistration of interface types.  A type can carry
 * common methods.  Certain drivers depend on types to be loaded.
 */
static SLIST_HEAD(, iftype) iftypehead = SLIST_HEAD_INITIALIZER(iftypehead);
void
iftype_register(struct iftype *ift)
{

	IFNET_WLOCK();
	SLIST_INSERT_HEAD(&iftypehead, ift, ift_next);
	IFNET_WUNLOCK();
}

void
iftype_unregister(struct iftype *ift)
{

	IFNET_WLOCK();
	SLIST_REMOVE(&iftypehead, ift, iftype, ift_next);
	IFNET_WUNLOCK();
}

static struct iftype *
iftype_find(ifType type)
{
	struct iftype *ift;

	IFNET_RLOCK();
	SLIST_FOREACH(ift, &iftypehead, ift_next)
		if (ift->ift_type == type)
			break;
	IFNET_RUNLOCK();

	return (ift);
}

#define	ifdrv_flags		__ifdrv_stack_owned
#define	IFDRV_BLESSED		0x00000001

static void
ifdriver_bless(struct ifdriver *ifdrv, struct iftype *ift)
{

	/*
	 * If the driver doesn't define certain op, but its type has
	 * default implementation, then copy it.
	 */
	if (ift != NULL) {
#define	COPYOP(op)	if (ifdrv->ifdrv_ops.ifop_ ## op == NULL)	\
				ifdrv->ifdrv_ops.ifop_ ## op =		\
				    ift->ift_ops.ifop_ ## op
		COPYOP(input);
		COPYOP(transmit);
		COPYOP(output);
		COPYOP(ioctl);
		COPYOP(get_counter);
		COPYOP(qflush);
		COPYOP(resolvemulti);
		COPYOP(reassign);
#undef COPYOP
#define	COPY(f)		if (ifdrv->ifdrv_ ## f == 0)			\
				ifdrv->ifdrv_ ## f = ift->ift_ ## f
		COPY(hdrlen);
		COPY(addrlen);
		COPY(dlt);
		COPY(dlt_hdrlen);
#undef COPY
	}

	/*
	 * If the driver has ifdrv_maxqlen defined, then opts-in
	 * for * generic software queue, and thus for default
	 * ifop_qflush.
	 */
	if (ifdrv->ifdrv_maxqlen > 0) {
		KASSERT(ifdrv->ifdrv_ops.ifop_qflush == NULL,
		    ("%s: fdrv_maxqlen > 0 and ifop_qflush",
		    ifdrv->ifdrv_name));
		ifdrv->ifdrv_ops.ifop_qflush = if_snd_qflush;
	}

	/*
	 * If neither driver nor its type has a definitation of an op
	 * that is mandatory, then set it to default implementation.
	 */
#define	DEFAULTOP(op)	if (ifdrv->ifdrv_ops.ifop_ ## op == NULL)	\
				ifdrv->ifdrv_ops.ifop_ ## op =		\
				    if_ ## op ## _default
	DEFAULTOP(get_counter);
#undef DEFAULTOP

#if defined(INET) || defined(INET6)
	/* Use defaults for TSO, if nothing is set. */
	if (ifdrv->ifdrv_tsomax == NULL)
		ifdrv->ifdrv_tsomax = &default_tsomax;
	else
		KASSERT(ifdrv->ifdrv_tsomax->tsomax_bytes == 0 ||
		    ifdrv->ifdrv_tsomax->tsomax_bytes >= (IP_MAXPACKET / 8),
		    ("%s: tsomax_bytes is outside of range",
		    ifdrv->ifdrv_name));
#endif

	ifdrv->ifdrv_ops.ifop_origin = IFOP_ORIGIN_DRIVER;
	ifdrv->ifdrv_flags |= IFDRV_BLESSED;
}

/*
 * Allocate a struct ifnet and an index for an interface.  A layer 2
 * common structure will also be allocated if an allocation routine is
 * registered for the passed type.
 *
 * The only reason for this function to fail is failure to allocate a
 * unit number, which is possible only if driver does cloning.
 */
if_t
if_attach(struct if_attach_args *ifat)
{
	struct ifdriver *ifdrv;
	struct iftype *ift;
	struct ifnet *ifp;
	struct ifaddr *ifa;
	struct sockaddr_dl *sdl;
	int socksize, ifasize, namelen, masklen;

	KASSERT(ifat->ifat_version == IF_ATTACH_VERSION,
	    ("%s: version %d, expected %d",
	    __func__, ifat->ifat_version, IF_ATTACH_VERSION));

	ifdrv = ifat->ifat_drv;
	ift = iftype_find(ifdrv->ifdrv_type);
	if ((ifdrv->ifdrv_flags & IFDRV_BLESSED) == 0)
		ifdriver_bless(ifdrv, ift);

	if (ifdrv->ifdrv_clone != NULL) {
		int error;

		error = ifc_alloc_unit(ifdrv->ifdrv_clone, &ifat->ifat_dunit);
		if (error) {
			log(LOG_WARNING, "%s unit allocation failure: %d\n",
			    ifdrv->ifdrv_name, error);
			ifat->ifat_error = error;
			return (NULL);
		}
	}

	ifp = malloc(sizeof(struct ifnet), M_IFNET, M_WAITOK | M_ZERO);
	ifp->if_scstore = malloc(sizeof(struct ifsoftc) * SOFTC_CACHE_SIZE,
	    M_IFNET, M_WAITOK | M_ZERO);
	ifp->if_nsoftcs = SOFTC_CACHE_SIZE;
	for (int i = 0; i < IFCOUNTERS; i++)
		ifp->if_counters[i] = counter_u64_alloc(M_WAITOK);
#ifdef MAC
	mac_ifnet_init(ifp);
	mac_ifnet_create(ifp);
#endif
	rw_init(&ifp->if_lock, "if_lock");

	ifp->if_ops = &ifdrv->ifdrv_ops;
	ifp->if_drv = ifdrv;
	ifp->if_type = ift;
	
#define	COPY(f)	ifp->if_ ## f = ifat->ifat_ ## f
	COPY(softc);
	COPY(mtu);
	COPY(flags);
	COPY(capabilities);
	COPY(capenable);
	COPY(hwassist);
	COPY(baudrate);
#undef COPY

	if (ifat->ifat_mediae) {
		KASSERT(ifp->if_ops->ifop_media_change != NULL &&
		    ifp->if_ops->ifop_media_status != NULL,
		    ("%s: media array but no callbacks", ifdrv->ifdrv_name));
		ifmedia_alloc(ifp, ifat);
	}

	if (ifat->ifat_tsomax) {
		/*
		 * Driver wants dynamic tsomax on this interface, we
		 * will allocate one and are responsible for freeing
		 * it on detach.
		 */
		KASSERT(ifat->ifat_tsomax->tsomax_bytes == 0 ||
		    ifat->ifat_tsomax->tsomax_bytes >= (IP_MAXPACKET / 8),
		    ("%s: tsomax_bytes is outside of range",
		    ifdrv->ifdrv_name));
		ifp->if_tsomax = malloc(sizeof(struct iftsomax), M_IFNET,
		    M_WAITOK);
		bcopy(ifat->ifat_tsomax, ifp->if_tsomax,
		    sizeof(struct iftsomax));
	} else
		ifp->if_tsomax = ifdrv->ifdrv_tsomax;

	if (ifdrv->ifdrv_maxqlen > 0)
		ifp->if_snd = if_snd_alloc(ifdrv->ifdrv_maxqlen);

	IF_AFDATA_LOCK_INIT(ifp);
	TASK_INIT(&ifp->if_linktask, 0, do_link_state_change, ifp);
	TAILQ_INIT(&ifp->if_addrhead);
	TAILQ_INIT(&ifp->if_multiaddrs);
	TAILQ_INIT(&ifp->if_groups);

	/* XXXGL: there is no check that name is unique. */
	ifp->if_dunit = ifat->ifat_dunit;
	if (ifat->ifat_name)
		strlcpy(ifp->if_xname, ifat->ifat_name, IFNAMSIZ);
	else if (ifat->ifat_dunit != IFAT_DUNIT_NONE)
		snprintf(ifp->if_xname, IFNAMSIZ, "%s%d",
		    ifdrv->ifdrv_name, ifat->ifat_dunit);
	else
		strlcpy(ifp->if_xname, ifdrv->ifdrv_name, IFNAMSIZ);

	ifindex_alloc(ifp);
	refcount_init(&ifp->if_refcount, 1);

	/*
	 * Allocate ifaddr to store link level address and name for this
	 * interface.  Always save enough space for any possiable name so
	 * we can do a rename in place later.
	 */
	namelen = strlen(ifp->if_xname);
	masklen = offsetof(struct sockaddr_dl, sdl_data[0]) + IFNAMSIZ;
	socksize = masklen + ifdrv->ifdrv_addrlen;
	if (socksize < sizeof(*sdl))
		socksize = sizeof(*sdl);
	socksize = roundup2(socksize, sizeof(long));
	ifasize = sizeof(*ifa) + 2 * socksize;
	ifa = ifa_alloc(ifasize, M_WAITOK);
	sdl = (struct sockaddr_dl *)(ifa + 1);
	sdl->sdl_len = socksize;
	sdl->sdl_family = AF_LINK;
	bcopy(ifp->if_xname, sdl->sdl_data, namelen);
	sdl->sdl_nlen = namelen;
	sdl->sdl_index = ifp->if_index;
	sdl->sdl_type = ifdrv->ifdrv_type;
	sdl->sdl_alen = ifdrv->ifdrv_addrlen;
	if (ifat->ifat_lla != NULL)
		bcopy(ifat->ifat_lla, LLADDR(sdl), ifdrv->ifdrv_addrlen);
	ifp->if_addr = ifa;
	ifa->ifa_ifp = ifp;
	ifa->ifa_rtrequest = link_rtrequest;
	ifa->ifa_addr = (struct sockaddr *)sdl;
	sdl = (struct sockaddr_dl *)(socksize + (char *)sdl);
	ifa->ifa_netmask = (struct sockaddr *)sdl;
	sdl->sdl_len = masklen;
	while (namelen != 0)
		sdl->sdl_data[--namelen] = 0xff;
	TAILQ_INSERT_HEAD(&ifp->if_addrhead, ifa, ifa_link);

	if (ift)
		ift->ift_attach(ifp, ifat);

	bpfattach(ifp, ifdrv->ifdrv_dlt, ifdrv->ifdrv_dlt_hdrlen);

	if_attach_internal(ifp, 0, NULL);

	return (ifp);
}

/*
 * Do the actual work of freeing a struct ifnet, and layer 2 common
 * structure.  This call is made when the last reference to an
 * interface is released.
 */
static void
if_free_internal(struct ifnet *ifp)
{

	KASSERT((ifp->if_flags & IFF_DYING),
	    ("if_free_internal: interface not dying"));

#ifdef MAC
	mac_ifnet_destroy(ifp);
#endif /* MAC */
	if (ifp->if_description != NULL)
		free(ifp->if_description, M_IFDESCR);
	IF_AFDATA_DESTROY(ifp);
	if (ifp->if_snd)
		if_snd_free(ifp->if_snd);

	for (int i = 0; i < IFCOUNTERS; i++)
		counter_u64_free(ifp->if_counters[i]);

	if (ifp->if_tsomax != ifp->if_drv->ifdrv_tsomax)
		free(ifp->if_tsomax, M_IFNET);

	ifmedia_free(ifp);

	rw_destroy(&ifp->if_lock);
	free(ifp, M_IFNET);
}

void
if_mtap(if_t ifp, struct mbuf *m, void *data, u_int dlen)
{

	if (!bpf_peers_present(ifp->if_bpf))
		return;

	if (dlen == 0) {
		if (m->m_flags & M_VLANTAG)
			ether_vlan_mtap(ifp->if_bpf, m, NULL, 0);
		else
			bpf_mtap(ifp->if_bpf, m);
	} else
		bpf_mtap2(ifp->if_bpf, data, dlen, m);
}

/*
 * Interfaces to keep an ifnet type-stable despite the possibility of the
 * driver calling if_free().  If there are additional references, we defer
 * freeing the underlying data structure.
 */
void
if_ref(struct ifnet *ifp)
{

	/* We don't assert the ifnet list lock here, but arguably should. */
	refcount_acquire(&ifp->if_refcount);
}

void
if_rele(struct ifnet *ifp)
{

	if (!refcount_release(&ifp->if_refcount))
		return;
	if_free_internal(ifp);
}

/*
 * Compute the least common TSO limit.
 */
void
if_tsomax_common(const struct iftsomax *from, struct iftsomax *to)
{

	/*
	 * 1) If there is no limit currently, take the limit from
	 * the network adapter.
	 *
	 * 2) If the network adapter has a limit below the current
	 * limit, apply it.
	 */
	if (to->tsomax_bytes == 0 || (from->tsomax_bytes != 0 &&
	    from->tsomax_bytes < to->tsomax_bytes)) {
		to->tsomax_bytes = from->tsomax_bytes;
	}
	if (to->tsomax_segcount == 0 || (from->tsomax_segcount != 0 &&
	    from->tsomax_segcount < to->tsomax_segcount)) {
		to->tsomax_segcount = from->tsomax_segcount;
	}
	if (to->tsomax_segsize == 0 || (from->tsomax_segsize != 0 &&
	    from->tsomax_segsize < to->tsomax_segsize)) {
		to->tsomax_segsize = from->tsomax_segsize;
	}
}

/*
 * Update TSO limit of a network adapter.
 *
 * Returns zero if no change. Else non-zero.
 */
int
if_tsomax_update(if_t ifp, const struct iftsomax *new)
{
	int retval = 0;

	KASSERT(ifp->if_tsomax != ifp->if_drv->ifdrv_tsomax,
	    ("%s: interface %s (driver %s) has static if_tsomax", __func__,
	    ifp->if_xname, ifp->if_drv->ifdrv_name));

	if (ifp->if_tsomax->tsomax_bytes != new->tsomax_bytes) {
		ifp->if_tsomax->tsomax_bytes = new->tsomax_bytes;
		retval++;
	}
	if (ifp->if_tsomax->tsomax_segsize != new->tsomax_segsize) {
		ifp->if_tsomax->tsomax_segsize = new->tsomax_segsize;
		retval++;
	}
	if (ifp->if_tsomax->tsomax_segcount != new->tsomax_segcount) {
		ifp->if_tsomax->tsomax_segcount = new->tsomax_segcount;
		retval++;
	}
	KASSERT(ifp->if_tsomax->tsomax_bytes == 0 ||
	    ifp->if_tsomax->tsomax_bytes >= (IP_MAXPACKET / 8),
	    ("%s: tsomax_bytes is outside of range", ifp->if_xname));
	return (retval);
}

static void
if_attach_internal(struct ifnet *ifp, int vmove, struct if_clone *ifc)
{

	if (ifp->if_index == 0 || ifp != ifnet_byindex(ifp->if_index))
		panic ("%s: BUG: if_attach called without if_alloc'd input()\n",
		    ifp->if_xname);

#ifdef VIMAGE
	ifp->if_vnet = curvnet;
	if (ifp->if_home_vnet == NULL)
		ifp->if_home_vnet = curvnet;
#endif

	if_addgroup(ifp, IFG_ALL);

	/* Restore group membership for cloned interfaces. */
	if (vmove && ifc != NULL)
		if_clone_addgroup(ifp, ifc);

	getmicrotime(&ifp->if_lastchange);
	ifp->if_epoch = time_uptime;

#ifdef VIMAGE
	/*
	 * Update the interface index in the link layer address
	 * of the interface.
	 */
	for (ifa = ifp->if_addr; ifa != NULL;
	    ifa = TAILQ_NEXT(ifa, ifa_link)) {
		if (ifa->ifa_addr->sa_family == AF_LINK) {
			sdl = (struct sockaddr_dl *)ifa->ifa_addr;
			sdl->sdl_index = ifp->if_index;
		}
	}
#endif

	IFNET_WLOCK();
	TAILQ_INSERT_TAIL(&V_ifnet, ifp, if_link);
#ifdef VIMAGE
	curvnet->vnet_ifcnt++;
#endif
	IFNET_WUNLOCK();

	if (domain_init_status >= 2)
		if_attachdomain1(ifp);

	EVENTHANDLER_INVOKE(ifnet_arrival_event, ifp);
	if (IS_DEFAULT_VNET(curvnet))
		devctl_notify("IFNET", ifp->if_xname, "ATTACH", NULL);

	/* Announce the interface. */
	rt_ifannouncemsg(ifp, IFAN_ARRIVAL);
}

static void
if_attachdomain(void *dummy)
{
	struct ifnet *ifp;

	TAILQ_FOREACH(ifp, &V_ifnet, if_link)
		if_attachdomain1(ifp);
}
SYSINIT(domainifattach, SI_SUB_PROTO_IFATTACHDOMAIN, SI_ORDER_SECOND,
    if_attachdomain, NULL);

static void
if_attachdomain1(struct ifnet *ifp)
{
	struct domain *dp;

	/*
	 * Since dp->dom_ifattach calls malloc() with M_WAITOK, we
	 * cannot lock ifp->if_afdata initialization, entirely.
	 */
	if (IF_AFDATA_TRYLOCK(ifp) == 0)
		return;
	if (ifp->if_afdata_initialized >= domain_init_status) {
		IF_AFDATA_UNLOCK(ifp);
		log(LOG_WARNING, "%s called more than once on %s\n",
		    __func__, ifp->if_xname);
		return;
	}
	ifp->if_afdata_initialized = domain_init_status;
	IF_AFDATA_UNLOCK(ifp);

	/* address family dependent data region */
	bzero(ifp->if_afdata, sizeof(ifp->if_afdata));
	for (dp = domains; dp; dp = dp->dom_next) {
		if (dp->dom_ifattach)
			ifp->if_afdata[dp->dom_family] =
			    (*dp->dom_ifattach)(ifp);
	}
}

/*
 * Remove any unicast or broadcast network addresses from an interface.
 */
void
if_purgeaddrs(struct ifnet *ifp)
{
	struct ifaddr *ifa, *next;

	TAILQ_FOREACH_SAFE(ifa, &ifp->if_addrhead, ifa_link, next) {
		if (ifa->ifa_addr->sa_family == AF_LINK)
			continue;
#ifdef INET
		/* XXX: Ugly!! ad hoc just for INET */
		if (ifa->ifa_addr->sa_family == AF_INET) {
			struct ifaliasreq ifr;

			bzero(&ifr, sizeof(ifr));
			ifr.ifra_addr = *ifa->ifa_addr;
			if (ifa->ifa_dstaddr)
				ifr.ifra_broadaddr = *ifa->ifa_dstaddr;
			if (in_control(NULL, SIOCDIFADDR, (caddr_t)&ifr, ifp,
			    NULL) == 0)
				continue;
		}
#endif /* INET */
#ifdef INET6
		if (ifa->ifa_addr->sa_family == AF_INET6) {
			in6_purgeaddr(ifa);
			/* ifp_addrhead is already updated */
			continue;
		}
#endif /* INET6 */
		TAILQ_REMOVE(&ifp->if_addrhead, ifa, ifa_link);
		ifa_free(ifa);
	}
}

/*
 * Remove any multicast network addresses from an interface when an ifnet
 * is going away.
 */
static void
if_purgemaddrs(struct ifnet *ifp)
{
	struct ifmultiaddr *ifma;
	struct ifmultiaddr *next;

	IF_ADDR_WLOCK(ifp);
	TAILQ_FOREACH_SAFE(ifma, &ifp->if_multiaddrs, ifma_link, next)
		if_delmulti_locked(ifp, ifma, 1);
	IF_ADDR_WUNLOCK(ifp);
}

/*
 * Detach an interface, removing it from the list of "active" interfaces.
 * If vmove flag is set on entry to if_detach_internal(), perform only a
 * limited subset of cleanup tasks, given that we are moving an ifnet from
 * one vnet to another, where it must be fully operational.
 *
 * XXXRW: There are some significant questions about event ordering, and
 * how to prevent things from starting to use the interface during detach.
 */
void
if_detach(if_t ifp)
{

	ifp->if_flags |= IFF_DYING;			/* XXX: Locking */

	bpfdetach(ifp);
#ifdef DEVICE_POLLING
	if (ifp->if_capenable & IFCAP_POLLING)
		if_poll_deregister(ifp);
#endif
	CURVNET_SET_QUIET(ifp->if_vnet);
	if_detach_internal(ifp, 0, NULL);

	IFNET_WLOCK();
	KASSERT(ifp == ifnet_byindex_locked(ifp->if_index),
	    ("%s: freeing unallocated ifnet", ifp->if_xname));

	ifindex_free(ifp->if_index);
	IFNET_WUNLOCK();

	if (ifp->if_drv->ifdrv_clone != NULL)
		ifc_free_unit(ifp->if_drv->ifdrv_clone, ifp->if_dunit);

	if (refcount_release(&ifp->if_refcount))
		if_free_internal(ifp);
	CURVNET_RESTORE();
}

static void
if_detach_internal(struct ifnet *ifp, int vmove, struct if_clone **ifcp)
{
	struct ifaddr *ifa;
	int i;
	struct domain *dp;
 	struct ifnet *iter;
 	int found = 0;

	IFNET_WLOCK();
	TAILQ_FOREACH(iter, &V_ifnet, if_link)
		if (iter == ifp) {
			TAILQ_REMOVE(&V_ifnet, ifp, if_link);
			found = 1;
			break;
		}
#ifdef VIMAGE
	if (found)
		curvnet->vnet_ifcnt--;
#endif
	IFNET_WUNLOCK();
	if (!found) {
		if (vmove)
			panic("%s: ifp=%p not on the ifnet tailq %p",
			    __func__, ifp, &V_ifnet);
		else
			return; /* XXX this should panic as well? */
	}

	/* Check if this is a cloned interface or not. */
	if (vmove && ifcp != NULL)
		*ifcp = if_clone_findifc(ifp);

	/*
	 * Remove/wait for pending events.
	 */
	taskqueue_drain(taskqueue_swi, &ifp->if_linktask);

	/*
	 * Remove routes and flush queues.
	 */
	if_down(ifp);
#ifdef ALTQ
	if (ALTQ_IS_ENABLED(&ifp->if_snd))
		altq_disable(&ifp->if_snd);
	if (ALTQ_IS_ATTACHED(&ifp->if_snd))
		altq_detach(&ifp->if_snd);
#endif

	if_purgeaddrs(ifp);

#ifdef INET
	in_ifdetach(ifp);
#endif

#ifdef INET6
	/*
	 * Remove all IPv6 kernel structs related to ifp.  This should be done
	 * before removing routing entries below, since IPv6 interface direct
	 * routes are expected to be removed by the IPv6-specific kernel API.
	 * Otherwise, the kernel will detect some inconsistency and bark it.
	 */
	in6_ifdetach(ifp);
#endif
	if_purgemaddrs(ifp);

	/* Announce that the interface is gone. */
	rt_ifannouncemsg(ifp, IFAN_DEPARTURE);
	EVENTHANDLER_INVOKE(ifnet_departure_event, ifp);
	if (IS_DEFAULT_VNET(curvnet))
		devctl_notify("IFNET", ifp->if_xname, "DETACH", NULL);

	if (!vmove) {
		struct iftype *ift = ifp->if_type;

		if (ift != NULL && ift->ift_detach != NULL)
			ift->ift_detach(ifp);

		/*
		 * Prevent further calls into the device driver via ifnet.
		 */
		ifp->if_ops = &ifdead_ops;

		/*
		 * Remove link ifaddr pointer and maybe decrement if_index.
		 * Clean up all addresses.
		 */
		ifp->if_addr = NULL;

		/* We can now free link ifaddr. */
		if (!TAILQ_EMPTY(&ifp->if_addrhead)) {
			ifa = TAILQ_FIRST(&ifp->if_addrhead);
			TAILQ_REMOVE(&ifp->if_addrhead, ifa, ifa_link);
			ifa_free(ifa);
		}
	}

	rt_flushifroutes(ifp);
	if_delgroups(ifp);

	/*
	 * We cannot hold the lock over dom_ifdetach calls as they might
	 * sleep, for example trying to drain a callout, thus open up the
	 * theoretical race with re-attaching.
	 */
	IF_AFDATA_LOCK(ifp);
	i = ifp->if_afdata_initialized;
	ifp->if_afdata_initialized = 0;
	IF_AFDATA_UNLOCK(ifp);
	for (dp = domains; i > 0 && dp; dp = dp->dom_next) {
		if (dp->dom_ifdetach && ifp->if_afdata[dp->dom_family])
			(*dp->dom_ifdetach)(ifp,
			    ifp->if_afdata[dp->dom_family]);
	}
}

#ifdef VIMAGE
/*
 * if_vmove() performs a limited version of if_detach() in current
 * vnet and if_attach()es the ifnet to the vnet specified as 2nd arg.
 * An attempt is made to shrink if_index in current vnet, find an
 * unused if_index in target vnet and calls if_grow() if necessary,
 * and finally find an unused if_xname for the target vnet.
 */
void
if_vmove(struct ifnet *ifp, struct vnet *new_vnet)
{
	struct if_clone *ifc;

	/*
	 * Detach from current vnet, but preserve LLADDR info, do not
	 * mark as dead etc. so that the ifnet can be reattached later.
	 */
	if_detach_internal(ifp, 1, &ifc);

	/*
	 * Unlink the ifnet from ifindex_table[] in current vnet, and shrink
	 * the if_index for that vnet if possible.
	 *
	 * NOTE: IFNET_WLOCK/IFNET_WUNLOCK() are assumed to be unvirtualized,
	 * or we'd lock on one vnet and unlock on another.
	 */
	IFNET_WLOCK();
	ifindex_free(ifp->if_index);
	IFNET_WUNLOCK();

	/*
	 * Perform interface-specific reassignment tasks, if provided by
	 * the driver.
	 */
	if (ifp->if_reassign != NULL)
		ifp->if_reassign(ifp, new_vnet, NULL);

	/*
	 * Switch to the context of the target vnet.
	 */
	CURVNET_SET_QUIET(new_vnet);

	IFNET_WLOCK();
	ifp->if_index = ifindex_alloc();
	ifnet_setbyindex_locked(ifp->if_index, ifp);
	IFNET_WUNLOCK();

	if_attach_internal(ifp, 1, ifc);

	CURVNET_RESTORE();
}

/*
 * Move an ifnet to or from another child prison/vnet, specified by the jail id.
 */
static int
if_vmove_loan(struct thread *td, struct ifnet *ifp, char *ifname, int jid)
{
	struct prison *pr;
	struct ifnet *difp;

	/* Try to find the prison within our visibility. */
	sx_slock(&allprison_lock);
	pr = prison_find_child(td->td_ucred->cr_prison, jid);
	sx_sunlock(&allprison_lock);
	if (pr == NULL)
		return (ENXIO);
	prison_hold_locked(pr);
	mtx_unlock(&pr->pr_mtx);

	/* Do not try to move the iface from and to the same prison. */
	if (pr->pr_vnet == ifp->if_vnet) {
		prison_free(pr);
		return (EEXIST);
	}

	/* Make sure the named iface does not exists in the dst. prison/vnet. */
	/* XXX Lock interfaces to avoid races. */
	CURVNET_SET_QUIET(pr->pr_vnet);
	difp = ifunit(ifname);
	CURVNET_RESTORE();
	if (difp != NULL) {
		prison_free(pr);
		return (EEXIST);
	}

	/* Move the interface into the child jail/vnet. */
	if_vmove(ifp, pr->pr_vnet);

	/* Report the new if_xname back to the userland. */
	sprintf(ifname, "%s", ifp->if_xname);

	prison_free(pr);
	return (0);
}

static int
if_vmove_reclaim(struct thread *td, char *ifname, int jid)
{
	struct prison *pr;
	struct vnet *vnet_dst;
	struct ifnet *ifp;

	/* Try to find the prison within our visibility. */
	sx_slock(&allprison_lock);
	pr = prison_find_child(td->td_ucred->cr_prison, jid);
	sx_sunlock(&allprison_lock);
	if (pr == NULL)
		return (ENXIO);
	prison_hold_locked(pr);
	mtx_unlock(&pr->pr_mtx);

	/* Make sure the named iface exists in the source prison/vnet. */
	CURVNET_SET(pr->pr_vnet);
	ifp = ifunit(ifname);		/* XXX Lock to avoid races. */
	if (ifp == NULL) {
		CURVNET_RESTORE();
		prison_free(pr);
		return (ENXIO);
	}

	/* Do not try to move the iface from and to the same prison. */
	vnet_dst = TD_TO_VNET(td);
	if (vnet_dst == ifp->if_vnet) {
		CURVNET_RESTORE();
		prison_free(pr);
		return (EEXIST);
	}

	/* Get interface back from child jail/vnet. */
	if_vmove(ifp, vnet_dst);
	CURVNET_RESTORE();

	/* Report the new if_xname back to the userland. */
	sprintf(ifname, "%s", ifp->if_xname);

	prison_free(pr);
	return (0);
}
#endif /* VIMAGE */

/*
 * Add a group to an interface
 */
int
if_addgroup(struct ifnet *ifp, const char *groupname)
{
	struct ifg_list		*ifgl;
	struct ifg_group	*ifg = NULL;
	struct ifg_member	*ifgm;
	int 			 new = 0;

	if (groupname[0] && groupname[strlen(groupname) - 1] >= '0' &&
	    groupname[strlen(groupname) - 1] <= '9')
		return (EINVAL);

	IFNET_WLOCK();
	TAILQ_FOREACH(ifgl, &ifp->if_groups, ifgl_next)
		if (!strcmp(ifgl->ifgl_group->ifg_group, groupname)) {
			IFNET_WUNLOCK();
			return (EEXIST);
		}

	if ((ifgl = (struct ifg_list *)malloc(sizeof(struct ifg_list), M_TEMP,
	    M_NOWAIT)) == NULL) {
	    	IFNET_WUNLOCK();
		return (ENOMEM);
	}

	if ((ifgm = (struct ifg_member *)malloc(sizeof(struct ifg_member),
	    M_TEMP, M_NOWAIT)) == NULL) {
		free(ifgl, M_TEMP);
		IFNET_WUNLOCK();
		return (ENOMEM);
	}

	TAILQ_FOREACH(ifg, &V_ifg_head, ifg_next)
		if (!strcmp(ifg->ifg_group, groupname))
			break;

	if (ifg == NULL) {
		if ((ifg = (struct ifg_group *)malloc(sizeof(struct ifg_group),
		    M_TEMP, M_NOWAIT)) == NULL) {
			free(ifgl, M_TEMP);
			free(ifgm, M_TEMP);
			IFNET_WUNLOCK();
			return (ENOMEM);
		}
		strlcpy(ifg->ifg_group, groupname, sizeof(ifg->ifg_group));
		ifg->ifg_refcnt = 0;
		TAILQ_INIT(&ifg->ifg_members);
		TAILQ_INSERT_TAIL(&V_ifg_head, ifg, ifg_next);
		new = 1;
	}

	ifg->ifg_refcnt++;
	ifgl->ifgl_group = ifg;
	ifgm->ifgm_ifp = ifp;

	IF_ADDR_WLOCK(ifp);
	TAILQ_INSERT_TAIL(&ifg->ifg_members, ifgm, ifgm_next);
	TAILQ_INSERT_TAIL(&ifp->if_groups, ifgl, ifgl_next);
	IF_ADDR_WUNLOCK(ifp);

	IFNET_WUNLOCK();

	if (new)
		EVENTHANDLER_INVOKE(group_attach_event, ifg);
	EVENTHANDLER_INVOKE(group_change_event, groupname);

	return (0);
}

/*
 * Remove a group from an interface
 */
int
if_delgroup(struct ifnet *ifp, const char *groupname)
{
	struct ifg_list		*ifgl;
	struct ifg_member	*ifgm;

	IFNET_WLOCK();
	TAILQ_FOREACH(ifgl, &ifp->if_groups, ifgl_next)
		if (!strcmp(ifgl->ifgl_group->ifg_group, groupname))
			break;
	if (ifgl == NULL) {
		IFNET_WUNLOCK();
		return (ENOENT);
	}

	IF_ADDR_WLOCK(ifp);
	TAILQ_REMOVE(&ifp->if_groups, ifgl, ifgl_next);
	IF_ADDR_WUNLOCK(ifp);

	TAILQ_FOREACH(ifgm, &ifgl->ifgl_group->ifg_members, ifgm_next)
		if (ifgm->ifgm_ifp == ifp)
			break;

	if (ifgm != NULL) {
		TAILQ_REMOVE(&ifgl->ifgl_group->ifg_members, ifgm, ifgm_next);
		free(ifgm, M_TEMP);
	}

	if (--ifgl->ifgl_group->ifg_refcnt == 0) {
		TAILQ_REMOVE(&V_ifg_head, ifgl->ifgl_group, ifg_next);
		IFNET_WUNLOCK();
		EVENTHANDLER_INVOKE(group_detach_event, ifgl->ifgl_group);
		free(ifgl->ifgl_group, M_TEMP);
	} else
		IFNET_WUNLOCK();

	free(ifgl, M_TEMP);

	EVENTHANDLER_INVOKE(group_change_event, groupname);

	return (0);
}

/*
 * Remove an interface from all groups
 */
static void
if_delgroups(struct ifnet *ifp)
{
	struct ifg_list		*ifgl;
	struct ifg_member	*ifgm;
	char groupname[IFNAMSIZ];

	IFNET_WLOCK();
	while (!TAILQ_EMPTY(&ifp->if_groups)) {
		ifgl = TAILQ_FIRST(&ifp->if_groups);

		strlcpy(groupname, ifgl->ifgl_group->ifg_group, IFNAMSIZ);

		IF_ADDR_WLOCK(ifp);
		TAILQ_REMOVE(&ifp->if_groups, ifgl, ifgl_next);
		IF_ADDR_WUNLOCK(ifp);

		TAILQ_FOREACH(ifgm, &ifgl->ifgl_group->ifg_members, ifgm_next)
			if (ifgm->ifgm_ifp == ifp)
				break;

		if (ifgm != NULL) {
			TAILQ_REMOVE(&ifgl->ifgl_group->ifg_members, ifgm,
			    ifgm_next);
			free(ifgm, M_TEMP);
		}

		if (--ifgl->ifgl_group->ifg_refcnt == 0) {
			TAILQ_REMOVE(&V_ifg_head, ifgl->ifgl_group, ifg_next);
			IFNET_WUNLOCK();
			EVENTHANDLER_INVOKE(group_detach_event,
			    ifgl->ifgl_group);
			free(ifgl->ifgl_group, M_TEMP);
		} else
			IFNET_WUNLOCK();

		free(ifgl, M_TEMP);

		EVENTHANDLER_INVOKE(group_change_event, groupname);

		IFNET_WLOCK();
	}
	IFNET_WUNLOCK();
}

/*
 * Stores all groups from an interface in memory pointed
 * to by data
 */
static int
if_getgroup(struct ifgroupreq *data, struct ifnet *ifp)
{
	int			 len, error;
	struct ifg_list		*ifgl;
	struct ifg_req		 ifgrq, *ifgp;
	struct ifgroupreq	*ifgr = data;

	if (ifgr->ifgr_len == 0) {
		IF_ADDR_RLOCK(ifp);
		TAILQ_FOREACH(ifgl, &ifp->if_groups, ifgl_next)
			ifgr->ifgr_len += sizeof(struct ifg_req);
		IF_ADDR_RUNLOCK(ifp);
		return (0);
	}

	len = ifgr->ifgr_len;
	ifgp = ifgr->ifgr_groups;
	/* XXX: wire */
	IF_ADDR_RLOCK(ifp);
	TAILQ_FOREACH(ifgl, &ifp->if_groups, ifgl_next) {
		if (len < sizeof(ifgrq)) {
			IF_ADDR_RUNLOCK(ifp);
			return (EINVAL);
		}
		bzero(&ifgrq, sizeof ifgrq);
		strlcpy(ifgrq.ifgrq_group, ifgl->ifgl_group->ifg_group,
		    sizeof(ifgrq.ifgrq_group));
		if ((error = copyout(&ifgrq, ifgp, sizeof(struct ifg_req)))) {
		    	IF_ADDR_RUNLOCK(ifp);
			return (error);
		}
		len -= sizeof(ifgrq);
		ifgp++;
	}
	IF_ADDR_RUNLOCK(ifp);

	return (0);
}

/*
 * Stores all members of a group in memory pointed to by data
 */
static int
if_getgroupmembers(struct ifgroupreq *data)
{
	struct ifgroupreq	*ifgr = data;
	struct ifg_group	*ifg;
	struct ifg_member	*ifgm;
	struct ifg_req		 ifgrq, *ifgp;
	int			 len, error;

	IFNET_RLOCK();
	TAILQ_FOREACH(ifg, &V_ifg_head, ifg_next)
		if (!strcmp(ifg->ifg_group, ifgr->ifgr_name))
			break;
	if (ifg == NULL) {
		IFNET_RUNLOCK();
		return (ENOENT);
	}

	if (ifgr->ifgr_len == 0) {
		TAILQ_FOREACH(ifgm, &ifg->ifg_members, ifgm_next)
			ifgr->ifgr_len += sizeof(ifgrq);
		IFNET_RUNLOCK();
		return (0);
	}

	len = ifgr->ifgr_len;
	ifgp = ifgr->ifgr_groups;
	TAILQ_FOREACH(ifgm, &ifg->ifg_members, ifgm_next) {
		if (len < sizeof(ifgrq)) {
			IFNET_RUNLOCK();
			return (EINVAL);
		}
		bzero(&ifgrq, sizeof ifgrq);
		strlcpy(ifgrq.ifgrq_member, ifgm->ifgm_ifp->if_xname,
		    sizeof(ifgrq.ifgrq_member));
		if ((error = copyout(&ifgrq, ifgp, sizeof(struct ifg_req)))) {
			IFNET_RUNLOCK();
			return (error);
		}
		len -= sizeof(ifgrq);
		ifgp++;
	}
	IFNET_RUNLOCK();

	return (0);
}

/*
 * Returning different software contexts associated with ifnet.
 */
void *
if_getsoftc(struct ifnet *ifp, ift_feature f)
{
	struct ifsoftc *sc;

	/*
	 * Some softcs are non-optional either for performance reasons,
	 * since they always exist and are often dereferenced, or for
	 * historical reasons.
	 */
	switch (f) {
	case IF_DRIVER_SOFTC:
		return (ifp->if_softc);
	case IF_LLADDR:
		return (LLADDR((struct sockaddr_dl *)(ifp->if_addr->ifa_addr)));
	case IF_BPF:
		return (ifp->if_bpf);
	case IF_NAME:
		return (ifp->if_xname);
	case IF_VLAN:
		return (ifp->if_vlantrunk);
	default:
		/* fall through */
		;
	};

	/*
	 * Rest of softc live in the store and in the cache.
	 * First check the cache.
	 */
	sc = ifp->if_sccache[f & (SOFTC_CACHE_SIZE - 1)];
	if (sc != NULL && sc->ifsc_desc == f)
		return (sc->ifsc_ptr);

	/*
	 * Then check the store.
	 * We can do lookup lockless, since if_nsoftcs only grows.
	 */
	for (int i = 0; i < ifp->if_nsoftcs; i++) {
		sc = &ifp->if_scstore[i];
		if (sc->ifsc_desc == f) {
			ifp->if_sccache[f & (SOFTC_CACHE_SIZE - 1)] = sc;
			return (sc->ifsc_ptr);
		}
	}

	/*
	 * XXXGL: a negative cache would be not bad.
	 */
	return (NULL);
}

/*
 * Set arbitrary context identified by ift_feature key.  It is responsibility
 * of the caller to establish race safety against two if_setsoftc()s.  The
 * function may sleep when setting new context.  The function will not sleep
 * when clearing previously set context.  May fail only if associated context
 * is already set.
 */
int
if_setsoftc(struct ifnet *ifp, ift_feature f, void *softc)
{
	int i;

	IF_WLOCK(ifp);
retry:
	for (i = 0; i < ifp->if_nsoftcs; i++)
		if (ifp->if_scstore[i].ifsc_desc == f) {
			IF_WUNLOCK(ifp);
			return (EEXIST);
		}

	for (i = 0; i < ifp->if_nsoftcs; i++)
		if (ifp->if_scstore[i].ifsc_desc == 0)
			break;

	if (i == ifp->if_nsoftcs) {
		struct ifsoftc *new, *old;
		u_int size;

		old = ifp->if_scstore;
		size = ifp->if_nsoftcs;
		IF_WUNLOCK(ifp);
		new = malloc(sizeof(struct ifsoftc) * size * 2,
		    M_IFNET, M_WAITOK | M_ZERO);
		IF_WLOCK(ifp);
		if (ifp->if_scstore != old) {
			free(new, M_IFNET);
			goto retry;
		}
		bcopy(ifp->if_scstore, new, sizeof(struct ifsoftc) * size);
		ifp->if_scstore = new;
		ifp->if_nsoftcs = size * 2;
		/*
		 * XXXGL: of course there is a race here against if_getsoftc(),
		 * which runs lockless.  We lack RCU or lightweight reference
		 * counting.
		 */
		free(old, M_IFNET);
	}

	if (softc != NULL) {
		ifp->if_scstore[i].ifsc_ptr = softc;
		ifp->if_scstore[i].ifsc_desc = f;
		ifp->if_sccache[f & (SOFTC_CACHE_SIZE - 1)] =
		    &ifp->if_scstore[i];
	} else {
		ifp->if_scstore[i].ifsc_desc = 0;
		ifp->if_scstore[i].ifsc_ptr = NULL;
		ifp->if_sccache[f & (SOFTC_CACHE_SIZE - 1)] = NULL;
	}
	IF_WUNLOCK(ifp);
	return (0);
}

/*
 * Return counter values from counter(9)s stored in ifnet.
 */
uint64_t
if_get_counter_default(struct ifnet *ifp, ift_counter cnt)
{

	KASSERT(cnt < IFCOUNTERS, ("%s: invalid cnt %d", __func__, cnt));

	return (counter_u64_fetch(ifp->if_counters[cnt]));
}

/*
 * Increase an ifnet counter. Usually used for counters shared
 * between the stack and a driver, but function supports them all.
 */
void
if_inc_counter(struct ifnet *ifp, ift_counter cnt, int64_t inc)
{

	KASSERT(cnt < IFCOUNTERS, ("%s: invalid cnt %d", __func__, cnt));

	counter_u64_add(ifp->if_counters[cnt], inc);
}

/*
 * Account successful transmission of an mbuf.
 */
void
if_inc_txcounters(struct ifnet *ifp, struct mbuf *m)
{

	counter_u64_add(ifp->if_counters[IFCOUNTER_OBYTES], m->m_pkthdr.len);
	counter_u64_add(ifp->if_counters[IFCOUNTER_OPACKETS], 1);
	if (m->m_flags & M_MCAST)
		counter_u64_add(ifp->if_counters[IFCOUNTER_OMCASTS], 1);
}

/*
 * Set the baudrate.
 */
void
if_setbaudrate(struct ifnet *ifp, uint64_t baudrate)
{

	ifp->if_baudrate = baudrate;
}

/*
 * Copy data from ifnet to userland API structure if_data.
 */
void
if_data_copy(struct ifnet *ifp, struct if_data *ifd)
{

	ifd->ifi_type = if_type(ifp);
	ifd->ifi_physical = 0;
	ifd->ifi_addrlen = if_addrlen(ifp);
	ifd->ifi_hdrlen = ifp->if_drv->ifdrv_hdrlen;
	ifd->ifi_link_state = ifp->if_link_state;
	ifd->ifi_vhid = 0;
	ifd->ifi_datalen = sizeof(struct if_data);
	ifd->ifi_mtu = ifp->if_mtu;
	ifd->ifi_metric = ifp->if_metric;
	ifd->ifi_baudrate = ifp->if_baudrate;
	ifd->ifi_hwassist = ifp->if_hwassist;
	ifd->ifi_epoch = ifp->if_epoch;
	ifd->ifi_lastchange = ifp->if_lastchange;

	ifd->ifi_ipackets = if_get_counter(ifp, IFCOUNTER_IPACKETS);
	ifd->ifi_ierrors = if_get_counter(ifp, IFCOUNTER_IERRORS);
	ifd->ifi_opackets = if_get_counter(ifp, IFCOUNTER_OPACKETS);
	ifd->ifi_oerrors = if_get_counter(ifp, IFCOUNTER_OERRORS);
	ifd->ifi_collisions = if_get_counter(ifp, IFCOUNTER_COLLISIONS);
	ifd->ifi_ibytes = if_get_counter(ifp, IFCOUNTER_IBYTES);
	ifd->ifi_obytes = if_get_counter(ifp, IFCOUNTER_OBYTES);
	ifd->ifi_imcasts = if_get_counter(ifp, IFCOUNTER_IMCASTS);
	ifd->ifi_omcasts = if_get_counter(ifp, IFCOUNTER_OMCASTS);
	ifd->ifi_iqdrops = if_get_counter(ifp, IFCOUNTER_IQDROPS);
	ifd->ifi_oqdrops = if_get_counter(ifp, IFCOUNTER_OQDROPS);
	ifd->ifi_noproto = if_get_counter(ifp, IFCOUNTER_NOPROTO);
}

/*
 * Initialization, destruction and refcounting functions for ifaddrs.
 */
struct ifaddr *
ifa_alloc(size_t size, int flags)
{
	struct ifaddr *ifa;

	KASSERT(size >= sizeof(struct ifaddr),
	    ("%s: invalid size %zu", __func__, size));

	ifa = malloc(size, M_IFADDR, M_ZERO | flags);
	if (ifa == NULL)
		return (NULL);

	if ((ifa->ifa_opackets = counter_u64_alloc(flags)) == NULL)
		goto fail;
	if ((ifa->ifa_ipackets = counter_u64_alloc(flags)) == NULL)
		goto fail;
	if ((ifa->ifa_obytes = counter_u64_alloc(flags)) == NULL)
		goto fail;
	if ((ifa->ifa_ibytes = counter_u64_alloc(flags)) == NULL)
		goto fail;

	refcount_init(&ifa->ifa_refcnt, 1);

	return (ifa);

fail:
	/* free(NULL) is okay */
	counter_u64_free(ifa->ifa_opackets);
	counter_u64_free(ifa->ifa_ipackets);
	counter_u64_free(ifa->ifa_obytes);
	counter_u64_free(ifa->ifa_ibytes);
	free(ifa, M_IFADDR);

	return (NULL);
}

void
ifa_ref(struct ifaddr *ifa)
{

	refcount_acquire(&ifa->ifa_refcnt);
}

void
ifa_free(struct ifaddr *ifa)
{

	if (refcount_release(&ifa->ifa_refcnt)) {
		counter_u64_free(ifa->ifa_opackets);
		counter_u64_free(ifa->ifa_ipackets);
		counter_u64_free(ifa->ifa_obytes);
		counter_u64_free(ifa->ifa_ibytes);
		free(ifa, M_IFADDR);
	}
}

int
ifa_add_loopback_route(struct ifaddr *ifa, struct sockaddr *ia)
{
	int error = 0;
	struct rtentry *rt = NULL;
	struct rt_addrinfo info;
	static struct sockaddr_dl null_sdl = {sizeof(null_sdl), AF_LINK};

	bzero(&info, sizeof(info));
	info.rti_ifp = V_loif;
	info.rti_flags = ifa->ifa_flags | RTF_HOST | RTF_STATIC;
	info.rti_info[RTAX_DST] = ia;
	info.rti_info[RTAX_GATEWAY] = (struct sockaddr *)&null_sdl;
	error = rtrequest1_fib(RTM_ADD, &info, &rt, ifa->ifa_ifp->if_fib);

	if (error == 0 && rt != NULL) {
		RT_LOCK(rt);
		((struct sockaddr_dl *)rt->rt_gateway)->sdl_type =
		    if_type(ifa->ifa_ifp);
		((struct sockaddr_dl *)rt->rt_gateway)->sdl_index =
		    ifa->ifa_ifp->if_index;
		RT_REMREF(rt);
		RT_UNLOCK(rt);
	} else if (error != 0)
		log(LOG_DEBUG, "%s: insertion failed: %u\n", __func__, error);

	return (error);
}

int
ifa_del_loopback_route(struct ifaddr *ifa, struct sockaddr *ia)
{
	int error = 0;
	struct rt_addrinfo info;
	struct sockaddr_dl null_sdl;

	bzero(&null_sdl, sizeof(null_sdl));
	null_sdl.sdl_len = sizeof(null_sdl);
	null_sdl.sdl_family = AF_LINK;
	null_sdl.sdl_type = if_type(ifa->ifa_ifp);
	null_sdl.sdl_index = ifa->ifa_ifp->if_index;
	bzero(&info, sizeof(info));
	info.rti_flags = ifa->ifa_flags | RTF_HOST | RTF_STATIC;
	info.rti_info[RTAX_DST] = ia;
	info.rti_info[RTAX_GATEWAY] = (struct sockaddr *)&null_sdl;
	error = rtrequest1_fib(RTM_DELETE, &info, NULL, ifa->ifa_ifp->if_fib);

	if (error != 0)
		log(LOG_DEBUG, "%s: deletion failed: %u\n", __func__, error);

	return (error);
}

int
ifa_switch_loopback_route(struct ifaddr *ifa, struct sockaddr *sa, int fib)
{
	struct rtentry *rt;

	rt = rtalloc1_fib(sa, 0, 0, fib);
	if (rt == NULL) {
		log(LOG_DEBUG, "%s: fail", __func__);
		return (EHOSTUNREACH);
	}
	((struct sockaddr_dl *)rt->rt_gateway)->sdl_type =
	    if_type(ifa->ifa_ifp);
	((struct sockaddr_dl *)rt->rt_gateway)->sdl_index =
	    ifa->ifa_ifp->if_index;
	RTFREE_LOCKED(rt);

	return (0);
}

/*
 * XXX: Because sockaddr_dl has deeper structure than the sockaddr
 * structs used to represent other address families, it is necessary
 * to perform a different comparison.
 */

#define	sa_dl_equal(a1, a2)	\
	((((struct sockaddr_dl *)(a1))->sdl_len ==			\
	 ((struct sockaddr_dl *)(a2))->sdl_len) &&			\
	 (bcmp(LLADDR((struct sockaddr_dl *)(a1)),			\
	       LLADDR((struct sockaddr_dl *)(a2)),			\
	       ((struct sockaddr_dl *)(a1))->sdl_alen) == 0))

/*
 * Locate an interface based on a complete address.
 */
/*ARGSUSED*/
static struct ifaddr *
ifa_ifwithaddr_internal(struct sockaddr *addr, int getref)
{
	struct ifnet *ifp;
	struct ifaddr *ifa;

	IFNET_RLOCK_NOSLEEP();
	TAILQ_FOREACH(ifp, &V_ifnet, if_link) {
		IF_ADDR_RLOCK(ifp);
		TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
			if (ifa->ifa_addr->sa_family != addr->sa_family)
				continue;
			if (sa_equal(addr, ifa->ifa_addr)) {
				if (getref)
					ifa_ref(ifa);
				IF_ADDR_RUNLOCK(ifp);
				goto done;
			}
			/* IP6 doesn't have broadcast */
			if ((ifp->if_flags & IFF_BROADCAST) &&
			    ifa->ifa_broadaddr &&
			    ifa->ifa_broadaddr->sa_len != 0 &&
			    sa_equal(ifa->ifa_broadaddr, addr)) {
				if (getref)
					ifa_ref(ifa);
				IF_ADDR_RUNLOCK(ifp);
				goto done;
			}
		}
		IF_ADDR_RUNLOCK(ifp);
	}
	ifa = NULL;
done:
	IFNET_RUNLOCK_NOSLEEP();
	return (ifa);
}

struct ifaddr *
ifa_ifwithaddr(struct sockaddr *addr)
{

	return (ifa_ifwithaddr_internal(addr, 1));
}

int
ifa_ifwithaddr_check(struct sockaddr *addr)
{

	return (ifa_ifwithaddr_internal(addr, 0) != NULL);
}

/*
 * Locate an interface based on the broadcast address.
 */
/* ARGSUSED */
struct ifaddr *
ifa_ifwithbroadaddr(struct sockaddr *addr, int fibnum)
{
	struct ifnet *ifp;
	struct ifaddr *ifa;

	IFNET_RLOCK_NOSLEEP();
	TAILQ_FOREACH(ifp, &V_ifnet, if_link) {
		if ((fibnum != RT_ALL_FIBS) && (ifp->if_fib != fibnum))
			continue;
		IF_ADDR_RLOCK(ifp);
		TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
			if (ifa->ifa_addr->sa_family != addr->sa_family)
				continue;
			if ((ifp->if_flags & IFF_BROADCAST) &&
			    ifa->ifa_broadaddr &&
			    ifa->ifa_broadaddr->sa_len != 0 &&
			    sa_equal(ifa->ifa_broadaddr, addr)) {
				ifa_ref(ifa);
				IF_ADDR_RUNLOCK(ifp);
				goto done;
			}
		}
		IF_ADDR_RUNLOCK(ifp);
	}
	ifa = NULL;
done:
	IFNET_RUNLOCK_NOSLEEP();
	return (ifa);
}

/*
 * Locate the point to point interface with a given destination address.
 */
/*ARGSUSED*/
struct ifaddr *
ifa_ifwithdstaddr(struct sockaddr *addr, int fibnum)
{
	struct ifnet *ifp;
	struct ifaddr *ifa;

	IFNET_RLOCK_NOSLEEP();
	TAILQ_FOREACH(ifp, &V_ifnet, if_link) {
		if ((ifp->if_flags & IFF_POINTOPOINT) == 0)
			continue;
		if ((fibnum != RT_ALL_FIBS) && (ifp->if_fib != fibnum))
			continue;
		IF_ADDR_RLOCK(ifp);
		TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
			if (ifa->ifa_addr->sa_family != addr->sa_family)
				continue;
			if (ifa->ifa_dstaddr != NULL &&
			    sa_equal(addr, ifa->ifa_dstaddr)) {
				ifa_ref(ifa);
				IF_ADDR_RUNLOCK(ifp);
				goto done;
			}
		}
		IF_ADDR_RUNLOCK(ifp);
	}
	ifa = NULL;
done:
	IFNET_RUNLOCK_NOSLEEP();
	return (ifa);
}

/*
 * Find an interface on a specific network.  If many, choice
 * is most specific found.
 */
struct ifaddr *
ifa_ifwithnet(struct sockaddr *addr, int ignore_ptp, int fibnum)
{
	struct ifnet *ifp;
	struct ifaddr *ifa;
	struct ifaddr *ifa_maybe = NULL;
	u_int af = addr->sa_family;
	char *addr_data = addr->sa_data, *cplim;

	/*
	 * AF_LINK addresses can be looked up directly by their index number,
	 * so do that if we can.
	 */
	if (af == AF_LINK) {
	    struct sockaddr_dl *sdl = (struct sockaddr_dl *)addr;
	    if (sdl->sdl_index && sdl->sdl_index <= V_if_index)
		return (ifaddr_byindex(sdl->sdl_index));
	}

	/*
	 * Scan though each interface, looking for ones that have addresses
	 * in this address family and the requested fib.  Maintain a reference
	 * on ifa_maybe once we find one, as we release the IF_ADDR_RLOCK() that
	 * kept it stable when we move onto the next interface.
	 */
	IFNET_RLOCK_NOSLEEP();
	TAILQ_FOREACH(ifp, &V_ifnet, if_link) {
		if ((fibnum != RT_ALL_FIBS) && (ifp->if_fib != fibnum))
			continue;
		IF_ADDR_RLOCK(ifp);
		TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
			char *cp, *cp2, *cp3;

			if (ifa->ifa_addr->sa_family != af)
next:				continue;
			if (af == AF_INET && 
			    ifp->if_flags & IFF_POINTOPOINT && !ignore_ptp) {
				/*
				 * This is a bit broken as it doesn't
				 * take into account that the remote end may
				 * be a single node in the network we are
				 * looking for.
				 * The trouble is that we don't know the
				 * netmask for the remote end.
				 */
				if (ifa->ifa_dstaddr != NULL &&
				    sa_equal(addr, ifa->ifa_dstaddr)) {
					ifa_ref(ifa);
					IF_ADDR_RUNLOCK(ifp);
					goto done;
				}
			} else {
				/*
				 * Scan all the bits in the ifa's address.
				 * If a bit dissagrees with what we are
				 * looking for, mask it with the netmask
				 * to see if it really matters.
				 * (A byte at a time)
				 */
				if (ifa->ifa_netmask == 0)
					continue;
				cp = addr_data;
				cp2 = ifa->ifa_addr->sa_data;
				cp3 = ifa->ifa_netmask->sa_data;
				cplim = ifa->ifa_netmask->sa_len
					+ (char *)ifa->ifa_netmask;
				while (cp3 < cplim)
					if ((*cp++ ^ *cp2++) & *cp3++)
						goto next; /* next address! */
				/*
				 * If the netmask of what we just found
				 * is more specific than what we had before
				 * (if we had one), or if the virtual status
				 * of new prefix is better than of the old one,
				 * then remember the new one before continuing
				 * to search for an even better one.
				 */
				if (ifa_maybe == NULL ||
				    ifa_preferred(ifa_maybe, ifa) ||
				    rn_refines((caddr_t)ifa->ifa_netmask,
				    (caddr_t)ifa_maybe->ifa_netmask)) {
					if (ifa_maybe != NULL)
						ifa_free(ifa_maybe);
					ifa_maybe = ifa;
					ifa_ref(ifa_maybe);
				}
			}
		}
		IF_ADDR_RUNLOCK(ifp);
	}
	ifa = ifa_maybe;
	ifa_maybe = NULL;
done:
	IFNET_RUNLOCK_NOSLEEP();
	if (ifa_maybe != NULL)
		ifa_free(ifa_maybe);
	return (ifa);
}

/*
 * Find an interface address specific to an interface best matching
 * a given address.
 */
struct ifaddr *
ifaof_ifpforaddr(struct sockaddr *addr, struct ifnet *ifp)
{
	struct ifaddr *ifa;
	char *cp, *cp2, *cp3;
	char *cplim;
	struct ifaddr *ifa_maybe = NULL;
	u_int af = addr->sa_family;

	if (af >= AF_MAX)
		return (NULL);
	IF_ADDR_RLOCK(ifp);
	TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
		if (ifa->ifa_addr->sa_family != af)
			continue;
		if (ifa_maybe == NULL)
			ifa_maybe = ifa;
		if (ifa->ifa_netmask == 0) {
			if (sa_equal(addr, ifa->ifa_addr) ||
			    (ifa->ifa_dstaddr &&
			    sa_equal(addr, ifa->ifa_dstaddr)))
				goto done;
			continue;
		}
		if (ifp->if_flags & IFF_POINTOPOINT) {
			if (sa_equal(addr, ifa->ifa_dstaddr))
				goto done;
		} else {
			cp = addr->sa_data;
			cp2 = ifa->ifa_addr->sa_data;
			cp3 = ifa->ifa_netmask->sa_data;
			cplim = ifa->ifa_netmask->sa_len + (char *)ifa->ifa_netmask;
			for (; cp3 < cplim; cp3++)
				if ((*cp++ ^ *cp2++) & *cp3)
					break;
			if (cp3 == cplim)
				goto done;
		}
	}
	ifa = ifa_maybe;
done:
	if (ifa != NULL)
		ifa_ref(ifa);
	IF_ADDR_RUNLOCK(ifp);
	return (ifa);
}

/*
 * See whether new ifa is better than current one:
 * 1) A non-virtual one is preferred over virtual.
 * 2) A virtual in master state preferred over any other state.
 *
 * Used in several address selecting functions.
 */
int
ifa_preferred(struct ifaddr *cur, struct ifaddr *next)
{

	return (cur->ifa_carp && (!next->ifa_carp ||
	    ((*carp_master_p)(next) && !(*carp_master_p)(cur))));
}

#include <net/if_llatbl.h>

/*
 * Default action when installing a route with a Link Level gateway.
 * Lookup an appropriate real ifa to point to.
 * This should be moved to /sys/net/link.c eventually.
 */
static void
link_rtrequest(int cmd, struct rtentry *rt, struct rt_addrinfo *info)
{
	struct ifaddr *ifa, *oifa;
	struct sockaddr *dst;
	struct ifnet *ifp;

	if (cmd != RTM_ADD || ((ifa = rt->rt_ifa) == 0) ||
	    ((ifp = ifa->ifa_ifp) == 0) || ((dst = rt_key(rt)) == 0))
		return;
	ifa = ifaof_ifpforaddr(dst, ifp);
	if (ifa) {
		oifa = rt->rt_ifa;
		rt->rt_ifa = ifa;
		ifa_free(oifa);
		if (ifa->ifa_rtrequest && ifa->ifa_rtrequest != link_rtrequest)
			ifa->ifa_rtrequest(cmd, rt, info);
	}
}

struct sockaddr_dl *
link_alloc_sdl(size_t size, int flags)
{

	return (malloc(size, M_TEMP, flags));
}

void
link_free_sdl(struct sockaddr *sa)
{
	free(sa, M_TEMP);
}

/*
 * Fills in given sdl with interface basic info.
 * Returns pointer to filled sdl.
 */
struct sockaddr_dl *
link_init_sdl(struct ifnet *ifp, struct sockaddr *paddr, u_char iftype)
{
	struct sockaddr_dl *sdl;

	sdl = (struct sockaddr_dl *)paddr;
	memset(sdl, 0, sizeof(struct sockaddr_dl));
	sdl->sdl_len = sizeof(struct sockaddr_dl);
	sdl->sdl_family = AF_LINK;
	sdl->sdl_index = ifp->if_index;
	sdl->sdl_type = iftype;

	return (sdl);
}

/*
 * Function pointers to vlan(4) module.
 * XXXGL: shouldn't we just make vlan(4) always in kernel?
 */
void	(*vlan_link_state_p)(struct ifnet *);
void	(*vlan_trunk_cap_p)(struct ifnet *);
struct ifnet *(*vlan_trunkdev_p)(struct ifnet *);
struct ifnet *(*vlan_dev_p)(struct ifnet *, uint16_t);
uint16_t (*vlan_vid_p)(struct ifnet *);

/*
 * Handle a change in the interface link state. To avoid LORs
 * between driver lock and upper layer locks, as well as possible
 * recursions, we post event to taskqueue, and all job
 * is done in static do_link_state_change().
 */
void
if_link_state_change(struct ifnet *ifp, int link_state)
{
	/* Return if state hasn't changed. */
	if (ifp->if_link_state == link_state)
		return;

	ifp->if_link_state = link_state;

	taskqueue_enqueue(taskqueue_swi, &ifp->if_linktask);
}

static void
do_link_state_change(void *arg, int pending)
{
	struct ifnet *ifp = (struct ifnet *)arg;
	int link_state = ifp->if_link_state;
	CURVNET_SET(ifp->if_vnet);

	/* Notify that the link state has changed. */
	rt_ifmsg(ifp);
	if (ifp->if_vlantrunk != NULL)
		(*vlan_link_state_p)(ifp);

	/* XXXGL: make ng_ether softc pointer */
	if ((if_type(ifp) == IFT_ETHER || if_type(ifp) == IFT_L2VLAN) &&
	    ifp->if_l2com != NULL)
		(*ng_ether_link_state_p)(ifp, link_state);
	if (if_getsoftc(ifp, IF_CARP) != NULL)
		(*carp_linkstate_p)(ifp);
	if (ifp->if_bridge)
		(*bridge_linkstate_p)(ifp);
	if (ifp->if_lagg)
		(*lagg_linkstate_p)(ifp, link_state);

	if (IS_DEFAULT_VNET(curvnet))
		devctl_notify("IFNET", ifp->if_xname,
		    (link_state == LINK_STATE_UP) ? "LINK_UP" : "LINK_DOWN",
		    NULL);
	if (pending > 1)
		if_printf(ifp, "%d link states coalesced\n", pending);
	if (log_link_state_change)
		log(LOG_NOTICE, "%s: link state changed to %s\n", ifp->if_xname,
		    (link_state == LINK_STATE_UP) ? "UP" : "DOWN" );
	EVENTHANDLER_INVOKE(ifnet_link_event, ifp, ifp->if_link_state);
	CURVNET_RESTORE();
}

/*
 * Mark an interface down and notify protocols of
 * the transition.
 */
void
if_down(struct ifnet *ifp)
{
	struct ifaddr *ifa;

	ifp->if_flags &= ~IFF_UP;
	getmicrotime(&ifp->if_lastchange);
	TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link)
		pfctlinput(PRC_IFDOWN, ifa->ifa_addr);
	if_qflush(ifp);
	if (if_getsoftc(ifp, IF_CARP) != NULL)
		(*carp_linkstate_p)(ifp);
	rt_ifmsg(ifp);
}

/*
 * Mark an interface up and notify protocols of
 * the transition.
 */
void
if_up(struct ifnet *ifp)
{
	struct ifaddr *ifa;

	ifp->if_flags |= IFF_UP;
	getmicrotime(&ifp->if_lastchange);
	TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link)
		pfctlinput(PRC_IFUP, ifa->ifa_addr);
	if (if_getsoftc(ifp, IF_CARP) != NULL)
		(*carp_linkstate_p)(ifp);
	rt_ifmsg(ifp);
#ifdef INET6
	in6_if_up(ifp);
#endif
}

/*
 * Map interface name to interface structure pointer, with or without
 * returning a reference.
 */
struct ifnet *
ifunit_ref(const char *name)
{
	struct ifnet *ifp;

	IFNET_RLOCK_NOSLEEP();
	TAILQ_FOREACH(ifp, &V_ifnet, if_link) {
		if (strncmp(name, ifp->if_xname, IFNAMSIZ) == 0 &&
		    !(ifp->if_flags & IFF_DYING))
			break;
	}
	if (ifp != NULL)
		if_ref(ifp);
	IFNET_RUNLOCK_NOSLEEP();
	return (ifp);
}

struct ifnet *
ifunit(const char *name)
{
	struct ifnet *ifp;

	IFNET_RLOCK_NOSLEEP();
	TAILQ_FOREACH(ifp, &V_ifnet, if_link) {
		if (strncmp(name, ifp->if_xname, IFNAMSIZ) == 0)
			break;
	}
	IFNET_RUNLOCK_NOSLEEP();
	return (ifp);
}

/*
 * Hardware specific interface ioctls.
 */
int
if_drvioctl(struct ifnet *ifp, u_long cmd, void *data, struct thread *td)
{
	struct ifreq *ifr;
	size_t namelen, onamelen;
	size_t descrlen;
	char *descrbuf, *odescrbuf;
	char new_name[IFNAMSIZ];
	struct ifaddr *ifa;
	struct sockaddr_dl *sdl;
	uint32_t flags, oflags;
	int error = 0;

	ifr = (struct ifreq *)data;
	switch (cmd) {
	case SIOCGIFINDEX:
		ifr->ifr_index = ifp->if_index;
		break;

	case SIOCGIFFLAGS:
		ifr->ifr_flags = ifp->if_flags & 0xffff;
		ifr->ifr_flagshigh = ifp->if_flags >> 16;
		/*
		 * Some software may care about IFF_RUNNING, so make
		 * it happy.
		 */
		if (ifp->if_flags & IFF_UP)
			ifr->ifr_flags |= IFF_RUNNING;
		break;

	case SIOCGIFCAP:
		ifr->ifr_reqcap = ifp->if_capabilities;
		ifr->ifr_curcap = ifp->if_capenable;
		break;

#ifdef MAC
	case SIOCGIFMAC:
		error = mac_ifnet_ioctl_get(td->td_ucred, ifr, ifp);
		break;
#endif

	case SIOCGIFMETRIC:
		ifr->ifr_metric = ifp->if_metric;
		break;

	case SIOCGIFMTU:
		ifr->ifr_mtu = ifp->if_mtu;
		break;

	case SIOCGIFPHYS:
		/* XXXGL: did this ever worked? */
		ifr->ifr_phys = 0;
		break;

	case SIOCGIFDESCR:
		error = 0;
		sx_slock(&ifdescr_sx);
		if (ifp->if_description == NULL)
			error = ENOMSG;
		else {
			/* space for terminating nul */
			descrlen = strlen(ifp->if_description) + 1;
			if (ifr->ifr_buffer.length < descrlen)
				ifr->ifr_buffer.buffer = NULL;
			else
				error = copyout(ifp->if_description,
				    ifr->ifr_buffer.buffer, descrlen);
			ifr->ifr_buffer.length = descrlen;
		}
		sx_sunlock(&ifdescr_sx);
		break;

	case SIOCSIFDESCR:
		error = priv_check(td, PRIV_NET_SETIFDESCR);
		if (error)
			return (error);

		/*
		 * Copy only (length-1) bytes to make sure that
		 * if_description is always nul terminated.  The
		 * length parameter is supposed to count the
		 * terminating nul in.
		 */
		if (ifr->ifr_buffer.length > ifdescr_maxlen)
			return (ENAMETOOLONG);
		else if (ifr->ifr_buffer.length == 0)
			descrbuf = NULL;
		else {
			descrbuf = malloc(ifr->ifr_buffer.length, M_IFDESCR,
			    M_WAITOK | M_ZERO);
			error = copyin(ifr->ifr_buffer.buffer, descrbuf,
			    ifr->ifr_buffer.length - 1);
			if (error) {
				free(descrbuf, M_IFDESCR);
				break;
			}
		}

		sx_xlock(&ifdescr_sx);
		odescrbuf = ifp->if_description;
		ifp->if_description = descrbuf;
		sx_xunlock(&ifdescr_sx);

		getmicrotime(&ifp->if_lastchange);
		free(odescrbuf, M_IFDESCR);
		break;

	case SIOCGIFFIB:
		ifr->ifr_fib = ifp->if_fib;
		break;

	case SIOCSIFFIB:
		error = priv_check(td, PRIV_NET_SETIFFIB);
		if (error)
			return (error);
		if (ifr->ifr_fib >= rt_numfibs)
			return (EINVAL);
		ifp->if_fib = ifr->ifr_fib;
		(void )if_ioctl(ifp, cmd, data, td);
		break;

	case SIOCSIFFLAGS:
		error = priv_check(td, PRIV_NET_SETIFFLAGS);
		if (error)
			return (error);
		/*
		 * Historically if_flags were 16-bit, and thus
		 * they come from userland in two parts, that
		 * we need to swap.  Clear IFF_RUNNING that is
		 * no longer used in kernel.
		 */
		ifr->ifr_flags &= ~IFF_RUNNING;
		flags = (ifr->ifr_flags & 0xffff) |
		    (ifr->ifr_flagshigh << 16);
		if ((flags & IFF_CANTCHANGE) !=
		    (ifp->if_flags & IFF_CANTCHANGE))
			return (EINVAL);
		/*
		 * Pass new flags down to driver and see if it accepts them.
		 */
		error = if_ioctl(ifp, cmd, data, td);
		if (error)
			return (error);
		flags = (ifr->ifr_flags & 0xffff) |
		    (ifr->ifr_flagshigh << 16);
		oflags = ifp->if_flags;
		ifp->if_flags = flags;
		getmicrotime(&ifp->if_lastchange);
		/*
		 * Manage IFF_UP flip.
		 */
		if (oflags & IFF_UP && (flags & IFF_UP) == 0)
			if_down(ifp);
		else if (flags & IFF_UP && (oflags & IFF_UP) == 0)
			if_up(ifp);
		/* See if permanently promiscuous mode bit is about to flip. */
		if ((oflags ^ flags) & IFF_PPROMISC) {
			if (flags & IFF_PPROMISC)
				ifp->if_flags |= IFF_PROMISC;
			else if (ifp->if_pcount == 0)
				ifp->if_flags &= ~IFF_PROMISC;
			log(LOG_INFO, "%s: permanently promiscuous mode %s\n",
			    ifp->if_xname,
			    (flags & IFF_PPROMISC) ? "enabled" : "disabled");
		}
		break;

	case SIOCSIFCAP:
		error = priv_check(td, PRIV_NET_SETIFCAP);
		if (error)
			return (error);
		/*
		 * All(?) NICs that do TSO require to perform VLAN tagging
		 * and checksum offloading in hardware, when doing TSO.
		 * Thus, turning TSO on implicitly turns on these features,
		 * and turning these features off implicitly turns off TSO.
		 */
		if ((ifr->ifr_reqcap & IFCAP_VLAN_HWTSO) != 0)
			ifr->ifr_reqcap |= IFCAP_VLAN_HWTAGGING;
		if ((ifr->ifr_reqcap & IFCAP_VLAN_HWTAGGING) == 0)
			ifr->ifr_reqcap &= ~IFCAP_VLAN_HWTSO;
		if ((ifr->ifr_reqcap & IFCAP_TSO4) != 0)
			ifr->ifr_reqcap |= IFCAP_TXCSUM;
		if ((ifr->ifr_reqcap & IFCAP_TXCSUM) == 0)
			ifr->ifr_reqcap &= ~IFCAP_TSO4;
		if ((ifr->ifr_reqcap & IFCAP_TSO6) != 0)
			ifr->ifr_reqcap |= IFCAP_TXCSUM_IPV6;
		if ((ifr->ifr_reqcap & IFCAP_TXCSUM_IPV6) == 0)
			ifr->ifr_reqcap &= ~IFCAP_TSO6;
		/*
		 * Now check that requested capabilities match
		 * what interface can actually do, and whether
		 * there is any change in the capenable.
		 */
		if (ifr->ifr_reqcap & ~ifp->if_capabilities)
			return (EINVAL);
		if (ifr->ifr_reqcap == ifp->if_capenable)
			return (0);
		ifr->ifr_curcap = ifp->if_capenable;
		/*
		 * See if driver accepts ifr_reqcap.  It may also
		 * adjust them.  Driver also fills in ifr_hwassist.
		 */
		error = if_ioctl(ifp, cmd, data, td);
		if (error != 0)
			break;
#ifdef DEVICE_POLLING
		if ((ifr->ifr_reqcap ^ ifr->ifr_curcap) & IFCAP_POLLING) {
			if (ifr->ifr_reqcap & IFCAP_POLLING)
				if_poll_register(ifp);
			else
				if_poll_deregister(ifp);
		}
#endif
		ifp->if_capenable = ifr->ifr_reqcap;
		ifp->if_hwassist = ifr->ifr_hwassist;
		getmicrotime(&ifp->if_lastchange);
		if (ifp->if_vlantrunk != NULL)
			(*vlan_trunk_cap_p)(ifp);
		break;
#ifdef MAC
	case SIOCSIFMAC:
		error = mac_ifnet_ioctl_set(td->td_ucred, ifr, ifp);
		break;
#endif

	case SIOCSIFNAME:
		error = priv_check(td, PRIV_NET_SETIFNAME);
		if (error)
			return (error);
		error = copyinstr(ifr->ifr_data, new_name, IFNAMSIZ, NULL);
		if (error != 0)
			return (error);
		if (new_name[0] == '\0')
			return (EINVAL);
		if (ifunit(new_name) != NULL)
			return (EEXIST);

		/*
		 * XXX: Locking.  Nothing else seems to lock if_flags,
		 * and there are numerous other races with the
		 * ifunit() checks not being atomic with namespace
		 * changes (renames, vmoves, if_attach, etc).
		 */
		ifp->if_flags |= IFF_RENAMING;
		
		/* Announce the departure of the interface. */
		rt_ifannouncemsg(ifp, IFAN_DEPARTURE);
		EVENTHANDLER_INVOKE(ifnet_departure_event, ifp);

		log(LOG_INFO, "%s: changing name to '%s'\n",
		    ifp->if_xname, new_name);

		IF_ADDR_WLOCK(ifp);
		strlcpy(ifp->if_xname, new_name, sizeof(ifp->if_xname));
		ifa = ifp->if_addr;
		sdl = (struct sockaddr_dl *)ifa->ifa_addr;
		namelen = strlen(new_name);
		onamelen = sdl->sdl_nlen;
		/*
		 * Move the address if needed.  This is safe because we
		 * allocate space for a name of length IFNAMSIZ when we
		 * create this in if_attach().
		 */
		if (namelen != onamelen) {
			bcopy(sdl->sdl_data + onamelen,
			    sdl->sdl_data + namelen, sdl->sdl_alen);
		}
		bcopy(new_name, sdl->sdl_data, namelen);
		sdl->sdl_nlen = namelen;
		sdl = (struct sockaddr_dl *)ifa->ifa_netmask;
		bzero(sdl->sdl_data, onamelen);
		while (namelen != 0)
			sdl->sdl_data[--namelen] = 0xff;
		IF_ADDR_WUNLOCK(ifp);

		EVENTHANDLER_INVOKE(ifnet_arrival_event, ifp);
		/* Announce the return of the interface. */
		rt_ifannouncemsg(ifp, IFAN_ARRIVAL);

		ifp->if_flags &= ~IFF_RENAMING;
		break;

#ifdef VIMAGE
	case SIOCSIFVNET:
		error = priv_check(td, PRIV_NET_SETIFVNET);
		if (error)
			return (error);
		error = if_vmove_loan(td, ifp, ifr->ifr_name, ifr->ifr_jid);
		break;
#endif

	case SIOCSIFMETRIC:
		error = priv_check(td, PRIV_NET_SETIFMETRIC);
		if (error)
			return (error);
		ifp->if_metric = ifr->ifr_metric;
		getmicrotime(&ifp->if_lastchange);
		break;

	case SIOCSIFPHYS:
		error = priv_check(td, PRIV_NET_SETIFPHYS);
		if (error)
			return (error);
		error = if_ioctl(ifp, cmd, data, td);
		if (error == 0)
			getmicrotime(&ifp->if_lastchange);
		break;

	case SIOCSIFMTU:
		error = priv_check(td, PRIV_NET_SETIFMTU);
		if (error)
			return (error);
		if (ifr->ifr_mtu < IF_MINMTU || ifr->ifr_mtu > IF_MAXMTU)
			return (EINVAL);
		if (ifr->ifr_mtu == ifp->if_mtu)
			return (0);
		error = if_ioctl(ifp, cmd, data, td);
		if (error == 0) {
			ifp->if_mtu = ifr->ifr_mtu;
			getmicrotime(&ifp->if_lastchange);
			rt_ifmsg(ifp);
#ifdef INET6
			nd6_setmtu(ifp);
#endif
			rt_updatemtu(ifp);
		}
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if (cmd == SIOCADDMULTI)
			error = priv_check(td, PRIV_NET_ADDMULTI);
		else
			error = priv_check(td, PRIV_NET_DELMULTI);
		if (error)
			return (error);

		/* Don't allow group membership on non-multicast interfaces. */
		if ((ifp->if_flags & IFF_MULTICAST) == 0)
			return (EOPNOTSUPP);

		/* Don't let users screw up protocols' entries. */
		if (ifr->ifr_addr.sa_family != AF_LINK)
			return (EINVAL);

		if (cmd == SIOCADDMULTI) {
			struct ifmultiaddr *ifma;

			/*
			 * Userland is only permitted to join groups once
			 * via the if_addmulti() KPI, because it cannot hold
			 * struct ifmultiaddr * between calls. It may also
			 * lose a race while we check if the membership
			 * already exists.
			 */
			IF_ADDR_RLOCK(ifp);
			ifma = if_findmulti(ifp, &ifr->ifr_addr);
			IF_ADDR_RUNLOCK(ifp);
			if (ifma != NULL)
				error = EADDRINUSE;
			else
				error = if_addmulti(ifp, &ifr->ifr_addr, &ifma);
		} else {
			error = if_delmulti(ifp, &ifr->ifr_addr);
		}
		if (error == 0)
			getmicrotime(&ifp->if_lastchange);
		break;

	case SIOCSIFPHYADDR:
	case SIOCDIFPHYADDR:
#ifdef INET6
	case SIOCSIFPHYADDR_IN6:
#endif
	case SIOCSIFGENERIC:
		error = priv_check(td, PRIV_NET_HWIOCTL);
		if (error)
			return (error);
		error = if_ioctl(ifp, cmd, data, td);
		if (error == 0)
			getmicrotime(&ifp->if_lastchange);
		break;

	case SIOCGIFSTATUS:
	case SIOCGIFPSRCADDR:
	case SIOCGIFPDSTADDR:
	case SIOCGIFGENERIC:
		error = if_ioctl(ifp, cmd, data, td);
		break;

	case SIOCSIFLLADDR:
		error = priv_check(td, PRIV_NET_SETLLADDR);
		if (error)
			return (error);
		error = if_setlladdr(ifp,
		    ifr->ifr_addr.sa_data, ifr->ifr_addr.sa_len);
		EVENTHANDLER_INVOKE(iflladdr_event, ifp);
		break;

	case SIOCAIFGROUP:
	{
		struct ifgroupreq *ifgr = (struct ifgroupreq *)ifr;

		error = priv_check(td, PRIV_NET_ADDIFGROUP);
		if (error)
			return (error);
		if ((error = if_addgroup(ifp, ifgr->ifgr_group)))
			return (error);
		break;
	}

	case SIOCGIFGROUP:
		if ((error = if_getgroup((struct ifgroupreq *)ifr, ifp)))
			return (error);
		break;

	case SIOCDIFGROUP:
	{
		struct ifgroupreq *ifgr = (struct ifgroupreq *)ifr;

		error = priv_check(td, PRIV_NET_DELIFGROUP);
		if (error)
			return (error);
		if ((error = if_delgroup(ifp, ifgr->ifgr_group)))
			return (error);
		break;
	}

	case SIOCSIFMEDIA:
		error = priv_check(td, PRIV_NET_HWIOCTL);
		if (error)
			return (error);
		/* FALLTHROUGH */
	case SIOCGIFMEDIA:
	case SIOCGIFXMEDIA:
		return (ifmedia_ioctl(ifp, ifr, cmd));

	default:
		error = ENOIOCTL;
		break;
	}
	return (error);
}

#ifdef COMPAT_FREEBSD32
struct ifconf32 {
	int32_t	ifc_len;
	union {
		uint32_t	ifcu_buf;
		uint32_t	ifcu_req;
	} ifc_ifcu;
};
#define	SIOCGIFCONF32	_IOWR('i', 36, struct ifconf32)
#endif

/*
 * Interface ioctls.
 */
int
ifioctl(struct socket *so, u_long cmd, caddr_t data, struct thread *td)
{
	struct ifnet *ifp;
	struct ifreq *ifr;
	int error;
	int oif_flags;

	CURVNET_SET(so->so_vnet);
	switch (cmd) {
	case SIOCGIFCONF:
		error = ifconf(cmd, data);
		CURVNET_RESTORE();
		return (error);

#ifdef COMPAT_FREEBSD32
	case SIOCGIFCONF32:
		{
			struct ifconf32 *ifc32;
			struct ifconf ifc;

			ifc32 = (struct ifconf32 *)data;
			ifc.ifc_len = ifc32->ifc_len;
			ifc.ifc_buf = PTRIN(ifc32->ifc_buf);

			error = ifconf(SIOCGIFCONF, (void *)&ifc);
			CURVNET_RESTORE();
			if (error == 0)
				ifc32->ifc_len = ifc.ifc_len;
			return (error);
		}
#endif
	}
	ifr = (struct ifreq *)data;

	switch (cmd) {
#ifdef VIMAGE
	case SIOCSIFRVNET:
		error = priv_check(td, PRIV_NET_SETIFVNET);
		if (error == 0)
			error = if_vmove_reclaim(td, ifr->ifr_name,
			    ifr->ifr_jid);
		CURVNET_RESTORE();
		return (error);
#endif
	case SIOCIFCREATE:
	case SIOCIFCREATE2:
		error = priv_check(td, PRIV_NET_IFCREATE);
		if (error == 0)
			error = if_clone_create(ifr->ifr_name,
			    sizeof(ifr->ifr_name),
			    cmd == SIOCIFCREATE2 ? ifr->ifr_data : NULL);
		CURVNET_RESTORE();
		return (error);
	case SIOCIFDESTROY:
		error = priv_check(td, PRIV_NET_IFDESTROY);
		if (error == 0)
			error = if_clone_destroy(ifr->ifr_name);
		CURVNET_RESTORE();
		return (error);

	case SIOCIFGCLONERS:
		error = if_clone_list((struct if_clonereq *)data);
		CURVNET_RESTORE();
		return (error);
	case SIOCGIFGMEMB:
		error = if_getgroupmembers((struct ifgroupreq *)data);
		CURVNET_RESTORE();
		return (error);
#if defined(INET) || defined(INET6)
	case SIOCSVH:
	case SIOCGVH:
		if (carp_ioctl_p == NULL)
			error = EPROTONOSUPPORT;
		else
			error = (*carp_ioctl_p)(ifr, cmd, td);
		CURVNET_RESTORE();
		return (error);
#endif
	}

	ifp = ifunit_ref(ifr->ifr_name);
	if (ifp == NULL) {
		CURVNET_RESTORE();
		return (ENXIO);
	}

	error = if_drvioctl(ifp, cmd, data, td);
	if (error != ENOIOCTL) {
		if_rele(ifp);
		CURVNET_RESTORE();
		return (error);
	}

	oif_flags = ifp->if_flags;
	if (so->so_proto == NULL) {
		if_rele(ifp);
		CURVNET_RESTORE();
		return (EOPNOTSUPP);
	}

	/*
	 * Pass the request on to the socket control method, and if the
	 * latter returns EOPNOTSUPP, directly to the interface.
	 */
	error = ((*so->so_proto->pr_usrreqs->pru_control)(so, cmd, data,
	    ifp, td));
	if (error == EOPNOTSUPP)
		error = if_ioctl(ifp, cmd, data, td);

	if ((oif_flags ^ ifp->if_flags) & IFF_UP) {
#ifdef INET6
		if (ifp->if_flags & IFF_UP)
			in6_if_up(ifp);
#endif
	}
	if_rele(ifp);
	CURVNET_RESTORE();
	return (error);
}

/*
 * The code common to handling reference counted flags,
 * e.g., in ifpromisc() and if_allmulti().
 * The "pflag" argument can specify a permanent mode flag to check,
 * such as IFF_PPROMISC for promiscuous mode; should be 0 if none.
 *
 * Only to be used on stack-owned flags, not driver-owned flags.
 */
static int
if_setflag(struct ifnet *ifp, int flag, int pflag, int *refcount, int onswitch)
{
	struct ifreq ifr;
	int error;
	int oldflags, oldcount;

	if (onswitch)
		KASSERT(*refcount >= 0,
		    ("%s: increment negative refcount %d for flag %d",
		    __func__, *refcount, flag));
	else
		KASSERT(*refcount > 0,
		    ("%s: decrement non-positive refcount %d for flag %d",
		    __func__, *refcount, flag));

	/* In case this mode is permanent, just touch refcount */
	if (ifp->if_flags & pflag) {
		*refcount += onswitch ? 1 : -1;
		return (0);
	}

	/* Save ifnet parameters for if_ioctl() may fail */
	oldcount = *refcount;
	oldflags = ifp->if_flags;
	
	/*
	 * See if we aren't the only and touching refcount is enough.
	 * Actually toggle interface flag if we are the first or last.
	 */
	if (onswitch) {
		if ((*refcount)++)
			return (0);
		ifp->if_flags |= flag;
	} else {
		if (--(*refcount))
			return (0);
		ifp->if_flags &= ~flag;
	}

	/* Call down the driver since we've changed interface flags */
	ifr.ifr_flags = ifp->if_flags & 0xffff;
	ifr.ifr_flagshigh = ifp->if_flags >> 16;
	error = if_ioctl(ifp, SIOCSIFFLAGS, (caddr_t)&ifr, curthread);
	if (error)
		goto recover;
	/* Notify userland that interface flags have changed */
	rt_ifmsg(ifp);
	return (0);

recover:
	/* Recover after driver error */
	*refcount = oldcount;
	ifp->if_flags = oldflags;
	return (error);
}

/*
 * Set/clear promiscuous mode on interface ifp based on the truth value
 * of pswitch.  The calls are reference counted so that only the first
 * "on" request actually has an effect, as does the final "off" request.
 * Results are undefined if the "off" and "on" requests are not matched.
 */
int
ifpromisc(struct ifnet *ifp, int pswitch)
{
	int error;
	int oldflags = ifp->if_flags;

	error = if_setflag(ifp, IFF_PROMISC, IFF_PPROMISC,
			   &ifp->if_pcount, pswitch);
	/* If promiscuous mode status has changed, log a message */
	if (error == 0 && ((ifp->if_flags ^ oldflags) & IFF_PROMISC))
		log(LOG_INFO, "%s: promiscuous mode %s\n",
		    ifp->if_xname,
		    (ifp->if_flags & IFF_PROMISC) ? "enabled" : "disabled");
	return (error);
}

/*
 * Return interface configuration
 * of system.  List may be used
 * in later ioctl's (above) to get
 * other information.
 */
/*ARGSUSED*/
static int
ifconf(u_long cmd, caddr_t data)
{
	struct ifconf *ifc = (struct ifconf *)data;
	struct ifnet *ifp;
	struct ifaddr *ifa;
	struct ifreq ifr;
	struct sbuf *sb;
	int error, full = 0, valid_len, max_len;

	/* Limit initial buffer size to MAXPHYS to avoid DoS from userspace. */
	max_len = MAXPHYS - 1;

	/* Prevent hostile input from being able to crash the system */
	if (ifc->ifc_len <= 0)
		return (EINVAL);

again:
	if (ifc->ifc_len <= max_len) {
		max_len = ifc->ifc_len;
		full = 1;
	}
	sb = sbuf_new(NULL, NULL, max_len + 1, SBUF_FIXEDLEN);
	max_len = 0;
	valid_len = 0;

	IFNET_RLOCK();
	TAILQ_FOREACH(ifp, &V_ifnet, if_link) {
		int addrs;

		/*
		 * Zero the ifr_name buffer to make sure we don't
		 * disclose the contents of the stack.
		 */
		memset(ifr.ifr_name, 0, sizeof(ifr.ifr_name));

		if (strlcpy(ifr.ifr_name, ifp->if_xname, sizeof(ifr.ifr_name))
		    >= sizeof(ifr.ifr_name)) {
			sbuf_delete(sb);
			IFNET_RUNLOCK();
			return (ENAMETOOLONG);
		}

		addrs = 0;
		IF_ADDR_RLOCK(ifp);
		TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
			struct sockaddr *sa = ifa->ifa_addr;

			if (prison_if(curthread->td_ucred, sa) != 0)
				continue;
			addrs++;
			if (sa->sa_len <= sizeof(*sa)) {
				ifr.ifr_addr = *sa;
				sbuf_bcat(sb, &ifr, sizeof(ifr));
				max_len += sizeof(ifr);
			} else {
				sbuf_bcat(sb, &ifr,
				    offsetof(struct ifreq, ifr_addr));
				max_len += offsetof(struct ifreq, ifr_addr);
				sbuf_bcat(sb, sa, sa->sa_len);
				max_len += sa->sa_len;
			}

			if (sbuf_error(sb) == 0)
				valid_len = sbuf_len(sb);
		}
		IF_ADDR_RUNLOCK(ifp);
		if (addrs == 0) {
			bzero((caddr_t)&ifr.ifr_addr, sizeof(ifr.ifr_addr));
			sbuf_bcat(sb, &ifr, sizeof(ifr));
			max_len += sizeof(ifr);

			if (sbuf_error(sb) == 0)
				valid_len = sbuf_len(sb);
		}
	}
	IFNET_RUNLOCK();

	/*
	 * If we didn't allocate enough space (uncommon), try again.  If
	 * we have already allocated as much space as we are allowed,
	 * return what we've got.
	 */
	if (valid_len != max_len && !full) {
		sbuf_delete(sb);
		goto again;
	}

	ifc->ifc_len = valid_len;
	sbuf_finish(sb);
	error = copyout(sbuf_data(sb), ifc->ifc_req, ifc->ifc_len);
	sbuf_delete(sb);
	return (error);
}

/*
 * Just like ifpromisc(), but for all-multicast-reception mode.
 */
int
if_allmulti(struct ifnet *ifp, int onswitch)
{

	return (if_setflag(ifp, IFF_ALLMULTI, 0, &ifp->if_amcount, onswitch));
}

struct ifmultiaddr *
if_findmulti(struct ifnet *ifp, struct sockaddr *sa)
{
	struct ifmultiaddr *ifma;

	IF_ADDR_LOCK_ASSERT(ifp);

	TAILQ_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
		if (sa->sa_family == AF_LINK) {
			if (sa_dl_equal(ifma->ifma_addr, sa))
				break;
		} else {
			if (sa_equal(ifma->ifma_addr, sa))
				break;
		}
	}

	return ifma;
}

/*
 * Allocate a new ifmultiaddr and initialize based on passed arguments.  We
 * make copies of passed sockaddrs.  The ifmultiaddr will not be added to
 * the ifnet multicast address list here, so the caller must do that and
 * other setup work (such as notifying the device driver).  The reference
 * count is initialized to 1.
 */
static struct ifmultiaddr *
if_allocmulti(struct ifnet *ifp, struct sockaddr *sa, struct sockaddr *llsa,
    int mflags)
{
	struct ifmultiaddr *ifma;
	struct sockaddr *dupsa;

	ifma = malloc(sizeof *ifma, M_IFMADDR, mflags |
	    M_ZERO);
	if (ifma == NULL)
		return (NULL);

	dupsa = malloc(sa->sa_len, M_IFMADDR, mflags);
	if (dupsa == NULL) {
		free(ifma, M_IFMADDR);
		return (NULL);
	}
	bcopy(sa, dupsa, sa->sa_len);
	ifma->ifma_addr = dupsa;

	ifma->ifma_ifp = ifp;
	ifma->ifma_refcount = 1;
	ifma->ifma_protospec = NULL;

	if (llsa == NULL) {
		ifma->ifma_lladdr = NULL;
		return (ifma);
	}

	dupsa = malloc(llsa->sa_len, M_IFMADDR, mflags);
	if (dupsa == NULL) {
		free(ifma->ifma_addr, M_IFMADDR);
		free(ifma, M_IFMADDR);
		return (NULL);
	}
	bcopy(llsa, dupsa, llsa->sa_len);
	ifma->ifma_lladdr = dupsa;

	return (ifma);
}

/*
 * if_freemulti: free ifmultiaddr structure and possibly attached related
 * addresses.  The caller is responsible for implementing reference
 * counting, notifying the driver, handling routing messages, and releasing
 * any dependent link layer state.
 */
static void
if_freemulti(struct ifmultiaddr *ifma)
{

	KASSERT(ifma->ifma_refcount == 0, ("if_freemulti: refcount %d",
	    ifma->ifma_refcount));

	if (ifma->ifma_lladdr != NULL)
		free(ifma->ifma_lladdr, M_IFMADDR);
	free(ifma->ifma_addr, M_IFMADDR);
	free(ifma, M_IFMADDR);
}

/*
 * Register an additional multicast address with a network interface.
 *
 * - If the address is already present, bump the reference count on the
 *   address and return.
 * - If the address is not link-layer, look up a link layer address.
 * - Allocate address structures for one or both addresses, and attach to the
 *   multicast address list on the interface.  If automatically adding a link
 *   layer address, the protocol address will own a reference to the link
 *   layer address, to be freed when it is freed.
 * - Notify the network device driver of an addition to the multicast address
 *   list.
 *
 * 'sa' points to caller-owned memory with the desired multicast address.
 *
 * 'retifma' will be used to return a pointer to the resulting multicast
 * address reference, if desired.
 */
int
if_addmulti(struct ifnet *ifp, struct sockaddr *sa,
    struct ifmultiaddr **retifma)
{
	struct ifmultiaddr *ifma, *ll_ifma;
	struct sockaddr *llsa;
	struct sockaddr_dl sdl;
	int error;

	/*
	 * If the address is already present, return a new reference to it;
	 * otherwise, allocate storage and set up a new address.
	 */
	IF_ADDR_WLOCK(ifp);
	ifma = if_findmulti(ifp, sa);
	if (ifma != NULL) {
		ifma->ifma_refcount++;
		if (retifma != NULL)
			*retifma = ifma;
		IF_ADDR_WUNLOCK(ifp);
		return (0);
	}

	/*
	 * The address isn't already present; resolve the protocol address
	 * into a link layer address, and then look that up, bump its
	 * refcount or allocate an ifma for that also.
	 * Most link layer resolving functions returns address data which
	 * fits inside default sockaddr_dl structure. However callback
	 * can allocate another sockaddr structure, in that case we need to
	 * free it later.
	 */
	sdl.sdl_len = sizeof(sdl);
	llsa = (struct sockaddr *)&sdl;
	error = if_resolvemulti(ifp, &llsa, sa);
	if (error == EOPNOTSUPP)
		llsa = NULL;
	else if (error)
		goto unlock_out;

	/*
	 * Allocate the new address.  Don't hook it up yet, as we may also
	 * need to allocate a link layer multicast address.
	 */
	ifma = if_allocmulti(ifp, sa, llsa, M_NOWAIT);
	if (ifma == NULL) {
		error = ENOMEM;
		goto free_llsa_out;
	}

	/*
	 * If a link layer address is found, we'll need to see if it's
	 * already present in the address list, or allocate is as well.
	 * When this block finishes, the link layer address will be on the
	 * list.
	 */
	if (llsa != NULL) {
		ll_ifma = if_findmulti(ifp, llsa);
		if (ll_ifma == NULL) {
			ll_ifma = if_allocmulti(ifp, llsa, NULL, M_NOWAIT);
			if (ll_ifma == NULL) {
				--ifma->ifma_refcount;
				if_freemulti(ifma);
				error = ENOMEM;
				goto free_llsa_out;
			}
			TAILQ_INSERT_HEAD(&ifp->if_multiaddrs, ll_ifma,
			    ifma_link);
		} else
			ll_ifma->ifma_refcount++;
		ifma->ifma_llifma = ll_ifma;
	}

	/*
	 * We now have a new multicast address, ifma, and possibly a new or
	 * referenced link layer address.  Add the primary address to the
	 * ifnet address list.
	 */
	TAILQ_INSERT_HEAD(&ifp->if_multiaddrs, ifma, ifma_link);

	if (retifma != NULL)
		*retifma = ifma;

	/*
	 * Must generate the message while holding the lock so that 'ifma'
	 * pointer is still valid.
	 */
	rt_newmaddrmsg(RTM_NEWMADDR, ifma);
	IF_ADDR_WUNLOCK(ifp);

	/*
	 * We are certain we have added something, so call down to the
	 * interface to let them know about it.
	 */
	if_ioctl(ifp, SIOCADDMULTI, 0, curthread);

	if ((llsa != NULL) && (llsa != (struct sockaddr *)&sdl))
		link_free_sdl(llsa);

	return (0);

free_llsa_out:
	if ((llsa != NULL) && (llsa != (struct sockaddr *)&sdl))
		link_free_sdl(llsa);

unlock_out:
	IF_ADDR_WUNLOCK(ifp);
	return (error);
}

/*
 * Delete a multicast group membership by network-layer group address.
 *
 * Returns ENOENT if the entry could not be found. If ifp no longer
 * exists, results are undefined. This entry point should only be used
 * from subsystems which do appropriate locking to hold ifp for the
 * duration of the call.
 * Network-layer protocol domains must use if_delmulti_ifma().
 */
int
if_delmulti(struct ifnet *ifp, struct sockaddr *sa)
{
	struct ifmultiaddr *ifma;
	int lastref;
#ifdef INVARIANTS
	struct ifnet *oifp;

	IFNET_RLOCK_NOSLEEP();
	TAILQ_FOREACH(oifp, &V_ifnet, if_link)
		if (ifp == oifp)
			break;
	if (ifp != oifp)
		ifp = NULL;
	IFNET_RUNLOCK_NOSLEEP();

	KASSERT(ifp != NULL, ("%s: ifnet went away", __func__));
#endif
	if (ifp == NULL)
		return (ENOENT);

	IF_ADDR_WLOCK(ifp);
	lastref = 0;
	ifma = if_findmulti(ifp, sa);
	if (ifma != NULL)
		lastref = if_delmulti_locked(ifp, ifma, 0);
	IF_ADDR_WUNLOCK(ifp);

	if (ifma == NULL)
		return (ENOENT);

	if (lastref)
		if_ioctl(ifp, SIOCDELMULTI, 0, curthread);

	return (0);
}

/*
 * Delete all multicast group membership for an interface.
 * Should be used to quickly flush all multicast filters.
 */
void
if_delallmulti(struct ifnet *ifp)
{
	struct ifmultiaddr *ifma;
	struct ifmultiaddr *next;

	IF_ADDR_WLOCK(ifp);
	TAILQ_FOREACH_SAFE(ifma, &ifp->if_multiaddrs, ifma_link, next)
		if_delmulti_locked(ifp, ifma, 0);
	IF_ADDR_WUNLOCK(ifp);
}

/*
 * Delete a multicast group membership by group membership pointer.
 * Network-layer protocol domains must use this routine.
 *
 * It is safe to call this routine if the ifp disappeared.
 */
void
if_delmulti_ifma(struct ifmultiaddr *ifma)
{
	struct ifnet *ifp;
	int lastref;

	ifp = ifma->ifma_ifp;
#ifdef DIAGNOSTIC
	if (ifp == NULL) {
		printf("%s: ifma_ifp seems to be detached\n", __func__);
	} else {
		struct ifnet *oifp;

		IFNET_RLOCK_NOSLEEP();
		TAILQ_FOREACH(oifp, &V_ifnet, if_link)
			if (ifp == oifp)
				break;
		if (ifp != oifp) {
			printf("%s: ifnet %p disappeared\n", __func__, ifp);
			ifp = NULL;
		}
		IFNET_RUNLOCK_NOSLEEP();
	}
#endif
	/*
	 * If and only if the ifnet instance exists: Acquire the address lock.
	 */
	if (ifp != NULL)
		IF_ADDR_WLOCK(ifp);

	lastref = if_delmulti_locked(ifp, ifma, 0);

	if (ifp != NULL) {
		/*
		 * If and only if the ifnet instance exists:
		 *  Release the address lock.
		 *  If the group was left: update the hardware hash filter.
		 */
		IF_ADDR_WUNLOCK(ifp);
		if (lastref)
			if_ioctl(ifp, SIOCDELMULTI, 0, curthread);
	}
}

/*
 * Perform deletion of network-layer and/or link-layer multicast address.
 *
 * Return 0 if the reference count was decremented.
 * Return 1 if the final reference was released, indicating that the
 * hardware hash filter should be reprogrammed.
 */
static int
if_delmulti_locked(struct ifnet *ifp, struct ifmultiaddr *ifma, int detaching)
{
	struct ifmultiaddr *ll_ifma;

	if (ifp != NULL && ifma->ifma_ifp != NULL) {
		KASSERT(ifma->ifma_ifp == ifp,
		    ("%s: inconsistent ifp %p", __func__, ifp));
		IF_ADDR_WLOCK_ASSERT(ifp);
	}

	ifp = ifma->ifma_ifp;

	/*
	 * If the ifnet is detaching, null out references to ifnet,
	 * so that upper protocol layers will notice, and not attempt
	 * to obtain locks for an ifnet which no longer exists. The
	 * routing socket announcement must happen before the ifnet
	 * instance is detached from the system.
	 */
	if (detaching) {
#ifdef DIAGNOSTIC
		printf("%s: detaching ifnet instance %p\n", __func__, ifp);
#endif
		/*
		 * ifp may already be nulled out if we are being reentered
		 * to delete the ll_ifma.
		 */
		if (ifp != NULL) {
			rt_newmaddrmsg(RTM_DELMADDR, ifma);
			ifma->ifma_ifp = NULL;
		}
	}

	if (--ifma->ifma_refcount > 0)
		return 0;

	/*
	 * If this ifma is a network-layer ifma, a link-layer ifma may
	 * have been associated with it. Release it first if so.
	 */
	ll_ifma = ifma->ifma_llifma;
	if (ll_ifma != NULL) {
		KASSERT(ifma->ifma_lladdr != NULL,
		    ("%s: llifma w/o lladdr", __func__));
		if (detaching)
			ll_ifma->ifma_ifp = NULL;	/* XXX */
		if (--ll_ifma->ifma_refcount == 0) {
			if (ifp != NULL) {
				TAILQ_REMOVE(&ifp->if_multiaddrs, ll_ifma,
				    ifma_link);
			}
			if_freemulti(ll_ifma);
		}
	}

	if (ifp != NULL)
		TAILQ_REMOVE(&ifp->if_multiaddrs, ifma, ifma_link);

	if_freemulti(ifma);

	/*
	 * The last reference to this instance of struct ifmultiaddr
	 * was released; the hardware should be notified of this change.
	 */
	return 1;
}

/*
 * Set the link layer address on an interface.
 *
 * At this time we only support certain types of interfaces,
 * and we don't allow the length of the address to change.
 */
int
if_setlladdr(struct ifnet *ifp, const u_char *lladdr, int len)
{
	struct sockaddr_dl *sdl;
	struct ifaddr *ifa;
	struct ifreq ifr;

	IF_ADDR_RLOCK(ifp);
	ifa = ifp->if_addr;
	if (ifa == NULL) {
		IF_ADDR_RUNLOCK(ifp);
		return (EINVAL);
	}
	ifa_ref(ifa);
	IF_ADDR_RUNLOCK(ifp);
	sdl = (struct sockaddr_dl *)ifa->ifa_addr;
	if (sdl == NULL) {
		ifa_free(ifa);
		return (EINVAL);
	}
	if (len != sdl->sdl_alen) {	/* don't allow length to change */
		ifa_free(ifa);
		return (EINVAL);
	}
	switch (if_type(ifp)) {
	case IFT_ETHER:
	case IFT_FDDI:
	case IFT_XETHER:
	case IFT_ISO88025:
	case IFT_L2VLAN:
	case IFT_BRIDGE:
	case IFT_ARCNET:
	case IFT_IEEE8023ADLAG:
	case IFT_IEEE80211:
		bcopy(lladdr, LLADDR(sdl), len);
		ifa_free(ifa);
		break;
	default:
		ifa_free(ifa);
		return (ENODEV);
	}

	/*
	 * If the interface is already up, we need
	 * to re-init it in order to reprogram its
	 * address filter.
	 */
	if ((ifp->if_flags & IFF_UP) != 0) {
		ifp->if_flags &= ~IFF_UP;
		ifr.ifr_flags = ifp->if_flags & 0xffff;
		ifr.ifr_flagshigh = ifp->if_flags >> 16;
		if_ioctl(ifp, SIOCSIFFLAGS, &ifr, curthread);
		ifp->if_flags |= IFF_UP;
		ifr.ifr_flags = ifp->if_flags & 0xffff;
		ifr.ifr_flagshigh = ifp->if_flags >> 16;
		if_ioctl(ifp, SIOCSIFFLAGS, &ifr, curthread);
#ifdef INET
		/*
		 * Also send gratuitous ARPs to notify other nodes about
		 * the address change.
		 */
		TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
			if (ifa->ifa_addr->sa_family == AF_INET)
				arp_ifinit(ifp, ifa);
		}
#endif
	}
	return (0);
}

/*
 * Return address length of the interface.
 *
 * For vlan(4) the address length of different instances can be different.
 * For usual interfaces sdl->sdl_alen == ifdrv_addrlen.
 */
uint8_t
if_addrlen(const if_t ifp)
{
	struct sockaddr_dl *sdl;

	sdl = (struct sockaddr_dl *)ifp->if_addr->ifa_addr;
	return (sdl->sdl_alen);
}

int
if_printf(struct ifnet *ifp, const char * fmt, ...)
{
	va_list ap;
	int retval;

	retval = printf("%s: ", ifp->if_xname);
	va_start(ap, fmt);
	retval += vprintf(fmt, ap);
	va_end(ap);
	return (retval);
}

int
if_getmtu_family(if_t ifp, int family)
{
	struct domain *dp;

	for (dp = domains; dp; dp = dp->dom_next)
		if (dp->dom_family == family && dp->dom_ifmtu != NULL)
			return (dp->dom_ifmtu(ifp));

	return (ifp->if_mtu);
}

/*
 * Methods for drivers to access interface unicast and multicast
 * addresses.  Driver do not know 'struct ifaddr' neither 'struct ifmultiaddr'.
 */
void
if_foreach_addr(if_t ifp, ifaddr_cb_t cb, void *cb_arg)
{
	struct ifaddr *ifa;

	IF_ADDR_RLOCK(ifp);
	TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link)
		(*cb)(cb_arg, ifa->ifa_addr, ifa->ifa_dstaddr,
		    ifa->ifa_netmask);
	IF_ADDR_RUNLOCK(ifp);
}

void
if_foreach_maddr(if_t ifp, ifmaddr_cb_t cb, void *cb_arg)
{
	struct ifmultiaddr *ifma;

	IF_ADDR_RLOCK(ifp);
	TAILQ_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link)
		(*cb)(cb_arg, ifma->ifma_addr);
	IF_ADDR_RUNLOCK(ifp);
}

/*
 * Generic software queue, that many non-high-end drivers use.  For now
 * it is minimalistic version of classic BSD ifqueue, but we can swap it
 * to any other implementation later.
 */
struct ifqueue {
	struct mbufq	ifq_mbq;
	struct mtx	ifq_mtx;
};

static struct ifqueue *
if_snd_alloc(int maxlen)
{
	struct ifqueue *ifq;

	ifq = malloc(sizeof(struct ifqueue), M_IFNET, M_WAITOK);
	mbufq_init(&ifq->ifq_mbq, maxlen);
	mtx_init(&ifq->ifq_mtx, "ifqueue", NULL, MTX_DEF | MTX_NEW);

	return (ifq);
}

static void
if_snd_free(struct ifqueue *ifq)
{

	mtx_destroy(&ifq->ifq_mtx);
	free(ifq, M_IFNET);
}

/*
 * Flush software interface queue.
 */
static void
if_snd_qflush(if_t ifp)
{
	struct ifqueue *ifq = ifp->if_snd;

	mtx_lock(&ifq->ifq_mtx);
	mbufq_drain(&ifq->ifq_mbq);
	mtx_unlock(&ifq->ifq_mtx);
}

int
if_snd_len(if_t ifp)
{
	struct ifqueue *ifq = ifp->if_snd;

	return (mbufq_len(&ifq->ifq_mbq));
}

int
if_snd_enqueue(struct ifnet *ifp, struct mbuf *m)
{
	struct ifqueue *ifq = ifp->if_snd;
	int error;

	mtx_lock(&ifq->ifq_mtx);
	error = mbufq_enqueue(&ifq->ifq_mbq, m);
	mtx_unlock(&ifq->ifq_mtx);
	if (error)
		if_inc_counter(ifp, IFCOUNTER_OQDROPS, 1);
	return (error);
}

struct mbuf *
if_snd_dequeue(if_t ifp)
{
	struct ifqueue *ifq = ifp->if_snd;
	struct mbuf *m;

	mtx_lock(&ifq->ifq_mtx);
	m = mbufq_dequeue(&ifq->ifq_mbq);
	mtx_unlock(&ifq->ifq_mtx);
	return (m);
}

void
if_snd_prepend(if_t ifp, struct mbuf *m)
{
	struct ifqueue *ifq = ifp->if_snd;

	mtx_lock(&ifq->ifq_mtx);
	mbufq_prepend(&ifq->ifq_mbq, m);
	mtx_unlock(&ifq->ifq_mtx);
}

int
if_vlanid(if_t vifp, uint16_t *vid)
{

	if (if_type(vifp) != IFT_L2VLAN)
		return (EINVAL);
	*vid = (*vlan_vid_p)(vifp);
	return (0);
}

if_t
if_vlandev(if_t parent, uint16_t vid)
{

	if (parent->if_vlantrunk == NULL)
                return (NULL);
	return ((*vlan_dev_p)(parent, vid));
}

if_t
if_vlantrunk(if_t vifp)
{

	if (if_type(vifp) != IFT_L2VLAN)
		return (NULL);
	return ((*vlan_trunkdev_p)(vifp));
}

/*
 * Implementation of if ops, that can be called from drivers.
 */
void
if_input_noinline(if_t ifp, struct mbuf *m)
{

	return (if_input(ifp, m));
}

int
if_transmit_noinline(if_t ifp, struct mbuf *m)
{

	return (if_transmit(ifp, m));
}
