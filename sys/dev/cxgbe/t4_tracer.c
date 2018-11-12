/*-
 * Copyright (c) 2013 Chelsio Communications, Inc.
 * All rights reserved.
 * Written by: Navdeep Parhar <np@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_inet.h"
#include "opt_inet6.h"

#include <sys/param.h>
#include <sys/eventhandler.h>
#include <sys/lock.h>
#include <sys/types.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sx.h>
#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_clone.h>
#include <net/if_types.h>

#include "common/common.h"
#include "common/t4_msg.h"
#include "common/t4_regs.h"
#include "t4_ioctl.h"

/*
 * Locking notes
 * =============
 *
 * An interface cloner is registered during mod_load and it can be used to
 * create or destroy the tracing ifnet for an adapter at any time.  It is
 * possible for the cloned interface to outlive the adapter (adapter disappears
 * in t4_detach but the tracing ifnet may live till mod_unload when removal of
 * the cloner finally destroys any remaining cloned interfaces).  When tracing
 * filters are active, this ifnet is also receiving data.  There are potential
 * bad races between ifnet create, ifnet destroy, ifnet rx, ifnet ioctl,
 * cxgbe_detach/t4_detach, mod_unload.
 *
 * a) The driver selects an iq for tracing (sc->traceq) inside a synch op.  The
 *    iq is destroyed inside a synch op too (and sc->traceq updated).
 * b) The cloner looks for an adapter that matches the name of the ifnet it's
 *    been asked to create, starts a synch op on that adapter, and proceeds only
 *    if the adapter has a tracing iq.
 * c) The cloned ifnet and the adapter are coupled to each other via
 *    ifp->if_softc and sc->ifp.  These can be modified only with the global
 *    t4_trace_lock sx as well as the sc->ifp_lock mutex held.  Holding either
 *    of these will prevent any change.
 *
 * The order in which all the locks involved should be acquired are:
 * t4_list_lock
 * adapter lock
 * (begin synch op and let go of the above two)
 * t4_trace_lock
 * sc->ifp_lock
 */

static struct sx t4_trace_lock;

/* tracer interface ops.  mostly no-ops. */
static int tracer_ioctl(if_t, unsigned long, void *, struct thread *);
static int tracer_media_change(if_t, if_media_t);
static void tracer_media_status(if_t, struct ifmediareq *);

static if_media_t tracer_mediae[] = { IFM_ETHER | IFM_FDX | IFM_NONE, 0 };

static struct ifdriver t4_tracer_ifdrv = {
	.ifdrv_ops = {
		.ifop_ioctl = tracer_ioctl,
		.ifop_media_change = tracer_media_change,
		.ifop_media_status = tracer_media_status,
	},
	.ifdrv_name = "tXnex",
	.ifdrv_type = IFT_ETHER,
};

/* match name (request/response) */
struct match_rr {
	const char *name;
	int lock;	/* set to 1 to returned sc locked. */
	struct adapter *sc;
	int rc;
};

static void
match_name(struct adapter *sc, void *arg)
{
	struct match_rr *mrr = arg;

	if (strcmp(device_get_nameunit(sc->dev), mrr->name) != 0)
		return;

	KASSERT(mrr->sc == NULL, ("%s: multiple matches (%p, %p) for %s",
	    __func__, mrr->sc, sc, mrr->name));

	mrr->sc = sc;
	if (mrr->lock)
		mrr->rc = begin_synchronized_op(mrr->sc, NULL, 0, "t4clon");
	else
		mrr->rc = 0;
}

static int
t4_cloner_match(struct if_clone *ifc, const char *name)
{

	if (strncmp(name, "t4nex", 5) != 0 &&
	    strncmp(name, "t5nex", 5) != 0)
		return (0);
	if (name[5] < '0' || name[5] > '9')
		return (0);
	return (1);
}

static int
t4_cloner_create(struct if_clone *ifc, char *name, size_t len, caddr_t params)
{
	const uint8_t lla[ETHER_ADDR_LEN] = {0, 0, 0, 0, 0, 0};
	struct if_attach_args ifat = {
		.ifat_drv = &t4_tracer_ifdrv,
		.ifat_name = name,
		.ifat_dunit = -1,
		.ifat_flags = IFF_SIMPLEX,
		.ifat_capabilities = IFCAP_JUMBO_MTU | IFCAP_VLAN_MTU,
		.ifat_lla = lla,
		.ifat_mediae = tracer_mediae,
		.ifat_media = tracer_mediae[0],
	};
	struct match_rr mrr;
	struct adapter *sc;
	if_t ifp;
	int rc = 0;

	mrr.name = name;
	mrr.lock = 1;
	mrr.sc = NULL;
	mrr.rc = ENOENT;
	t4_iterate(match_name, &mrr);

	if (mrr.rc != 0)
		return (mrr.rc);
	sc = mrr.sc;

	KASSERT(sc != NULL, ("%s: name (%s) matched but softc is NULL",
	    __func__, name));
	ASSERT_SYNCHRONIZED_OP(sc);

	sx_xlock(&t4_trace_lock);

	if (sc->ifp != NULL) {
		rc = EEXIST;
		goto done;
	}
	if (sc->traceq < 0) {
		rc = EAGAIN;
		goto done;
	}

	ifat.ifat_softc = sc;
	ifp = if_attach(&ifat);

	mtx_lock(&sc->ifp_lock);
	sc->ifp = ifp;
	mtx_unlock(&sc->ifp_lock);
done:
	sx_xunlock(&t4_trace_lock);
	end_synchronized_op(sc, 0);
	return (rc);
}

static int
t4_cloner_destroy(struct if_clone *ifc, if_t ifp)
{
	struct adapter *sc;

	sx_xlock(&t4_trace_lock);
	sc = if_getsoftc(ifp, IF_DRIVER_SOFTC);
	if (sc != NULL) {
		mtx_lock(&sc->ifp_lock);
		sc->ifp = NULL;
		mtx_unlock(&sc->ifp_lock);
	}
	if_detach(ifp);
	sx_xunlock(&t4_trace_lock);

	return (0);
}

void
t4_tracer_modload()
{
	struct ifdriver *drv = &t4_tracer_ifdrv;

	sx_init(&t4_trace_lock, "T4/T5 tracer lock");
	drv->ifdrv_clone = if_clone_advanced(drv->ifdrv_name, 0,
	    t4_cloner_match, t4_cloner_create, t4_cloner_destroy);
}

void
t4_tracer_modunload()
{

	if (t4_tracer_ifdrv.ifdrv_clone != NULL) {
		/*
		 * The module is being unloaded so the nexus drivers have
		 * detached.  The tracing interfaces can not outlive the nexus
		 * (ifp->if_softc is the nexus) and must have been destroyed
		 * already.  XXX: but if_clone is opaque to us and we can't
		 * assert LIST_EMPTY(&t4_cloner->ifc_iflist) at this time.
		 */
		if_clone_detach(t4_tracer_ifdrv.ifdrv_clone);
	}
	sx_destroy(&t4_trace_lock);
}

void
t4_tracer_port_detach(struct adapter *sc)
{

	sx_xlock(&t4_trace_lock);
	if (sc->ifp != NULL) {
		mtx_lock(&sc->ifp_lock);
		sc->ifp = NULL;
		mtx_unlock(&sc->ifp_lock);
	}
	sx_xunlock(&t4_trace_lock);
}

int
t4_get_tracer(struct adapter *sc, struct t4_tracer *t)
{
	int rc, i, enabled;
	struct trace_params tp;

	if (t->idx >= NTRACE) {
		t->idx = 0xff;
		t->enabled = 0;
		t->valid = 0;
		return (0);
	}

	rc = begin_synchronized_op(sc, NULL, HOLD_LOCK | SLEEP_OK | INTR_OK,
	    "t4gett");
	if (rc)
		return (rc);

	for (i = t->idx; i < NTRACE; i++) {
		if (isset(&sc->tracer_valid, t->idx)) {
			t4_get_trace_filter(sc, &tp, i, &enabled);
			t->idx = i;
			t->enabled = enabled;
			t->valid = 1;
			memcpy(&t->tp.data[0], &tp.data[0], sizeof(t->tp.data));
			memcpy(&t->tp.mask[0], &tp.mask[0], sizeof(t->tp.mask));
			t->tp.snap_len = tp.snap_len;
			t->tp.min_len = tp.min_len;
			t->tp.skip_ofst = tp.skip_ofst;
			t->tp.skip_len = tp.skip_len;
			t->tp.invert = tp.invert;

			/* convert channel to port iff 0 <= port < 8. */
			if (tp.port < 4)
				t->tp.port = sc->chan_map[tp.port];
			else if (tp.port < 8)
				t->tp.port = sc->chan_map[tp.port - 4] + 4;
			else
				t->tp.port = tp.port;

			goto done;
		}
	}

	t->idx = 0xff;
	t->enabled = 0;
	t->valid = 0;
done:
	end_synchronized_op(sc, LOCK_HELD);

	return (rc);
}

int
t4_set_tracer(struct adapter *sc, struct t4_tracer *t)
{
	int rc;
	struct trace_params tp, *tpp;

	if (t->idx >= NTRACE)
		return (EINVAL);

	rc = begin_synchronized_op(sc, NULL, HOLD_LOCK | SLEEP_OK | INTR_OK,
	    "t4sett");
	if (rc)
		return (rc);

	/*
	 * If no tracing filter is specified this time then check if the filter
	 * at the index is valid anyway because it was set previously.  If so
	 * then this is a legitimate enable/disable operation.
	 */
	if (t->valid == 0) {
		if (isset(&sc->tracer_valid, t->idx))
			tpp = NULL;
		else
			rc = EINVAL;
		goto done;
	}

	if (t->tp.port > 19 || t->tp.snap_len > 9600 ||
	    t->tp.min_len > M_TFMINPKTSIZE || t->tp.skip_len > M_TFLENGTH ||
	    t->tp.skip_ofst > M_TFOFFSET) {
		rc = EINVAL;
		goto done;
	}

	memcpy(&tp.data[0], &t->tp.data[0], sizeof(tp.data));
	memcpy(&tp.mask[0], &t->tp.mask[0], sizeof(tp.mask));
	tp.snap_len = t->tp.snap_len;
	tp.min_len = t->tp.min_len;
	tp.skip_ofst = t->tp.skip_ofst;
	tp.skip_len = t->tp.skip_len;
	tp.invert = !!t->tp.invert;

	/* convert port to channel iff 0 <= port < 8. */
	if (t->tp.port < 4) {
		if (sc->port[t->tp.port] == NULL) {
			rc = EINVAL;
			goto done;
		}
		tp.port = sc->port[t->tp.port]->tx_chan;
	} else if (t->tp.port < 8) {
		if (sc->port[t->tp.port - 4] == NULL) {
			rc = EINVAL;
			goto done;
		}
		tp.port = sc->port[t->tp.port - 4]->tx_chan + 4;
	}
	tpp = &tp;
done:
	if (rc == 0) {
		rc = -t4_set_trace_filter(sc, tpp, t->idx, t->enabled);
		if (rc == 0) {
			if (t->enabled) {
				setbit(&sc->tracer_valid, t->idx);
				if (sc->tracer_enabled == 0) {
					t4_set_reg_field(sc, A_MPS_TRC_CFG,
					    F_TRCEN, F_TRCEN);
				}
				setbit(&sc->tracer_enabled, t->idx);
			} else {
				clrbit(&sc->tracer_enabled, t->idx);
				if (sc->tracer_enabled == 0) {
					t4_set_reg_field(sc, A_MPS_TRC_CFG,
					    F_TRCEN, 0);
				}
			}
		}
	}
	end_synchronized_op(sc, LOCK_HELD);

	return (rc);
}

int
t4_trace_pkt(struct sge_iq *iq, const struct rss_header *rss, struct mbuf *m)
{
	struct adapter *sc = iq->adapter;
	if_t ifp;

	KASSERT(m != NULL, ("%s: no payload with opcode %02x", __func__,
	    rss->opcode));

	mtx_lock(&sc->ifp_lock);
	ifp = sc->ifp;
	if (sc->ifp) {
		m_adj(m, sizeof(struct cpl_trace_pkt));
		m->m_pkthdr.rcvif = ifp;
		if_mtap(ifp, m, NULL, 0);
	}
	mtx_unlock(&sc->ifp_lock);
	m_freem(m);

	return (0);
}

int
t5_trace_pkt(struct sge_iq *iq, const struct rss_header *rss, struct mbuf *m)
{
	struct adapter *sc = iq->adapter;
	if_t ifp;

	KASSERT(m != NULL, ("%s: no payload with opcode %02x", __func__,
	    rss->opcode));

	mtx_lock(&sc->ifp_lock);
	ifp = sc->ifp;
	if (ifp != NULL) {
		m_adj(m, sizeof(struct cpl_t5_trace_pkt));
		m->m_pkthdr.rcvif = ifp;
		if_mtap(ifp, m, NULL, 0);
	}
	mtx_unlock(&sc->ifp_lock);
	m_freem(m);

	return (0);
}

static int
tracer_ioctl(if_t ifp, unsigned long cmd, void *data, struct thread *td)
{

	switch (cmd) {
	case SIOCSIFMTU:
	case SIOCSIFFLAGS:
	case SIOCADDMULTI:
	case SIOCDELMULTI:
	case SIOCSIFCAP:
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static int
tracer_media_change(if_t ifp, if_media_t media)
{

	return (EOPNOTSUPP);
}

static void
tracer_media_status(if_t ifp, struct ifmediareq *ifmr)
{

	ifmr->ifm_status = IFM_AVALID | IFM_ACTIVE;

	return;
}
