/*
 * Copyright (c) 2008-2016 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

#include "internal.h"
#if DISPATCH_EVENT_BACKEND_KEVENT
#if HAVE_MACH
#include "protocol.h"
#include "protocolServer.h"
#endif

#if DISPATCH_USE_KEVENT_WORKQUEUE && !DISPATCH_USE_KEVENT_QOS
#error unsupported configuration
#endif

#define DISPATCH_KEVENT_MUXED_MARKER  1ul
#define DISPATCH_MACH_AUDIT_TOKEN_PID (5)

typedef struct dispatch_muxnote_s {
	LIST_ENTRY(dispatch_muxnote_s) dmn_list;
	LIST_HEAD(, dispatch_unote_linkage_s) dmn_unotes_head;
	dispatch_kevent_s dmn_kev DISPATCH_ATOMIC64_ALIGN;
} *dispatch_muxnote_t;

LIST_HEAD(dispatch_muxnote_bucket_s, dispatch_muxnote_s);

DISPATCH_STATIC_GLOBAL(bool _dispatch_timers_force_max_leeway);
DISPATCH_STATIC_GLOBAL(dispatch_once_t _dispatch_kq_poll_pred);
DISPATCH_STATIC_GLOBAL(struct dispatch_muxnote_bucket_s _dispatch_sources[DSL_HASH_SIZE]);

#define DISPATCH_NOTE_CLOCK_WALL      NOTE_NSECONDS | NOTE_MACH_CONTINUOUS_TIME
#define DISPATCH_NOTE_CLOCK_MONOTONIC NOTE_MACHTIME | NOTE_MACH_CONTINUOUS_TIME
#define DISPATCH_NOTE_CLOCK_UPTIME	  NOTE_MACHTIME

static const uint32_t _dispatch_timer_index_to_fflags[] = {
#define DISPATCH_TIMER_FFLAGS_INIT(kind, qos, note) \
	[DISPATCH_TIMER_INDEX(DISPATCH_CLOCK_##kind, DISPATCH_TIMER_QOS_##qos)] = \
			DISPATCH_NOTE_CLOCK_##kind | NOTE_ABSOLUTE | NOTE_LEEWAY | (note)
	DISPATCH_TIMER_FFLAGS_INIT(WALL, NORMAL, 0),
	DISPATCH_TIMER_FFLAGS_INIT(UPTIME, NORMAL, 0),
	DISPATCH_TIMER_FFLAGS_INIT(MONOTONIC, NORMAL, 0),
#if DISPATCH_HAVE_TIMER_QOS
	DISPATCH_TIMER_FFLAGS_INIT(WALL, CRITICAL, NOTE_CRITICAL),
	DISPATCH_TIMER_FFLAGS_INIT(UPTIME, CRITICAL, NOTE_CRITICAL),
	DISPATCH_TIMER_FFLAGS_INIT(MONOTONIC, CRITICAL, NOTE_CRITICAL),
	DISPATCH_TIMER_FFLAGS_INIT(WALL, BACKGROUND, NOTE_BACKGROUND),
	DISPATCH_TIMER_FFLAGS_INIT(UPTIME, BACKGROUND, NOTE_BACKGROUND),
	DISPATCH_TIMER_FFLAGS_INIT(MONOTONIC, BACKGROUND, NOTE_BACKGROUND),
#endif
#undef DISPATCH_TIMER_FFLAGS_INIT
};

static inline void _dispatch_kevent_timer_drain(dispatch_kevent_t ke);

#pragma mark -
#pragma mark kevent debug

DISPATCH_NOINLINE
static const char *
_evfiltstr(short filt)
{
	switch (filt) {
#define _evfilt2(f) case (f): return #f
	_evfilt2(EVFILT_READ);
	_evfilt2(EVFILT_WRITE);
	_evfilt2(EVFILT_SIGNAL);
	_evfilt2(EVFILT_TIMER);

#ifdef DISPATCH_EVENT_BACKEND_KEVENT
	_evfilt2(EVFILT_AIO);
	_evfilt2(EVFILT_VNODE);
	_evfilt2(EVFILT_PROC);
#if HAVE_MACH
	_evfilt2(EVFILT_MACHPORT);
	_evfilt2(DISPATCH_EVFILT_MACH_NOTIFICATION);
#endif
	_evfilt2(EVFILT_FS);
	_evfilt2(EVFILT_USER);
#ifdef EVFILT_SOCK
	_evfilt2(EVFILT_SOCK);
#endif
#ifdef EVFILT_MEMORYSTATUS
	_evfilt2(EVFILT_MEMORYSTATUS);
#endif
#endif // DISPATCH_EVENT_BACKEND_KEVENT

	_evfilt2(DISPATCH_EVFILT_TIMER);
	_evfilt2(DISPATCH_EVFILT_TIMER_WITH_CLOCK);
	_evfilt2(DISPATCH_EVFILT_CUSTOM_ADD);
	_evfilt2(DISPATCH_EVFILT_CUSTOM_OR);
	_evfilt2(DISPATCH_EVFILT_CUSTOM_REPLACE);
	default:
		return "EVFILT_missing";
	}
}

#if DISPATCH_DEBUG
static const char *
_evflagstr2(uint16_t *flagsp)
{
#define _evflag2(f) \
	if ((*flagsp & (f)) == (f) && (f)) { \
		*flagsp &= ~(f); \
		return #f "|"; \
	}
	_evflag2(EV_ADD);
	_evflag2(EV_DELETE);
	_evflag2(EV_ENABLE);
	_evflag2(EV_DISABLE);
	_evflag2(EV_ONESHOT);
	_evflag2(EV_CLEAR);
	_evflag2(EV_RECEIPT);
	_evflag2(EV_DISPATCH);
	_evflag2(EV_UDATA_SPECIFIC);
#ifdef EV_POLL
	_evflag2(EV_POLL);
#endif
#ifdef EV_OOBAND
	_evflag2(EV_OOBAND);
#endif
	_evflag2(EV_ERROR);
	_evflag2(EV_EOF);
	_evflag2(EV_VANISHED);
	*flagsp = 0;
	return "EV_UNKNOWN ";
}

DISPATCH_NOINLINE
static const char *
_evflagstr(uint16_t flags, char *str, size_t strsize)
{
	str[0] = 0;
	while (flags) {
		strlcat(str, _evflagstr2(&flags), strsize);
	}
	size_t sz = strlen(str);
	if (sz) str[sz-1] = 0;
	return str;
}

DISPATCH_NOINLINE
static void
dispatch_kevent_debug(const char *verb, const dispatch_kevent_s *kev,
		int i, int n, const char *function, unsigned int line)
{
	char flagstr[256];
	char i_n[31];

	if (n > 1) {
		snprintf(i_n, sizeof(i_n), "%d/%d ", i + 1, n);
	} else {
		i_n[0] = '\0';
	}
	if (verb == NULL) {
		if (kev->flags & EV_DELETE) {
			verb = "deleting";
		} else if (kev->flags & EV_ADD) {
			verb = "adding";
		} else {
			verb = "updating";
		}
	}
#if DISPATCH_USE_KEVENT_QOS
	_dispatch_debug("%s kevent[%p] %s= { ident = 0x%llx, filter = %s, "
			"flags = %s (0x%x), fflags = 0x%x, data = 0x%llx, udata = 0x%llx, "
			"qos = 0x%x, ext[0] = 0x%llx, ext[1] = 0x%llx, ext[2] = 0x%llx, "
			"ext[3] = 0x%llx }: %s #%u", verb, kev, i_n, kev->ident,
			_evfiltstr(kev->filter), _evflagstr(kev->flags, flagstr,
			sizeof(flagstr)), kev->flags, kev->fflags, kev->data, kev->udata,
			kev->qos, kev->ext[0], kev->ext[1], kev->ext[2], kev->ext[3],
			function, line);
#else
	_dispatch_debug("%s kevent[%p] %s= { ident = 0x%llx, filter = %s, "
			"flags = %s (0x%x), fflags = 0x%x, data = 0x%llx, udata = 0x%llx}: "
			"%s #%u", verb, kev, i_n,
			kev->ident, _evfiltstr(kev->filter), _evflagstr(kev->flags, flagstr,
			sizeof(flagstr)), kev->flags, kev->fflags, kev->data, kev->udata,
			function, line);
#endif
}
#else
static inline void
dispatch_kevent_debug(const char *verb, const dispatch_kevent_s *kev,
		int i, int n, const char *function, unsigned int line)
{
	(void)verb; (void)kev; (void)i; (void)n; (void)function; (void)line;
}
#endif // DISPATCH_DEBUG
#define _dispatch_kevent_debug_n(verb, _kev, i, n) \
		dispatch_kevent_debug(verb, _kev, i, n, __FUNCTION__, __LINE__)
#define _dispatch_kevent_debug(verb, _kev) \
		_dispatch_kevent_debug_n(verb, _kev, 0, 0)
#if DISPATCH_MGR_QUEUE_DEBUG
#define _dispatch_kevent_mgr_debug(verb, kev) _dispatch_kevent_debug(verb, kev)
#else
#define _dispatch_kevent_mgr_debug(verb, kev) ((void)verb, (void)kev)
#endif // DISPATCH_MGR_QUEUE_DEBUG
#if DISPATCH_WLH_DEBUG
#define _dispatch_kevent_wlh_debug(verb, kev) _dispatch_kevent_debug(verb, kev)
#else
#define _dispatch_kevent_wlh_debug(verb, kev)  ((void)verb, (void)kev)
#endif // DISPATCH_WLH_DEBUG

#define _dispatch_du_debug(what, du) \
		_dispatch_debug("kevent-source[%p]: %s kevent[%p] " \
				"{ filter = %s, ident = 0x%x }", \
				_dispatch_wref2ptr((du)->du_owner_wref), what, \
				(du), _evfiltstr((du)->du_filter), (du)->du_ident)

#if DISPATCH_MACHPORT_DEBUG
#ifndef MACH_PORT_TYPE_SPREQUEST
#define MACH_PORT_TYPE_SPREQUEST 0x40000000
#endif

DISPATCH_NOINLINE
void
dispatch_debug_machport(mach_port_t name, const char* str)
{
	mach_port_type_t type;
	mach_msg_bits_t ns = 0, nr = 0, nso = 0, nd = 0;
	unsigned int dnreqs = 0, dnrsiz;
	kern_return_t kr = mach_port_type(mach_task_self(), name, &type);
	if (kr) {
		_dispatch_log("machport[0x%08x] = { error(0x%x) \"%s\" }: %s", name,
				kr, mach_error_string(kr), str);
		return;
	}
	if (type & MACH_PORT_TYPE_SEND) {
		(void)dispatch_assume_zero(mach_port_get_refs(mach_task_self(), name,
				MACH_PORT_RIGHT_SEND, &ns));
	}
	if (type & MACH_PORT_TYPE_SEND_ONCE) {
		(void)dispatch_assume_zero(mach_port_get_refs(mach_task_self(), name,
				MACH_PORT_RIGHT_SEND_ONCE, &nso));
	}
	if (type & MACH_PORT_TYPE_DEAD_NAME) {
		(void)dispatch_assume_zero(mach_port_get_refs(mach_task_self(), name,
				MACH_PORT_RIGHT_DEAD_NAME, &nd));
	}
	if (type & (MACH_PORT_TYPE_RECEIVE|MACH_PORT_TYPE_SEND)) {
		kr = mach_port_dnrequest_info(mach_task_self(), name, &dnrsiz, &dnreqs);
		if (kr != KERN_INVALID_RIGHT) (void)dispatch_assume_zero(kr);
	}
	if (type & MACH_PORT_TYPE_RECEIVE) {
		mach_port_status_t status = { .mps_pset = 0, };
		mach_msg_type_number_t cnt = MACH_PORT_RECEIVE_STATUS_COUNT;
		(void)dispatch_assume_zero(mach_port_get_refs(mach_task_self(), name,
				MACH_PORT_RIGHT_RECEIVE, &nr));
		(void)dispatch_assume_zero(mach_port_get_attributes(mach_task_self(),
				name, MACH_PORT_RECEIVE_STATUS, (void*)&status, &cnt));
		_dispatch_log("machport[0x%08x] = { R(%03u) S(%03u) SO(%03u) D(%03u) "
				"dnreqs(%03u) spreq(%s) nsreq(%s) pdreq(%s) srights(%s) "
				"sorights(%03u) qlim(%03u) msgcount(%03u) mkscount(%03u) "
				"seqno(%03u) }: %s", name, nr, ns, nso, nd, dnreqs,
				type & MACH_PORT_TYPE_SPREQUEST ? "Y":"N",
				status.mps_nsrequest ? "Y":"N", status.mps_pdrequest ? "Y":"N",
				status.mps_srights ? "Y":"N", status.mps_sorights,
				status.mps_qlimit, status.mps_msgcount, status.mps_mscount,
				status.mps_seqno, str);
	} else if (type & (MACH_PORT_TYPE_SEND|MACH_PORT_TYPE_SEND_ONCE|
			MACH_PORT_TYPE_DEAD_NAME)) {
		_dispatch_log("machport[0x%08x] = { R(%03u) S(%03u) SO(%03u) D(%03u) "
				"dnreqs(%03u) spreq(%s) }: %s", name, nr, ns, nso, nd, dnreqs,
				type & MACH_PORT_TYPE_SPREQUEST ? "Y":"N", str);
	} else {
		_dispatch_log("machport[0x%08x] = { type(0x%08x) }: %s", name, type,
				str);
	}
}
#endif

#pragma mark dispatch_kevent_t

#if HAVE_MACH

DISPATCH_STATIC_GLOBAL(dispatch_once_t _dispatch_mach_host_port_pred);
DISPATCH_STATIC_GLOBAL(mach_port_t _dispatch_mach_host_port);

static inline void*
_dispatch_kevent_mach_msg_buf(dispatch_kevent_t ke)
{
	return (void*)ke->ext[0];
}

static inline mach_msg_size_t
_dispatch_kevent_mach_msg_size(dispatch_kevent_t ke)
{
	// buffer size in the successful receive case, but message size (like
	// msgh_size) in the MACH_RCV_TOO_LARGE case, i.e. add trailer size.
	return (mach_msg_size_t)ke->ext[1];
}

static void _dispatch_kevent_mach_msg_drain(dispatch_kevent_t ke);
static inline void _dispatch_mach_host_calendar_change_register(void);

// DISPATCH_MACH_NOTIFICATION_ARMED are muxnotes that aren't registered with
// kevent for real, but with mach_port_request_notification()
//
// the kevent structure is used for bookkeeping:
// - ident, filter, flags and fflags have their usual meaning
// - data is used to monitor the actual state of the
//   mach_port_request_notification()
// - ext[0] is a boolean that trackes whether the notification is armed or not
#define DISPATCH_MACH_NOTIFICATION_ARMED(dmn) ((dmn)->dmn_kev.ext[0])
#endif

DISPATCH_ALWAYS_INLINE
static dispatch_muxnote_t
_dispatch_kevent_get_muxnote(dispatch_kevent_t ke)
{
	uintptr_t dmn_addr = (uintptr_t)ke->udata & ~DISPATCH_KEVENT_MUXED_MARKER;
	return (dispatch_muxnote_t)dmn_addr;
}

DISPATCH_ALWAYS_INLINE
static dispatch_unote_t
_dispatch_kevent_get_unote(dispatch_kevent_t ke)
{
	dispatch_assert((ke->udata & DISPATCH_KEVENT_MUXED_MARKER) == 0);
	return (dispatch_unote_t){ ._du = (dispatch_unote_class_t)ke->udata };
}

DISPATCH_NOINLINE
static void
_dispatch_kevent_print_error(dispatch_kevent_t ke)
{
	dispatch_unote_class_t du = NULL;
	_dispatch_debug("kevent[0x%llx]: handling error",
			(unsigned long long)ke->udata);
	if (ke->flags & EV_DELETE) {
		if (ke->flags & EV_UDATA_SPECIFIC) {
			if (ke->data == EINPROGRESS) {
				// deferred EV_DELETE
				return;
			}
		}
		// for EV_DELETE if the update was deferred we may have reclaimed
		// the udata already, and it is unsafe to dereference it now.
	} else if (ke->udata & DISPATCH_KEVENT_MUXED_MARKER) {
		ke->flags |= _dispatch_kevent_get_muxnote(ke)->dmn_kev.flags;
	} else if (ke->udata) {
		du = (dispatch_unote_class_t)(uintptr_t)ke->udata;
		if (!_dispatch_unote_registered(du)) {
			ke->flags |= EV_ADD;
		}
	}

	switch (ke->data) {
	case 0:
		return;
	case ERANGE: /* A broken QoS was passed to kevent_id() */
		DISPATCH_INTERNAL_CRASH(ke->qos, "Invalid kevent priority");
	default:
		// log the unexpected error
		_dispatch_bug_kevent_client("kevent", _evfiltstr(ke->filter),
				!ke->udata ? NULL :
				ke->flags & EV_DELETE ? "delete" :
				ke->flags & EV_ADD ? "add" :
				ke->flags & EV_ENABLE ? "enable" : "monitor",
				(int)ke->data, ke->ident, ke->udata, du);
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_kevent_merge_ev_flags(dispatch_unote_t du, uint32_t flags)
{
	if (unlikely(!(flags & EV_UDATA_SPECIFIC) && (flags & EV_ONESHOT))) {
		_dispatch_unote_unregister(du, DUU_DELETE_ACK | DUU_MUST_SUCCEED);
		return;
	}

	if (flags & EV_DELETE) {
		// When a speculative deletion is requested by libdispatch,
		// and the kernel is about to deliver an event, it can acknowledge
		// our wish by delivering the event as a (EV_DELETE | EV_ONESHOT)
		// event and dropping the knote at once.
		_dispatch_unote_state_set(du, DU_STATE_NEEDS_DELETE);
	} else if (flags & (EV_ONESHOT | EV_VANISHED)) {
		// EV_VANISHED events if re-enabled will produce another EV_VANISHED
		// event. To avoid an infinite loop of such events, mark the unote
		// as needing deletion so that _dispatch_unote_needs_rearm()
		// eventually returns false.
		//
		// mach channels crash on EV_VANISHED, and dispatch sources stay
		// in a limbo until canceled (explicitly or not).
		dispatch_unote_state_t du_state = _dispatch_unote_state(du);
		du_state |= DU_STATE_NEEDS_DELETE;
		du_state &= ~DU_STATE_ARMED;
		_dispatch_unote_state_set(du, du_state);
	} else if (likely(flags & EV_DISPATCH)) {
		_dispatch_unote_state_clear_bit(du, DU_STATE_ARMED);
	} else {
		return;
	}

	_dispatch_du_debug((flags & EV_VANISHED) ? "vanished" :
			(flags & EV_DELETE) ? "deleted oneshot" :
			(flags & EV_ONESHOT) ? "oneshot" : "disarmed", du._du);
}

DISPATCH_NOINLINE
static void
_dispatch_kevent_merge(dispatch_unote_t du, dispatch_kevent_t ke)
{
	dispatch_unote_action_t action = dux_type(du._du)->dst_action;
	pthread_priority_t pp = 0;
	uintptr_t data;

	// once we modify the queue atomic flags below, it will allow concurrent
	// threads running _dispatch_source_invoke2 to dispose of the source,
	// so we can't safely borrow the reference we get from the muxnote udata
	// anymore, and need our own <rdar://20382435>
	_dispatch_retain_unote_owner(du);

	switch (action) {
	case DISPATCH_UNOTE_ACTION_PASS_DATA:
		data = (uintptr_t)ke->data;
		break;

	case DISPATCH_UNOTE_ACTION_PASS_FFLAGS:
		data = (uintptr_t)ke->fflags;
#if HAVE_MACH
		if (du._du->du_filter == EVFILT_MACHPORT) {
			data = DISPATCH_MACH_RECV_MESSAGE;
		}
#endif
		break;

	case DISPATCH_UNOTE_ACTION_SOURCE_SET_DATA:
		// ke->data is signed and "negative available data" makes no sense
		// zero bytes happens when EV_EOF is set
		dispatch_assert(ke->data >= 0l);
		data = (unsigned long)ke->data;
		os_atomic_store2o(du._dr, ds_pending_data, ~data, relaxed);
		break;

	case DISPATCH_UNOTE_ACTION_SOURCE_ADD_DATA:
		data = (unsigned long)ke->data;
		if (data) os_atomic_add2o(du._dr, ds_pending_data, data, relaxed);
		break;

	case DISPATCH_UNOTE_ACTION_SOURCE_OR_FFLAGS:
		data = ke->fflags & du._du->du_fflags;
		if (du._dr->du_has_extended_status) {
			uint64_t odata, ndata, value;
			uint32_t status = (uint32_t)ke->data;

			// We combine the data and status into a single 64-bit value.
			value = DISPATCH_SOURCE_COMBINE_DATA_AND_STATUS(data, status);
			os_atomic_rmw_loop2o(du._dr, ds_pending_data, odata, ndata, relaxed, {
				ndata = DISPATCH_SOURCE_GET_DATA(odata) | value;
			});
#if HAVE_MACH
		} else if (du._du->du_filter == EVFILT_MACHPORT) {
			data = DISPATCH_MACH_RECV_MESSAGE;
			os_atomic_store2o(du._dr, ds_pending_data, data, relaxed);
#endif
		} else {
			if (data) os_atomic_or2o(du._dr, ds_pending_data, data, relaxed);
		}
		break;

	default:
		DISPATCH_INTERNAL_CRASH(action, "Corrupt unote action");
	}

	_dispatch_kevent_merge_ev_flags(du, ke->flags);
#if DISPATCH_USE_KEVENT_QOS
	pp = ((pthread_priority_t)ke->qos) & ~_PTHREAD_PRIORITY_FLAGS_MASK;
#endif
	return dux_merge_evt(du._du, ke->flags, data, pp);
}

DISPATCH_NOINLINE
static void
_dispatch_kevent_merge_muxed(dispatch_kevent_t ke)
{
	dispatch_muxnote_t dmn = _dispatch_kevent_get_muxnote(ke);
	dispatch_unote_linkage_t dul, dul_next;

	if (ke->flags & (EV_ONESHOT | EV_DELETE)) {
		// tell _dispatch_unote_unregister_muxed() the kernel half is gone
		dmn->dmn_kev.flags |= EV_DELETE;
	}
	LIST_FOREACH_SAFE(dul, &dmn->dmn_unotes_head, du_link, dul_next) {
		_dispatch_kevent_merge(_dispatch_unote_linkage_get_unote(dul), ke);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_kevent_drain(dispatch_kevent_t ke)
{
	if (ke->filter == EVFILT_USER) {
		_dispatch_kevent_mgr_debug("received", ke);
		return;
	}
	_dispatch_kevent_debug("received", ke);
	if (unlikely(ke->flags & EV_ERROR)) {
		if (ke->filter == EVFILT_PROC && ke->data == ESRCH) {
			// <rdar://problem/5067725&6626350> EVFILT_PROC may fail with ESRCH
			// when the process exists but is a zombie. As a workaround, we
			// simulate an exit event for any EVFILT_PROC with an invalid pid.
			ke->flags  = EV_UDATA_SPECIFIC | EV_ONESHOT | EV_DELETE;
			ke->fflags = NOTE_EXIT;
			ke->data   = 0;
			_dispatch_kevent_debug("synthetic NOTE_EXIT", ke);
		} else {
			return _dispatch_kevent_print_error(ke);
		}
	}
	if (ke->filter == EVFILT_TIMER) {
		return _dispatch_kevent_timer_drain(ke);
	}

#if HAVE_MACH
	if (ke->filter == EVFILT_MACHPORT && _dispatch_kevent_mach_msg_size(ke)) {
		return _dispatch_kevent_mach_msg_drain(ke);
	}
#endif

	if (ke->udata & DISPATCH_KEVENT_MUXED_MARKER) {
		return _dispatch_kevent_merge_muxed(ke);
	}
	return _dispatch_kevent_merge(_dispatch_kevent_get_unote(ke), ke);
}

#pragma mark dispatch_kq

#if DISPATCH_USE_MGR_THREAD
DISPATCH_NOINLINE
static void
_dispatch_kq_create(intptr_t *fd_ptr)
{
	static const dispatch_kevent_s kev = {
		.ident = 1,
		.filter = EVFILT_USER,
		.flags = EV_ADD|EV_CLEAR,
		.udata = (uintptr_t)DISPATCH_WLH_MANAGER,
	};
	int kqfd;

	_dispatch_fork_becomes_unsafe();
#if DISPATCH_USE_GUARDED_FD
	guardid_t guard = (uintptr_t)fd_ptr;
	kqfd = guarded_kqueue_np(&guard, GUARD_CLOSE | GUARD_DUP);
#else
	(void)guard_ptr;
	kqfd = kqueue();
#endif
	if (kqfd == -1) {
		int err = errno;
		switch (err) {
		case EMFILE:
			DISPATCH_CLIENT_CRASH(err, "kqueue() failure: "
					"process is out of file descriptors");
			break;
		case ENFILE:
			DISPATCH_CLIENT_CRASH(err, "kqueue() failure: "
					"system is out of file descriptors");
			break;
		case ENOMEM:
			DISPATCH_CLIENT_CRASH(err, "kqueue() failure: "
					"kernel is out of memory");
			break;
		default:
			DISPATCH_INTERNAL_CRASH(err, "kqueue() failure");
			break;
		}
	}
#if DISPATCH_USE_KEVENT_QOS
	dispatch_assume_zero(kevent_qos(kqfd, &kev, 1, NULL, 0, NULL, NULL, 0));
#else
	dispatch_assume_zero(kevent(kqfd, &kev, 1, NULL, 0, NULL));
#endif
	*fd_ptr = kqfd;
}
#endif

static inline int
_dispatch_kq_fd(void)
{
	return (int)(intptr_t)_dispatch_mgr_q.do_ctxt;
}

static void
_dispatch_kq_init(void *context)
{
	bool *kq_initialized = context;

	_dispatch_fork_becomes_unsafe();
	if (unlikely(getenv("LIBDISPATCH_TIMERS_FORCE_MAX_LEEWAY"))) {
		_dispatch_timers_force_max_leeway = true;
	}
	*kq_initialized = true;

#if DISPATCH_USE_KEVENT_WORKQUEUE
	_dispatch_kevent_workqueue_init();
	if (_dispatch_kevent_workqueue_enabled) {
		int r;
		int kqfd = _dispatch_kq_fd();
		const dispatch_kevent_s ke = {
			.ident = 1,
			.filter = EVFILT_USER,
			.flags = EV_ADD|EV_CLEAR,
			.qos = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG,
			.udata = (uintptr_t)DISPATCH_WLH_MANAGER,
		};
retry:
		r = kevent_qos(kqfd, &ke, 1, NULL, 0, NULL, NULL,
				KEVENT_FLAG_WORKQ|KEVENT_FLAG_IMMEDIATE);
		if (unlikely(r == -1)) {
			int err = errno;
			switch (err) {
			case EINTR:
				goto retry;
			default:
				DISPATCH_CLIENT_CRASH(err,
						"Failed to initalize workqueue kevent");
				break;
			}
		}
		return;
	}
#endif // DISPATCH_USE_KEVENT_WORKQUEUE
#if DISPATCH_USE_MGR_THREAD
	_dispatch_kq_create((intptr_t *)&_dispatch_mgr_q.do_ctxt);
	_dispatch_trace_item_push(_dispatch_mgr_q.do_targetq, &_dispatch_mgr_q);
	dx_push(_dispatch_mgr_q.do_targetq, &_dispatch_mgr_q, 0);
#endif // DISPATCH_USE_MGR_THREAD
}

#if DISPATCH_USE_MEMORYPRESSURE_SOURCE
static void _dispatch_memorypressure_init(void);
#else
#define _dispatch_memorypressure_init() ((void)0)
#endif

DISPATCH_NOINLINE
static int
_dispatch_kq_poll(dispatch_wlh_t wlh, dispatch_kevent_t ke, int n,
		dispatch_kevent_t ke_out, int n_out, void *buf, size_t *avail,
		uint32_t flags)
{
	bool kq_initialized = false;
	int r = 0;

	dispatch_once_f(&_dispatch_kq_poll_pred, &kq_initialized, _dispatch_kq_init);
	if (unlikely(kq_initialized)) {
		// The calling thread was the one doing the initialization
		//
		// The event loop needs the memory pressure source and debug channel,
		// however creating these will recursively call _dispatch_kq_poll(),
		// so we can't quite initialize them under the dispatch once.
		_dispatch_memorypressure_init();
		_voucher_activity_debug_channel_init();
	}

#if !DISPATCH_USE_KEVENT_QOS
	if (flags & KEVENT_FLAG_ERROR_EVENTS) {
		// emulate KEVENT_FLAG_ERROR_EVENTS
		for (r = 0; r < n; r++) {
			ke[r].flags |= EV_RECEIPT;
		}
		out_n = n;
	}
#endif

retry:
	if (unlikely(wlh == NULL)) {
		DISPATCH_INTERNAL_CRASH(wlh, "Invalid wlh");
	} else if (wlh == DISPATCH_WLH_ANON) {
		int kqfd = _dispatch_kq_fd();
#if DISPATCH_USE_KEVENT_QOS
		if (_dispatch_kevent_workqueue_enabled) {
			flags |= KEVENT_FLAG_WORKQ;
		}
		r = kevent_qos(kqfd, ke, n, ke_out, n_out, buf, avail, flags);
#else
		const struct timespec timeout_immediately = {}, *timeout = NULL;
		if (flags & KEVENT_FLAG_IMMEDIATE) timeout = &timeout_immediately;
		r = kevent(kqfd, ke, n, ke_out, n_out, timeout);
#endif
	}
	if (unlikely(r == -1)) {
		int err = errno;
		switch (err) {
		case ENOMEM:
			_dispatch_temporary_resource_shortage();
			/* FALLTHROUGH */
		case EINTR:
			goto retry;
		case EBADF:
			DISPATCH_CLIENT_CRASH(err, "Do not close random Unix descriptors");
		default:
			DISPATCH_CLIENT_CRASH(err, "Unexpected error from kevent");
		}
	}
	return r;
}

DISPATCH_NOINLINE
static int
_dispatch_kq_drain(dispatch_wlh_t wlh, dispatch_kevent_t ke, int n,
		uint32_t flags)
{
	dispatch_kevent_s ke_out[DISPATCH_DEFERRED_ITEMS_EVENT_COUNT];
	bool poll_for_events = !(flags & KEVENT_FLAG_ERROR_EVENTS);
	int i, n_out = countof(ke_out), r = 0;
	size_t *avail = NULL;
	void *buf = NULL;

#if DISPATCH_USE_KEVENT_QOS
	size_t size;
	if (poll_for_events) {
		size = DISPATCH_MACH_RECEIVE_MAX_INLINE_MESSAGE_SIZE +
				DISPATCH_MACH_TRAILER_SIZE;
		buf = alloca(size);
		avail = &size;
	}
#endif

#if DISPATCH_DEBUG
	for (r = 0; r < n; r++) {
		if (ke[r].filter != EVFILT_USER || DISPATCH_MGR_QUEUE_DEBUG) {
			_dispatch_kevent_debug_n(NULL, ke + r, r, n);
		}
	}
#endif

	if (poll_for_events) _dispatch_clear_return_to_kernel();
	n = _dispatch_kq_poll(wlh, ke, n, ke_out, n_out, buf, avail, flags);
	if (n == 0) {
		r = 0;
	} else if (flags & KEVENT_FLAG_ERROR_EVENTS) {
		for (i = 0, r = 0; i < n; i++) {
			if ((ke_out[i].flags & EV_ERROR) && ke_out[i].data) {
				_dispatch_kevent_drain(&ke_out[i]);
				r = (int)ke_out[i].data;
			}
		}
	} else {
		for (i = 0, r = 0; i < n; i++) {
			_dispatch_kevent_drain(&ke_out[i]);
		}
	}
	return r;
}

DISPATCH_ALWAYS_INLINE
static inline int
_dispatch_kq_update_one(dispatch_wlh_t wlh, dispatch_kevent_t ke)
{
	return _dispatch_kq_drain(wlh, ke, 1,
			KEVENT_FLAG_IMMEDIATE | KEVENT_FLAG_ERROR_EVENTS);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_kq_update_all(dispatch_wlh_t wlh, dispatch_kevent_t ke, int n)
{
	(void)_dispatch_kq_drain(wlh, ke, n,
			KEVENT_FLAG_IMMEDIATE | KEVENT_FLAG_ERROR_EVENTS);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_kq_unote_set_kevent(dispatch_unote_t _du, dispatch_kevent_t dk,
		uint16_t action)
{
	dispatch_unote_class_t du = _du._du;
	dispatch_source_type_t dst = dux_type(du);
	uint16_t flags = dst->dst_flags | action;

	if ((flags & EV_VANISHED) && !(flags & EV_ADD)) {
		flags &= ~EV_VANISHED;
	}

	*dk = (dispatch_kevent_s){
		.ident  = du->du_ident,
		.filter = dst->dst_filter,
		.flags  = flags,
		.udata  = (uintptr_t)du,
		.fflags = du->du_fflags | dst->dst_fflags,
		.data   = (typeof(dk->data))dst->dst_data,
#if DISPATCH_USE_KEVENT_QOS
		.qos    = (typeof(dk->qos))_dispatch_priority_to_pp_prefer_fallback(
				du->du_priority),
#endif
	};
}

DISPATCH_ALWAYS_INLINE
static inline int
_dispatch_kq_deferred_find_slot(dispatch_deferred_items_t ddi,
		int16_t filter, uint64_t ident, uint64_t udata)
{
	dispatch_kevent_t events = ddi->ddi_eventlist;
	int i;

	for (i = 0; i < ddi->ddi_nevents; i++) {
		if (events[i].filter == filter && events[i].ident == ident &&
				events[i].udata == udata) {
			break;
		}
	}
	return i;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_kevent_t
_dispatch_kq_deferred_reuse_slot(dispatch_wlh_t wlh,
		dispatch_deferred_items_t ddi, int slot)
{
	if (wlh != DISPATCH_WLH_ANON) _dispatch_set_return_to_kernel();
	if (unlikely(slot == ddi->ddi_maxevents)) {
		int nevents = ddi->ddi_nevents;
		ddi->ddi_nevents = 1;
		_dispatch_kq_update_all(wlh, ddi->ddi_eventlist, nevents);
		dispatch_assert(ddi->ddi_nevents == 1);
		slot = 0;
	} else if (slot == ddi->ddi_nevents) {
		ddi->ddi_nevents++;
	}
	return ddi->ddi_eventlist + slot;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_kq_deferred_discard_slot(dispatch_deferred_items_t ddi, int slot)
{
	if (slot < ddi->ddi_nevents) {
		int last = --ddi->ddi_nevents;
		if (slot != last) {
			ddi->ddi_eventlist[slot] = ddi->ddi_eventlist[last];
		}
	}
}

DISPATCH_NOINLINE
static void
_dispatch_kq_deferred_update(dispatch_wlh_t wlh, dispatch_kevent_t ke)
{
	dispatch_deferred_items_t ddi = _dispatch_deferred_items_get();

	if (ddi && ddi->ddi_wlh == wlh && ddi->ddi_maxevents) {
		int slot = _dispatch_kq_deferred_find_slot(ddi, ke->filter, ke->ident,
				ke->udata);
		dispatch_kevent_t dk = _dispatch_kq_deferred_reuse_slot(wlh, ddi, slot);
		*dk = *ke;
		if (ke->filter != EVFILT_USER) {
			_dispatch_kevent_mgr_debug("deferred", ke);
		}
	} else {
		_dispatch_kq_update_one(wlh, ke);
	}
}

DISPATCH_NOINLINE
static int
_dispatch_kq_immediate_update(dispatch_wlh_t wlh, dispatch_kevent_t ke)
{
	dispatch_deferred_items_t ddi = _dispatch_deferred_items_get();
	if (ddi && ddi->ddi_wlh == wlh) {
		int slot = _dispatch_kq_deferred_find_slot(ddi, ke->filter, ke->ident,
				ke->udata);
		_dispatch_kq_deferred_discard_slot(ddi, slot);
	}
	return _dispatch_kq_update_one(wlh, ke);
}

DISPATCH_NOINLINE
static bool
_dispatch_kq_unote_update(dispatch_wlh_t wlh, dispatch_unote_t _du,
		uint16_t action_flags)
{
	dispatch_deferred_items_t ddi = _dispatch_deferred_items_get();
	dispatch_unote_class_t du = _du._du;
	dispatch_kevent_t ke;
	int r = 0;

	if (action_flags & EV_ADD) {
		// as soon as we register we may get an event delivery and it has to
		// see du_state already set, else it will not unregister the kevent
		_dispatch_wlh_retain(wlh);
		_dispatch_unote_state_set(du, wlh, DU_STATE_ARMED);
	}

	if (ddi && ddi->ddi_wlh == wlh) {
		int slot = _dispatch_kq_deferred_find_slot(ddi,
				du->du_filter, du->du_ident, (uintptr_t)du);
		if (slot < ddi->ddi_nevents) {
			// <rdar://problem/26202376> when deleting and an enable is pending,
			// we must merge EV_ENABLE to do an immediate deletion
			action_flags |= (ddi->ddi_eventlist[slot].flags & EV_ENABLE);
		}

		if (!(action_flags & EV_ADD) && (action_flags & EV_ENABLE)) {
			// can be deferred, so do it!
			ke = _dispatch_kq_deferred_reuse_slot(wlh, ddi, slot);
			_dispatch_kq_unote_set_kevent(du, ke, action_flags);
			_dispatch_kevent_debug("deferred", ke);
			goto done;
		}

		// get rid of the deferred item if any, we can't wait
		_dispatch_kq_deferred_discard_slot(ddi, slot);
	}

	if (action_flags) {
		dispatch_kevent_s dk;
		_dispatch_kq_unote_set_kevent(du, &dk, action_flags);
		r = _dispatch_kq_update_one(wlh, &dk);
	}

done:
	if (action_flags & EV_ADD) {
		if (unlikely(r)) {
			_dispatch_wlh_release(wlh);
			_dispatch_unote_state_set(du, DU_STATE_UNREGISTERED);
		} else {
			_dispatch_du_debug("installed", du);
		}
		return r == 0;
	}

	if (action_flags & EV_DELETE) {
		if (r == EINPROGRESS) {
			_dispatch_du_debug("deferred delete", du);
			return false;
		}
		_dispatch_wlh_release(wlh);
		_dispatch_unote_state_set(du, DU_STATE_UNREGISTERED);
		_dispatch_du_debug("deleted", du);
	} else if (action_flags & EV_ENABLE) {
		_dispatch_du_debug("rearmed", du);
	}

	dispatch_assume_zero(r);
	return true;
}

#pragma mark dispatch_muxnote_t

DISPATCH_ALWAYS_INLINE
static inline struct dispatch_muxnote_bucket_s *
_dispatch_muxnote_bucket(uint64_t ident, int16_t filter)
{
	switch (filter) {
#if HAVE_MACH
	case EVFILT_MACHPORT:
	case DISPATCH_EVFILT_MACH_NOTIFICATION:
		ident = MACH_PORT_INDEX(ident);
		break;
#endif
	case EVFILT_SIGNAL: // signo
	case EVFILT_PROC: // pid_t
	default: // fd
		break;
	}

	return &_dispatch_sources[DSL_HASH((uintptr_t)ident)];
}
#define _dispatch_unote_muxnote_bucket(du) \
	_dispatch_muxnote_bucket(du._du->du_ident, du._du->du_filter)

DISPATCH_ALWAYS_INLINE
static inline dispatch_muxnote_t
_dispatch_muxnote_find(struct dispatch_muxnote_bucket_s *dmb,
		uint64_t ident, int16_t filter)
{
	dispatch_muxnote_t dmn;
	LIST_FOREACH(dmn, dmb, dmn_list) {
		if (dmn->dmn_kev.ident == ident && dmn->dmn_kev.filter == filter) {
			break;
		}
	}
	return dmn;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_muxnote_t
_dispatch_mach_muxnote_find(mach_port_t name, int16_t filter)
{
	struct dispatch_muxnote_bucket_s *dmb;
	dmb = _dispatch_muxnote_bucket(name, filter);
	return _dispatch_muxnote_find(dmb, name, filter);
}

bool
_dispatch_unote_register_muxed(dispatch_unote_t du)
{
	struct dispatch_muxnote_bucket_s *dmb = _dispatch_unote_muxnote_bucket(du);
	dispatch_muxnote_t dmn;
	bool installed = true;

	dmn = _dispatch_muxnote_find(dmb, du._du->du_ident, du._du->du_filter);
	if (dmn) {
		uint32_t flags = du._du->du_fflags & ~dmn->dmn_kev.fflags;
		if (flags) {
			dmn->dmn_kev.fflags |= flags;
			if (unlikely(dux_type(du._du)->dst_update_mux)) {
				installed = dux_type(du._du)->dst_update_mux(dmn);
			} else {
				installed = !_dispatch_kq_immediate_update(DISPATCH_WLH_ANON,
						&dmn->dmn_kev);
			}
			if (!installed) dmn->dmn_kev.fflags &= ~flags;
		}
	} else {
		dmn = _dispatch_calloc(1, sizeof(struct dispatch_muxnote_s));
		_dispatch_kq_unote_set_kevent(du, &dmn->dmn_kev, EV_ADD | EV_ENABLE);
#if DISPATCH_USE_KEVENT_QOS
		dmn->dmn_kev.qos = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;
#endif
		dmn->dmn_kev.udata = (uintptr_t)dmn | DISPATCH_KEVENT_MUXED_MARKER;
		if (unlikely(dux_type(du._du)->dst_update_mux)) {
			installed = dux_type(du._du)->dst_update_mux(dmn);
		} else {
			installed = !_dispatch_kq_immediate_update(DISPATCH_WLH_ANON,
					&dmn->dmn_kev);
		}
		if (installed) {
			dmn->dmn_kev.flags &= ~(EV_ADD | EV_VANISHED);
			LIST_INSERT_HEAD(dmb, dmn, dmn_list);
		} else {
			free(dmn);
		}
	}

	if (installed) {
		dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);
		LIST_INSERT_HEAD(&dmn->dmn_unotes_head, dul, du_link);
		if (du._du->du_filter == DISPATCH_EVFILT_MACH_NOTIFICATION) {
			os_atomic_store2o(du._dmsr, dmsr_notification_armed,
					DISPATCH_MACH_NOTIFICATION_ARMED(dmn), relaxed);
		}
		dul->du_muxnote = dmn;
		_dispatch_unote_state_set(du, DISPATCH_WLH_ANON, DU_STATE_ARMED);
		_dispatch_du_debug("installed", du._du);
	}
	return installed;
}

void
_dispatch_unote_resume_muxed(dispatch_unote_t du)
{
	_dispatch_unote_state_set_bit(du, DU_STATE_ARMED);
	if (unlikely(dux_type(du._du)->dst_update_mux)) {
		dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);
		dux_type(du._du)->dst_update_mux(dul->du_muxnote);
	} else {
		dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);
		dispatch_muxnote_t dmn = dul->du_muxnote;
		_dispatch_kq_deferred_update(DISPATCH_WLH_ANON, &dmn->dmn_kev);
	}
}

bool
_dispatch_unote_unregister_muxed(dispatch_unote_t du)
{
	dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);
	dispatch_muxnote_t dmn = dul->du_muxnote;
	bool update = false, dispose = false;

	if (dmn->dmn_kev.filter == DISPATCH_EVFILT_MACH_NOTIFICATION) {
		os_atomic_store2o(du._dmsr, dmsr_notification_armed, false, relaxed);
	}
	_dispatch_unote_state_set(du, DU_STATE_UNREGISTERED);
	LIST_REMOVE(dul, du_link);
	_LIST_TRASH_ENTRY(dul, du_link);
	dul->du_muxnote = NULL;

	if (LIST_EMPTY(&dmn->dmn_unotes_head)) {
		dispose = true;
		update = !(dmn->dmn_kev.flags & EV_DELETE);
		dmn->dmn_kev.flags |= EV_DELETE;
	} else {
		uint32_t fflags = dux_type(du._du)->dst_fflags;
		LIST_FOREACH(dul, &dmn->dmn_unotes_head, du_link) {
			du = _dispatch_unote_linkage_get_unote(dul);
			fflags |= du._du->du_fflags;
		}
		if (dmn->dmn_kev.fflags & ~fflags) {
			dmn->dmn_kev.fflags &= fflags;
			update = true;
		}
	}
	if (update) {
		if (unlikely(dux_type(du._du)->dst_update_mux)) {
			dispatch_assume(dux_type(du._du)->dst_update_mux(dmn));
		} else {
			_dispatch_kq_deferred_update(DISPATCH_WLH_ANON, &dmn->dmn_kev);
		}
	}
	if (dispose) {
		LIST_REMOVE(dmn, dmn_list);
		free(dmn);
	}
	_dispatch_du_debug("deleted", du._du);
	return true;
}

#if DISPATCH_HAVE_DIRECT_KNOTES
bool
_dispatch_unote_register_direct(dispatch_unote_t du, dispatch_wlh_t wlh)
{
	return _dispatch_kq_unote_update(wlh, du, EV_ADD | EV_ENABLE);
}

void
_dispatch_unote_resume_direct(dispatch_unote_t du)
{
	_dispatch_unote_state_set_bit(du, DU_STATE_ARMED);
	_dispatch_kq_unote_update(_dispatch_unote_wlh(du), du, EV_ENABLE);
}

bool
_dispatch_unote_unregister_direct(dispatch_unote_t du, uint32_t flags)
{
	dispatch_unote_state_t du_state = _dispatch_unote_state(du);
	dispatch_wlh_t du_wlh = _du_state_wlh(du_state);
	dispatch_deferred_items_t ddi = _dispatch_deferred_items_get();
	uint16_t action = EV_DELETE;
	if (likely(du_wlh != DISPATCH_WLH_ANON && ddi && ddi->ddi_wlh == du_wlh)) {
		action |= EV_ENABLE;
		flags |= DUU_DELETE_ACK | DUU_MUST_SUCCEED;
	}

	if (!_du_state_needs_delete(du_state) || (flags & DUU_DELETE_ACK)) {
		if (du_state == DU_STATE_NEEDS_DELETE) {
			// There is no knote to unregister anymore, just do it.
			_dispatch_unote_state_set(du, DU_STATE_UNREGISTERED);
			_dispatch_du_debug("acknowledged deleted oneshot", du._du);
			return true;
		}
		if (!_du_state_armed(du_state)) {
			action |= EV_ENABLE;
			flags |= DUU_MUST_SUCCEED;
		}
		if ((action & EV_ENABLE) || (flags & DUU_PROBE)) {
			if (_dispatch_kq_unote_update(du_wlh, du, action)) {
				return true;
			}
		}
	}
	if (flags & DUU_MUST_SUCCEED) {
		DISPATCH_INTERNAL_CRASH(0, "Unregistration failed");
	}
	return false;
}
#endif // DISPATCH_HAVE_DIRECT_KNOTES

#pragma mark -
#pragma mark dispatch_event_loop

enum {
	DISPATCH_WORKLOOP_ASYNC,
	DISPATCH_WORKLOOP_ASYNC_FROM_SYNC,
	DISPATCH_WORKLOOP_ASYNC_DISCOVER_SYNC,
	DISPATCH_WORKLOOP_ASYNC_QOS_UPDATE,
	DISPATCH_WORKLOOP_ASYNC_LEAVE,
	DISPATCH_WORKLOOP_ASYNC_LEAVE_FROM_SYNC,
	DISPATCH_WORKLOOP_ASYNC_LEAVE_FROM_TRANSFER,
	DISPATCH_WORKLOOP_ASYNC_FORCE_END_OWNERSHIP,
	DISPATCH_WORKLOOP_RETARGET,

	DISPATCH_WORKLOOP_SYNC_WAIT,
	DISPATCH_WORKLOOP_SYNC_WAKE,
	DISPATCH_WORKLOOP_SYNC_FAKE,
	DISPATCH_WORKLOOP_SYNC_END,
};

static char const * const _dispatch_workloop_actions[] = {
	[DISPATCH_WORKLOOP_ASYNC]                       = "async",
	[DISPATCH_WORKLOOP_ASYNC_FROM_SYNC]             = "async (from sync)",
	[DISPATCH_WORKLOOP_ASYNC_DISCOVER_SYNC]         = "discover sync",
	[DISPATCH_WORKLOOP_ASYNC_QOS_UPDATE]            = "qos update",
	[DISPATCH_WORKLOOP_ASYNC_LEAVE]                 = "leave",
	[DISPATCH_WORKLOOP_ASYNC_LEAVE_FROM_SYNC]       = "leave (from sync)",
	[DISPATCH_WORKLOOP_ASYNC_LEAVE_FROM_TRANSFER]   = "leave (from transfer)",
	[DISPATCH_WORKLOOP_ASYNC_FORCE_END_OWNERSHIP]   = "leave (forced)",
	[DISPATCH_WORKLOOP_RETARGET]                    = "retarget",

	[DISPATCH_WORKLOOP_SYNC_WAIT]                   = "sync-wait",
	[DISPATCH_WORKLOOP_SYNC_FAKE]                   = "sync-fake",
	[DISPATCH_WORKLOOP_SYNC_WAKE]                   = "sync-wake",
	[DISPATCH_WORKLOOP_SYNC_END]                    = "sync-end",
};

void
_dispatch_event_loop_atfork_child(void)
{
#if HAVE_MACH
	_dispatch_mach_host_port_pred = 0;
	_dispatch_mach_host_port = MACH_PORT_NULL;
#endif
}


DISPATCH_NOINLINE
void
_dispatch_event_loop_poke(dispatch_wlh_t wlh, uint64_t dq_state, uint32_t flags)
{
	if (wlh == DISPATCH_WLH_MANAGER) {
		dispatch_kevent_s ke = (dispatch_kevent_s){
			.ident  = 1,
			.filter = EVFILT_USER,
			.fflags = NOTE_TRIGGER,
			.udata = (uintptr_t)DISPATCH_WLH_MANAGER,
		};
		return _dispatch_kq_deferred_update(DISPATCH_WLH_ANON, &ke);
	} else if (wlh && wlh != DISPATCH_WLH_ANON) {
		(void)dq_state; (void)flags;
	}
	DISPATCH_INTERNAL_CRASH(wlh, "Unsupported wlh configuration");
}

DISPATCH_NOINLINE
void
_dispatch_event_loop_drain(uint32_t flags)
{
	dispatch_deferred_items_t ddi = _dispatch_deferred_items_get();
	dispatch_wlh_t wlh = ddi->ddi_wlh;
	int n;

again:
	n = ddi->ddi_nevents;
	ddi->ddi_nevents = 0;
	_dispatch_kq_drain(wlh, ddi->ddi_eventlist, n, flags);


	if ((flags & KEVENT_FLAG_IMMEDIATE) &&
			!(flags & KEVENT_FLAG_ERROR_EVENTS) &&
			_dispatch_needs_to_return_to_kernel()) {
		goto again;
	}
}

void
_dispatch_event_loop_merge(dispatch_kevent_t events, int nevents)
{
	dispatch_deferred_items_t ddi = _dispatch_deferred_items_get();
	dispatch_wlh_t wlh = ddi->ddi_wlh;
	dispatch_kevent_s kev[nevents];

	// now we can re-use the whole event list, but we need to save one slot
	// for the event loop poke
	memcpy(kev, events, sizeof(kev));
	ddi->ddi_maxevents = DISPATCH_DEFERRED_ITEMS_EVENT_COUNT - 2;

	for (int i = 0; i < nevents; i++) {
		_dispatch_kevent_drain(&kev[i]);
	}

	if (wlh == DISPATCH_WLH_ANON) {
		if (ddi->ddi_stashed_dou._do && ddi->ddi_nevents) {
			// We will drain the stashed item and not return to the kernel
			// right away. As a consequence, do not delay these updates.
			_dispatch_event_loop_drain(KEVENT_FLAG_IMMEDIATE |
					KEVENT_FLAG_ERROR_EVENTS);
		}
	}
}

void
_dispatch_event_loop_leave_immediate(uint64_t dq_state)
{
	(void)dq_state;
}

void
_dispatch_event_loop_leave_deferred(dispatch_deferred_items_t ddi,
		uint64_t dq_state)
{
	(void)ddi; (void)dq_state;
}

void
_dispatch_event_loop_cancel_waiter(dispatch_sync_context_t dsc)
{
	(void)dsc;
}

void
_dispatch_event_loop_wake_owner(dispatch_sync_context_t dsc,
		dispatch_wlh_t wlh, uint64_t old_state, uint64_t new_state)
{
	(void)dsc; (void)wlh; (void)old_state; (void)new_state;
}

void
_dispatch_event_loop_wait_for_ownership(dispatch_sync_context_t dsc)
{
	if (dsc->dsc_waiter_needs_cancel) {
		_dispatch_event_loop_cancel_waiter(dsc);
		dsc->dsc_waiter_needs_cancel = false;
	}
	if (dsc->dsc_release_storage) {
		_dispatch_queue_release_storage(dsc->dc_data);
	}
}

void
_dispatch_event_loop_end_ownership(dispatch_wlh_t wlh, uint64_t old_state,
		uint64_t new_state, uint32_t flags)
{
	(void)wlh; (void)old_state; (void)new_state; (void)flags;
}

#if DISPATCH_WLH_DEBUG
void
_dispatch_event_loop_assert_not_owned(dispatch_wlh_t wlh)
{
	(void)wlh;
}
#endif // DISPATCH_WLH_DEBUG

#pragma mark -
#pragma mark dispatch_event_loop timers

#define DISPATCH_KEVENT_TIMEOUT_IDENT_MASK (~0ull << 8)

DISPATCH_NOINLINE
static void
_dispatch_event_loop_timer_program(dispatch_timer_heap_t dth, uint32_t tidx,
		uint64_t target, uint64_t leeway, uint16_t action)
{
	dispatch_wlh_t wlh = _dispatch_get_wlh();
#if DISPATCH_USE_KEVENT_QOS
	pthread_priority_t pp = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;
	if (wlh != DISPATCH_WLH_ANON) {
		pp = _dispatch_qos_to_pp(dth[tidx].dth_max_qos);
	}
#endif
	dispatch_kevent_s ke = {
		.ident = DISPATCH_KEVENT_TIMEOUT_IDENT_MASK | tidx,
		.filter = EVFILT_TIMER,
		.flags = action | EV_ONESHOT,
		.fflags = _dispatch_timer_index_to_fflags[tidx],
		.data = (int64_t)target,
		.udata = (uintptr_t)dth,
#if DISPATCH_HAVE_TIMER_COALESCING
		.ext[1] = leeway,
#endif
#if DISPATCH_USE_KEVENT_QOS
		.qos = (typeof(ke.qos))pp,
#endif
	};

	_dispatch_kq_deferred_update(wlh, &ke);
}

void
_dispatch_event_loop_timer_arm(dispatch_timer_heap_t dth, uint32_t tidx,
		dispatch_timer_delay_s range, dispatch_clock_now_cache_t nows)
{
	dispatch_clock_t clock = DISPATCH_TIMER_CLOCK(tidx);
	uint64_t target = range.delay + _dispatch_time_now_cached(clock, nows);
	if (unlikely(_dispatch_timers_force_max_leeway)) {
		target += range.leeway;
		range.leeway = 0;
	}

	_dispatch_event_loop_timer_program(dth, tidx, target, range.leeway,
			EV_ADD | EV_ENABLE);
#if HAVE_MACH
	if (clock == DISPATCH_CLOCK_WALL) {
		_dispatch_mach_host_calendar_change_register();
	}
#endif
}

void
_dispatch_event_loop_timer_delete(dispatch_timer_heap_t dth, uint32_t tidx)
{
	_dispatch_event_loop_timer_program(dth, tidx, 0, 0, EV_DELETE);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_kevent_timer_drain(dispatch_kevent_t ke)
{
	dispatch_timer_heap_t dth = (dispatch_timer_heap_t)ke->udata;
	uint32_t tidx = ke->ident & ~DISPATCH_KEVENT_TIMEOUT_IDENT_MASK;

	dispatch_assert(ke->data > 0);
	dispatch_assert(ke->ident == (tidx | DISPATCH_KEVENT_TIMEOUT_IDENT_MASK));
	dispatch_assert(tidx < DISPATCH_TIMER_COUNT);

	_dispatch_timers_heap_dirty(dth, tidx);
	dth[tidx].dth_needs_program = true;
	dth[tidx].dth_armed = false;
}

#pragma mark -
#pragma mark kevent specific sources

static dispatch_unote_t
_dispatch_source_proc_create(dispatch_source_type_t dst DISPATCH_UNUSED,
		uintptr_t handle, unsigned long mask DISPATCH_UNUSED)
{
	dispatch_unote_t du = _dispatch_unote_create_with_handle(dst, handle, mask);
	if (du._du && (mask & DISPATCH_PROC_EXIT_STATUS)) {
		du._du->du_has_extended_status = true;
	}
	return du;
}

const dispatch_source_type_s _dispatch_source_type_proc = {
	.dst_kind       = "proc",
	.dst_filter     = EVFILT_PROC,
	.dst_flags      = DISPATCH_EV_DIRECT|EV_CLEAR,
	.dst_fflags     = NOTE_EXIT, // rdar://16655831
	.dst_mask       = NOTE_EXIT|NOTE_FORK|NOTE_EXEC|NOTE_EXITSTATUS
#if HAVE_DECL_NOTE_SIGNAL
			|NOTE_SIGNAL
#endif
#if HAVE_DECL_NOTE_REAP
			|NOTE_REAP
#endif
			,
	.dst_action     = DISPATCH_UNOTE_ACTION_SOURCE_OR_FFLAGS,
	.dst_size       = sizeof(struct dispatch_source_refs_s),
	.dst_strict     = false,

	.dst_create     = _dispatch_source_proc_create,
	.dst_merge_evt  = _dispatch_source_merge_evt,
};

const dispatch_source_type_s _dispatch_source_type_vnode = {
	.dst_kind       = "vnode",
	.dst_filter     = EVFILT_VNODE,
	.dst_flags      = DISPATCH_EV_DIRECT|EV_CLEAR|EV_VANISHED,
	.dst_mask       = NOTE_DELETE|NOTE_WRITE|NOTE_EXTEND|NOTE_ATTRIB|NOTE_LINK
			|NOTE_RENAME|NOTE_FUNLOCK
#if HAVE_DECL_NOTE_REVOKE
			|NOTE_REVOKE
#endif
#if HAVE_DECL_NOTE_NONE
			|NOTE_NONE
#endif
			,
	.dst_action     = DISPATCH_UNOTE_ACTION_SOURCE_OR_FFLAGS,
	.dst_size       = sizeof(struct dispatch_source_refs_s),
	.dst_strict     = false,

	.dst_create     = _dispatch_unote_create_with_fd,
	.dst_merge_evt  = _dispatch_source_merge_evt,
};

const dispatch_source_type_s _dispatch_source_type_vfs = {
	.dst_kind       = "vfs",
	.dst_filter     = EVFILT_FS,
	.dst_flags      = DISPATCH_EV_DIRECT|EV_CLEAR,
	.dst_mask       = VQ_NOTRESP|VQ_NEEDAUTH|VQ_LOWDISK|VQ_MOUNT|VQ_UNMOUNT
			|VQ_DEAD|VQ_ASSIST|VQ_NOTRESPLOCK
#if HAVE_DECL_VQ_UPDATE
			|VQ_UPDATE
#endif
#if HAVE_DECL_VQ_VERYLOWDISK
			|VQ_VERYLOWDISK
#endif
#if HAVE_DECL_VQ_QUOTA
			|VQ_QUOTA
#endif
#if HAVE_DECL_VQ_NEARLOWDISK
			|VQ_NEARLOWDISK
#endif
#if HAVE_DECL_VQ_DESIRED_DISK
			|VQ_DESIRED_DISK
#endif
			,
	.dst_action     = DISPATCH_UNOTE_ACTION_SOURCE_OR_FFLAGS,
	.dst_size       = sizeof(struct dispatch_source_refs_s),
	.dst_strict     = false,

	.dst_create     = _dispatch_unote_create_without_handle,
	.dst_merge_evt  = _dispatch_source_merge_evt,
};

#ifdef EVFILT_SOCK
const dispatch_source_type_s _dispatch_source_type_sock = {
	.dst_kind       = "sock",
	.dst_filter     = EVFILT_SOCK,
	.dst_flags      = DISPATCH_EV_DIRECT|EV_CLEAR|EV_VANISHED,
	.dst_mask       = NOTE_CONNRESET|NOTE_READCLOSED|NOTE_WRITECLOSED
			|NOTE_TIMEOUT|NOTE_NOSRCADDR|NOTE_IFDENIED|NOTE_SUSPEND|NOTE_RESUME
			|NOTE_KEEPALIVE
#ifdef NOTE_ADAPTIVE_WTIMO
			|NOTE_ADAPTIVE_WTIMO|NOTE_ADAPTIVE_RTIMO
#endif
#ifdef NOTE_CONNECTED
			|NOTE_CONNECTED|NOTE_DISCONNECTED|NOTE_CONNINFO_UPDATED
#endif
#ifdef NOTE_NOTIFY_ACK
			|NOTE_NOTIFY_ACK
#endif
		,
	.dst_action     = DISPATCH_UNOTE_ACTION_SOURCE_OR_FFLAGS,
	.dst_size       = sizeof(struct dispatch_source_refs_s),
	.dst_strict     = false,

	.dst_create     = _dispatch_unote_create_with_fd,
	.dst_merge_evt  = _dispatch_source_merge_evt,
};
#endif // EVFILT_SOCK

#ifdef EVFILT_NW_CHANNEL
const dispatch_source_type_s _dispatch_source_type_nw_channel = {
	.dst_kind       = "nw_channel",
	.dst_filter     = EVFILT_NW_CHANNEL,
	.dst_flags      = DISPATCH_EV_DIRECT|EV_CLEAR|EV_VANISHED,
	.dst_mask       = NOTE_FLOW_ADV_UPDATE,
	.dst_action     = DISPATCH_UNOTE_ACTION_SOURCE_OR_FFLAGS,
	.dst_size       = sizeof(struct dispatch_source_refs_s),
	.dst_strict     = false,

	.dst_create     = _dispatch_unote_create_with_fd,
	.dst_merge_evt  = _dispatch_source_merge_evt,
};
#endif // EVFILT_NW_CHANNEL

#if DISPATCH_USE_MEMORYSTATUS

#if DISPATCH_USE_MEMORYPRESSURE_SOURCE
#define DISPATCH_MEMORYPRESSURE_SOURCE_MASK ( \
		DISPATCH_MEMORYPRESSURE_NORMAL | \
		DISPATCH_MEMORYPRESSURE_WARN | \
		DISPATCH_MEMORYPRESSURE_CRITICAL | \
		DISPATCH_MEMORYPRESSURE_PROC_LIMIT_WARN | \
		DISPATCH_MEMORYPRESSURE_PROC_LIMIT_CRITICAL | \
		DISPATCH_MEMORYPRESSURE_MSL_STATUS)

#define DISPATCH_MEMORYPRESSURE_MALLOC_MASK ( \
		DISPATCH_MEMORYPRESSURE_WARN | \
		DISPATCH_MEMORYPRESSURE_CRITICAL | \
		DISPATCH_MEMORYPRESSURE_PROC_LIMIT_WARN | \
		DISPATCH_MEMORYPRESSURE_PROC_LIMIT_CRITICAL | \
		DISPATCH_MEMORYPRESSURE_MSL_STATUS)


static void
_dispatch_memorypressure_handler(void *context)
{
	dispatch_source_t ds = context;
	unsigned long memorypressure = dispatch_source_get_data(ds);

	if (memorypressure & DISPATCH_MEMORYPRESSURE_NORMAL) {
		_dispatch_memory_warn = false;
		_dispatch_continuation_cache_limit = DISPATCH_CONTINUATION_CACHE_LIMIT;
#if VOUCHER_USE_MACH_VOUCHER
		if (_firehose_task_buffer) {
			firehose_buffer_clear_bank_flags(_firehose_task_buffer,
					FIREHOSE_BUFFER_BANK_FLAG_LOW_MEMORY);
		}
#endif
	}
	if (memorypressure & DISPATCH_MEMORYPRESSURE_WARN) {
		_dispatch_memory_warn = true;
		_dispatch_continuation_cache_limit =
				DISPATCH_CONTINUATION_CACHE_LIMIT_MEMORYPRESSURE_PRESSURE_WARN;
#if VOUCHER_USE_MACH_VOUCHER
		if (_firehose_task_buffer) {
			firehose_buffer_set_bank_flags(_firehose_task_buffer,
					FIREHOSE_BUFFER_BANK_FLAG_LOW_MEMORY);
		}
#endif
	}
	memorypressure &= DISPATCH_MEMORYPRESSURE_MALLOC_MASK;
	if (memorypressure) {
		malloc_memory_event_handler(memorypressure);
	}
}

static void
_dispatch_memorypressure_init(void)
{
	dispatch_source_t ds = dispatch_source_create(
			DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
			DISPATCH_MEMORYPRESSURE_SOURCE_MASK, _dispatch_mgr_q._as_dq);
	dispatch_set_context(ds, ds);
	dispatch_source_set_event_handler_f(ds, _dispatch_memorypressure_handler);
	dispatch_activate(ds);
}
#endif // DISPATCH_USE_MEMORYPRESSURE_SOURCE

#if TARGET_OS_SIMULATOR // rdar://problem/9219483
static int _dispatch_ios_simulator_memory_warnings_fd = -1;
static void
_dispatch_ios_simulator_memorypressure_init(void *context DISPATCH_UNUSED)
{
	char *e = getenv("SIMULATOR_MEMORY_WARNINGS");
	if (!e) return;
	_dispatch_ios_simulator_memory_warnings_fd = open(e, O_EVTONLY);
	if (_dispatch_ios_simulator_memory_warnings_fd == -1) {
		(void)dispatch_assume_zero(errno);
	}
}

static dispatch_unote_t
_dispatch_source_memorypressure_create(dispatch_source_type_t dst,
	uintptr_t handle, unsigned long mask)
{
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_ios_simulator_memorypressure_init);

	if (handle) {
		return DISPATCH_UNOTE_NULL;
	}

	dst = &_dispatch_source_type_vnode;
	handle = (uintptr_t)_dispatch_ios_simulator_memory_warnings_fd;
	mask = NOTE_ATTRIB;

	dispatch_unote_t du = dux_create(dst, handle, mask);
	if (du._du) {
		du._du->du_memorypressure_override = true;
	}
	return du;
}
#endif // TARGET_OS_SIMULATOR

const dispatch_source_type_s _dispatch_source_type_memorypressure = {
	.dst_kind       = "memorystatus",
	.dst_filter     = EVFILT_MEMORYSTATUS,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_DISPATCH,
	.dst_mask       = NOTE_MEMORYSTATUS_PRESSURE_NORMAL
			|NOTE_MEMORYSTATUS_PRESSURE_WARN|NOTE_MEMORYSTATUS_PRESSURE_CRITICAL
			|NOTE_MEMORYSTATUS_LOW_SWAP|NOTE_MEMORYSTATUS_PROC_LIMIT_WARN
			|NOTE_MEMORYSTATUS_PROC_LIMIT_CRITICAL
			|NOTE_MEMORYSTATUS_MSL_STATUS,
	.dst_action     = DISPATCH_UNOTE_ACTION_SOURCE_OR_FFLAGS,
	.dst_size       = sizeof(struct dispatch_source_refs_s),
	.dst_strict     = false,

#if TARGET_OS_SIMULATOR
	.dst_create     = _dispatch_source_memorypressure_create,
	// redirected to _dispatch_source_type_vnode
#else
	.dst_create     = _dispatch_unote_create_without_handle,
	.dst_merge_evt  = _dispatch_source_merge_evt,
#endif
};

static dispatch_unote_t
_dispatch_source_vm_create(dispatch_source_type_t dst DISPATCH_UNUSED,
		uintptr_t handle, unsigned long mask DISPATCH_UNUSED)
{
	// Map legacy vm pressure to memorypressure warning rdar://problem/15907505
	dispatch_unote_t du = dux_create(&_dispatch_source_type_memorypressure,
			handle, NOTE_MEMORYSTATUS_PRESSURE_WARN);
	if (du._du) {
		du._du->du_vmpressure_override = 1;
	}
	return du;
}

const dispatch_source_type_s _dispatch_source_type_vm = {
	.dst_kind       = "vm (deprecated)",
	.dst_filter     = EVFILT_MEMORYSTATUS,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_DISPATCH,
	.dst_mask       = NOTE_VM_PRESSURE,
	.dst_action     = DISPATCH_UNOTE_ACTION_SOURCE_OR_FFLAGS,
	.dst_size       = sizeof(struct dispatch_source_refs_s),
	.dst_strict     = false,

	.dst_create     = _dispatch_source_vm_create,
	// redirected to _dispatch_source_type_memorypressure
};
#endif // DISPATCH_USE_MEMORYSTATUS

#pragma mark mach send / notifications
#if HAVE_MACH

// Flags for all notifications that are registered/unregistered when a
// send-possible notification is requested/delivered
#define _DISPATCH_MACH_SP_FLAGS (DISPATCH_MACH_SEND_POSSIBLE| \
		DISPATCH_MACH_SEND_DEAD|DISPATCH_MACH_SEND_DELETED)

static void _dispatch_mach_host_notify_update(void *context);

DISPATCH_STATIC_GLOBAL(dispatch_once_t _dispatch_mach_notify_port_pred);
DISPATCH_STATIC_GLOBAL(dispatch_once_t _dispatch_mach_calendar_pred);
DISPATCH_STATIC_GLOBAL(mach_port_t _dispatch_mach_notify_port);

static void
_dispatch_timers_calendar_change(void)
{
	dispatch_timer_heap_t dth = _dispatch_timers_heap;
	uint32_t qos, tidx;

	// calendar change may have gone past the wallclock deadline
	for (qos = 0; qos < DISPATCH_TIMER_QOS_COUNT; qos++) {
		tidx = DISPATCH_TIMER_INDEX(DISPATCH_CLOCK_WALL, qos);
		_dispatch_timers_heap_dirty(dth, tidx);
		dth[tidx].dth_needs_program = true;
	}
}

static mach_msg_audit_trailer_t *
_dispatch_mach_msg_get_audit_trailer(mach_msg_header_t *hdr)
{
	mach_msg_trailer_t *tlr = NULL;
	mach_msg_audit_trailer_t *audit_tlr = NULL;
	tlr = (mach_msg_trailer_t *)((unsigned char *)hdr +
			round_msg(hdr->msgh_size));
	// The trailer should always be of format zero.
	if (tlr->msgh_trailer_type == MACH_MSG_TRAILER_FORMAT_0) {
		if (tlr->msgh_trailer_size >= sizeof(mach_msg_audit_trailer_t)) {
			audit_tlr = (mach_msg_audit_trailer_t *)tlr;
		}
	}
	return audit_tlr;
}

DISPATCH_NOINLINE
static void
_dispatch_mach_notification_merge_msg(dispatch_unote_t du, uint32_t flags,
		mach_msg_header_t *hdr, mach_msg_size_t msgsz DISPATCH_UNUSED,
		pthread_priority_t msg_pp DISPATCH_UNUSED,
		pthread_priority_t ovr_pp DISPATCH_UNUSED)
{
	mig_reply_error_t reply;
	mach_msg_audit_trailer_t *tlr = NULL;
	dispatch_assert(sizeof(mig_reply_error_t) == sizeof(union
		__ReplyUnion___dispatch_libdispatch_internal_protocol_subsystem));
	dispatch_assert(sizeof(mig_reply_error_t) <
			DISPATCH_MACH_RECEIVE_MAX_INLINE_MESSAGE_SIZE);
	tlr = _dispatch_mach_msg_get_audit_trailer(hdr);
	if (!tlr) {
		DISPATCH_INTERNAL_CRASH(0, "message received without expected trailer");
	}
	if (hdr->msgh_id <= MACH_NOTIFY_LAST &&
			dispatch_assume_zero(tlr->msgh_audit.val[
			DISPATCH_MACH_AUDIT_TOKEN_PID])) {
		mach_msg_destroy(hdr);
		goto out;
	}

	boolean_t success = libdispatch_internal_protocol_server(hdr, &reply.Head);
	if (!success && reply.RetCode == MIG_BAD_ID &&
			(hdr->msgh_id == HOST_CALENDAR_SET_REPLYID ||
			hdr->msgh_id == HOST_CALENDAR_CHANGED_REPLYID)) {
		_dispatch_debug("calendar-change notification");
		_dispatch_timers_calendar_change();
		_dispatch_mach_host_notify_update(NULL);
		success = TRUE;
		reply.RetCode = KERN_SUCCESS;
	}
	if (dispatch_assume(success) && reply.RetCode != MIG_NO_REPLY) {
		(void)dispatch_assume_zero(reply.RetCode);
	}
	if (!success || (reply.RetCode && reply.RetCode != MIG_NO_REPLY)) {
		mach_msg_destroy(hdr);
	}

out:
	if (flags & DISPATCH_EV_MSG_NEEDS_FREE) {
		free(hdr);
	}
	return _dispatch_unote_resume(du);
}

DISPATCH_NOINLINE
static void
_dispatch_mach_notify_port_init(void *context DISPATCH_UNUSED)
{
	mach_port_options_t opts = { .flags = MPO_CONTEXT_AS_GUARD | MPO_STRICT };
	mach_port_context_t guard = (uintptr_t)&_dispatch_mach_notify_port;
	kern_return_t kr;

	kr = mach_port_construct(mach_task_self(), &opts, guard,
			&_dispatch_mach_notify_port);
	if (unlikely(kr)) {
		DISPATCH_CLIENT_CRASH(kr,
				"mach_port_construct() failed: cannot create receive right");
	}

	dispatch_unote_t du = dux_create(&_dispatch_mach_type_notification,
			_dispatch_mach_notify_port, 0);

	// make sure _dispatch_kevent_mach_msg_recv can call
	// _dispatch_retain_unote_owner
	du._du->du_owner_wref = _dispatch_ptr2wref(&_dispatch_mgr_q);

	dispatch_assume(_dispatch_unote_register(du, DISPATCH_WLH_ANON,
			DISPATCH_PRIORITY_FLAG_MANAGER));
}

static void
_dispatch_mach_host_port_init(void *ctxt DISPATCH_UNUSED)
{
	kern_return_t kr;
	mach_port_t mp, mhp = mach_host_self();
	kr = host_get_host_port(mhp, &mp);
	DISPATCH_VERIFY_MIG(kr);
	if (likely(!kr)) {
		// mach_host_self returned the HOST_PRIV port
		kr = mach_port_deallocate(mach_task_self(), mhp);
		DISPATCH_VERIFY_MIG(kr);
		mhp = mp;
	} else if (kr != KERN_INVALID_ARGUMENT) {
		(void)dispatch_assume_zero(kr);
	}
	if (unlikely(!mhp)) {
		DISPATCH_CLIENT_CRASH(kr, "Could not get unprivileged host port");
	}
	_dispatch_mach_host_port = mhp;
}

mach_port_t
_dispatch_get_mach_host_port(void)
{
	dispatch_once_f(&_dispatch_mach_host_port_pred, NULL,
			_dispatch_mach_host_port_init);
	return _dispatch_mach_host_port;
}

DISPATCH_ALWAYS_INLINE
static inline mach_port_t
_dispatch_get_mach_notify_port(void)
{
	dispatch_once_f(&_dispatch_mach_notify_port_pred, NULL,
			_dispatch_mach_notify_port_init);
	return _dispatch_mach_notify_port;
}

static void
_dispatch_mach_host_notify_update(void *context DISPATCH_UNUSED)
{
	kern_return_t kr;
	_dispatch_debug("registering for calendar-change notification");

	kr = host_request_notification(_dispatch_get_mach_host_port(),
			HOST_NOTIFY_CALENDAR_SET, _dispatch_get_mach_notify_port());
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_mach_host_calendar_change_register(void)
{
	dispatch_once_f(&_dispatch_mach_calendar_pred, NULL,
			_dispatch_mach_host_notify_update);
}

static kern_return_t
_dispatch_mach_notify_update(dispatch_muxnote_t dmn, uint32_t new_flags,
		uint32_t del_flags, uint32_t mask, mach_msg_id_t notify_msgid,
		mach_port_mscount_t notify_sync)
{
	mach_port_t previous, port = (mach_port_t)dmn->dmn_kev.ident;
	typeof(dmn->dmn_kev.data) prev = dmn->dmn_kev.data;
	kern_return_t kr, krr = 0;

	// Update notification registration state.
	dmn->dmn_kev.data |= (new_flags | dmn->dmn_kev.fflags) & mask;
	dmn->dmn_kev.data &= ~(del_flags & mask);

	_dispatch_debug_machport(port);
	if ((dmn->dmn_kev.data & mask) && !(prev & mask)) {
		_dispatch_debug("machport[0x%08x]: registering for send-possible "
				"notification", port);
		previous = MACH_PORT_NULL;
		krr = mach_port_request_notification(mach_task_self(), port,
				notify_msgid, notify_sync, _dispatch_get_mach_notify_port(),
				MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
		DISPATCH_VERIFY_MIG(krr);

		switch (krr) {
		case KERN_INVALID_NAME:
		case KERN_INVALID_RIGHT:
			// Suppress errors & clear registration state
			dmn->dmn_kev.data &= ~mask;
			break;
		default:
			// Else, we don't expect any errors from mach. Log any errors
			if (dispatch_assume_zero(krr)) {
				// log the error & clear registration state
				dmn->dmn_kev.data &= ~mask;
			} else if (dispatch_assume_zero(previous)) {
				// Another subsystem has beat libdispatch to requesting the
				// specified Mach notification on this port. We should
				// technically cache the previous port and message it when the
				// kernel messages our port. Or we can just say screw those
				// subsystems and deallocate the previous port.
				// They should adopt libdispatch :-P
				kr = mach_port_deallocate(mach_task_self(), previous);
				DISPATCH_VERIFY_MIG(kr);
				(void)dispatch_assume_zero(kr);
				previous = MACH_PORT_NULL;
			}
		}
	} else if (!(dmn->dmn_kev.data & mask) && (prev & mask)) {
		_dispatch_debug("machport[0x%08x]: unregistering for send-possible "
				"notification", port);
		previous = MACH_PORT_NULL;
		kr = mach_port_request_notification(mach_task_self(), port,
				notify_msgid, notify_sync, MACH_PORT_NULL,
				MACH_MSG_TYPE_MOVE_SEND_ONCE, &previous);
		DISPATCH_VERIFY_MIG(kr);

		switch (kr) {
		case KERN_INVALID_NAME:
		case KERN_INVALID_RIGHT:
		case KERN_INVALID_ARGUMENT:
			break;
		default:
			if (dispatch_assume_zero(kr)) {
				// log the error
			}
		}
	} else {
		return 0;
	}
	if (unlikely(previous)) {
		// the kernel has not consumed the send-once right yet
		(void)dispatch_assume_zero(
				_dispatch_send_consume_send_once_right(previous));
	}
	return krr;
}

static bool
_dispatch_kevent_mach_notify_resume(dispatch_muxnote_t dmn, uint32_t new_flags,
		uint32_t del_flags)
{
	kern_return_t kr = KERN_SUCCESS;
	dispatch_assert_zero(new_flags & del_flags);
	if ((new_flags & _DISPATCH_MACH_SP_FLAGS) ||
			(del_flags & _DISPATCH_MACH_SP_FLAGS)) {
		// Requesting a (delayed) non-sync send-possible notification
		// registers for both immediate dead-name notification and delayed-arm
		// send-possible notification for the port.
		// The send-possible notification is armed when a mach_msg() with the
		// the MACH_SEND_NOTIFY to the port times out.
		// If send-possible is unavailable, fall back to immediate dead-name
		// registration rdar://problem/2527840&9008724
		kr = _dispatch_mach_notify_update(dmn, new_flags, del_flags,
				_DISPATCH_MACH_SP_FLAGS, MACH_NOTIFY_SEND_POSSIBLE,
				MACH_NOTIFY_SEND_POSSIBLE == MACH_NOTIFY_DEAD_NAME);
	}
	return kr == KERN_SUCCESS;
}

DISPATCH_NOINLINE
static void
_dispatch_mach_notify_merge(mach_port_t name, uint32_t data, bool final)
{
	dispatch_unote_linkage_t dul, dul_next;
	dispatch_muxnote_t dmn;
	uint32_t flags = EV_ENABLE;

	_dispatch_debug_machport(name);
	dmn = _dispatch_mach_muxnote_find(name, DISPATCH_EVFILT_MACH_NOTIFICATION);
	if (!dmn) {
		return;
	}

	dmn->dmn_kev.data &= ~_DISPATCH_MACH_SP_FLAGS;
	if (final || !_dispatch_kevent_mach_notify_resume(dmn, data, 0)) {
		flags = EV_ONESHOT;
		dmn->dmn_kev.flags |= EV_DELETE;
	}
	os_atomic_store(&DISPATCH_MACH_NOTIFICATION_ARMED(dmn), 0, relaxed);

	LIST_FOREACH_SAFE(dul, &dmn->dmn_unotes_head, du_link, dul_next) {
		if (os_atomic_load(&DISPATCH_MACH_NOTIFICATION_ARMED(dmn), relaxed)) {
			dispatch_assert(!final);
			break;
		}
		dispatch_unote_t du = _dispatch_unote_linkage_get_unote(dul);
		uint32_t fflags = (data & du._du->du_fflags);
		os_atomic_store2o(du._du, dmsr_notification_armed, 0, relaxed);
		if (final || fflags) {
			// consumed by dux_merge_evt()
			_dispatch_retain_unote_owner(du);
			if (final) _dispatch_unote_unregister_muxed(du);
			if (fflags && dux_type(du._du)->dst_action ==
					DISPATCH_UNOTE_ACTION_SOURCE_OR_FFLAGS) {
				os_atomic_or2o(du._dr, ds_pending_data, fflags, relaxed);
			}
			dux_merge_evt(du._du, flags, fflags, 0);
		}
	}
}

kern_return_t
_dispatch_mach_notify_port_deleted(mach_port_t notify DISPATCH_UNUSED,
		mach_port_name_t name)
{
#if DISPATCH_DEBUG
	_dispatch_log("Corruption: Mach send/send-once/dead-name right 0x%x "
			"deleted prematurely", name);
#endif
	_dispatch_debug_machport(name);
	_dispatch_mach_notify_merge(name, DISPATCH_MACH_SEND_DELETED, true);
	return KERN_SUCCESS;
}

kern_return_t
_dispatch_mach_notify_dead_name(mach_port_t notify DISPATCH_UNUSED,
		mach_port_name_t name)
{
	kern_return_t kr;

	_dispatch_debug("machport[0x%08x]: dead-name notification", name);
	_dispatch_debug_machport(name);
	_dispatch_mach_notify_merge(name, DISPATCH_MACH_SEND_DEAD, true);

	// the act of receiving a dead name notification allocates a dead-name
	// right that must be deallocated
	kr = mach_port_deallocate(mach_task_self(), name);
	DISPATCH_VERIFY_MIG(kr);
	//(void)dispatch_assume_zero(kr);
	return KERN_SUCCESS;
}

kern_return_t
_dispatch_mach_notify_send_possible(mach_port_t notify DISPATCH_UNUSED,
		mach_port_name_t name)
{
	_dispatch_debug("machport[0x%08x]: send-possible notification", name);
	_dispatch_debug_machport(name);
	_dispatch_mach_notify_merge(name, DISPATCH_MACH_SEND_POSSIBLE, false);
	return KERN_SUCCESS;
}

void
_dispatch_mach_notification_set_armed(dispatch_mach_send_refs_t dmsr)
{
	dispatch_muxnote_t dmn = _dispatch_unote_get_linkage(dmsr)->du_muxnote;
	dispatch_unote_linkage_t dul;
	if (dmn) {
		os_atomic_store(&DISPATCH_MACH_NOTIFICATION_ARMED(dmn), 1, relaxed);
		LIST_FOREACH(dul, &dmn->dmn_unotes_head, du_link) {
			dispatch_unote_t du = _dispatch_unote_linkage_get_unote(dul);
			os_atomic_store2o(du._du, dmsr_notification_armed, 1, relaxed);
		}
		_dispatch_debug("machport[0x%08x]: send-possible notification armed",
				(mach_port_name_t)dmn->dmn_kev.ident);
	}
}

static dispatch_unote_t
_dispatch_source_mach_send_create(dispatch_source_type_t dst,
		uintptr_t handle, unsigned long mask)
{
	if (!mask) {
		// Preserve legacy behavior that (mask == 0) => DISPATCH_MACH_SEND_DEAD
		mask = DISPATCH_MACH_SEND_DEAD;
	}
	if (!handle) {
		handle = MACH_PORT_DEAD; // <rdar://problem/27651332>
	}
	return _dispatch_unote_create_with_handle(dst, handle, mask);
}

static bool
_dispatch_mach_send_update(dispatch_muxnote_t dmn)
{
	if (dmn->dmn_kev.flags & EV_DELETE) {
		return _dispatch_kevent_mach_notify_resume(dmn, 0, dmn->dmn_kev.fflags);
	} else {
		return _dispatch_kevent_mach_notify_resume(dmn, dmn->dmn_kev.fflags, 0);
	}
}

const dispatch_source_type_s _dispatch_source_type_mach_send = {
	.dst_kind       = "mach_send",
	.dst_filter     = DISPATCH_EVFILT_MACH_NOTIFICATION,
	.dst_flags      = EV_CLEAR,
	.dst_mask       = DISPATCH_MACH_SEND_DEAD|DISPATCH_MACH_SEND_POSSIBLE,
	.dst_action     = DISPATCH_UNOTE_ACTION_SOURCE_OR_FFLAGS,
	.dst_size       = sizeof(struct dispatch_source_refs_s),
	.dst_strict     = false,

	.dst_create     = _dispatch_source_mach_send_create,
	.dst_update_mux = _dispatch_mach_send_update,
	.dst_merge_evt  = _dispatch_source_merge_evt,
};

static dispatch_unote_t
_dispatch_mach_send_create(dispatch_source_type_t dst,
	uintptr_t handle, unsigned long mask)
{
	// without handle because the mach code will set the ident later
	dispatch_unote_t du =
			_dispatch_unote_create_without_handle(dst, handle, mask);
	if (du._dmsr) {
		du._dmsr->dmsr_disconnect_cnt = DISPATCH_MACH_NEVER_CONNECTED;
		LIST_INIT(&du._dmsr->dmsr_replies);
	}
	return du;
}

const dispatch_source_type_s _dispatch_mach_type_send = {
	.dst_kind       = "mach_send (mach)",
	.dst_filter     = DISPATCH_EVFILT_MACH_NOTIFICATION,
	.dst_flags      = EV_CLEAR,
	.dst_mask       = DISPATCH_MACH_SEND_DEAD|DISPATCH_MACH_SEND_POSSIBLE,
	.dst_action     = DISPATCH_UNOTE_ACTION_PASS_FFLAGS,
	.dst_size       = sizeof(struct dispatch_mach_send_refs_s),
	.dst_strict     = false,

	.dst_create     = _dispatch_mach_send_create,
	.dst_update_mux = _dispatch_mach_send_update,
	.dst_merge_evt  = _dispatch_mach_notification_merge_evt,
};

#endif // HAVE_MACH
#pragma mark mach recv / reply
#if HAVE_MACH

static void
_dispatch_kevent_mach_msg_recv(dispatch_unote_t du, uint32_t flags,
		mach_msg_header_t *hdr, pthread_priority_t msg_pp,
		pthread_priority_t ovr_pp)
{
	mach_port_t name = hdr->msgh_local_port;
	mach_msg_size_t siz;

	if (os_add_overflow(hdr->msgh_size, DISPATCH_MACH_TRAILER_SIZE, &siz)) {
		DISPATCH_CLIENT_CRASH(hdr->msgh_size, "Overlarge message received");
	}
	if (os_unlikely(name == MACH_PORT_NULL)) {
		DISPATCH_CLIENT_CRASH(hdr->msgh_id, "Received message with "
				"MACH_PORT_NULL msgh_local_port");
	}

	_dispatch_debug_machport(name);
	// consumed by dux_merge_evt()
	_dispatch_retain_unote_owner(du);
	_dispatch_kevent_merge_ev_flags(du, flags);
	return dux_merge_msg(du._du, flags, hdr, siz, msg_pp, ovr_pp);
}

DISPATCH_NOINLINE
static void
_dispatch_kevent_mach_msg_drain(dispatch_kevent_t ke)
{
	mach_msg_header_t *hdr = _dispatch_kevent_mach_msg_buf(ke);
	dispatch_unote_t du = _dispatch_kevent_get_unote(ke);
	pthread_priority_t msg_pp = (pthread_priority_t)(ke->ext[2] >> 32);
	pthread_priority_t ovr_pp = (pthread_priority_t)ke->qos;
	uint32_t flags = ke->flags;
	mach_msg_size_t siz;
	mach_msg_return_t kr = (mach_msg_return_t)ke->fflags;

	if (unlikely(!hdr)) {
		DISPATCH_INTERNAL_CRASH(kr, "EVFILT_MACHPORT with no message");
	}
	if (likely(!kr)) {
		return _dispatch_kevent_mach_msg_recv(du, flags, hdr, msg_pp, ovr_pp);
	}
	if (kr != MACH_RCV_TOO_LARGE) {
		goto out;
	}

	if (!ke->data) {
		DISPATCH_INTERNAL_CRASH(0, "MACH_RCV_LARGE_IDENTITY with no identity");
	}
	if (unlikely(ke->ext[1] > (UINT_MAX - DISPATCH_MACH_TRAILER_SIZE))) {
		DISPATCH_INTERNAL_CRASH(ke->ext[1],
				"EVFILT_MACHPORT with overlarge message");
	}
	const mach_msg_option_t options = ((DISPATCH_MACH_RCV_OPTIONS |
			MACH_RCV_TIMEOUT) & ~MACH_RCV_LARGE);
	siz = _dispatch_kevent_mach_msg_size(ke) + DISPATCH_MACH_TRAILER_SIZE;
	hdr = malloc(siz); // mach_msg will return TOO_LARGE if hdr/siz is NULL/0
	kr = mach_msg(hdr, options, 0, dispatch_assume(hdr) ? siz : 0,
			(mach_port_name_t)ke->data, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (likely(!kr)) {
		flags |= DISPATCH_EV_MSG_NEEDS_FREE;
		return _dispatch_kevent_mach_msg_recv(du, flags, hdr, msg_pp, ovr_pp);
	}

	if (kr == MACH_RCV_TOO_LARGE) {
		_dispatch_log("BUG in libdispatch client: "
				"_dispatch_kevent_mach_msg_drain: dropped message too "
				"large to fit in memory: id = 0x%x, size = %u",
				hdr->msgh_id, _dispatch_kevent_mach_msg_size(ke));
		kr = MACH_MSG_SUCCESS;
	}
	free(hdr);

out:
	if (unlikely(kr)) {
		_dispatch_bug_mach_client("_dispatch_kevent_mach_msg_drain: "
				"message reception failed", kr);
	}
}

const dispatch_source_type_s _dispatch_source_type_mach_recv = {
	.dst_kind       = "mach_recv",
	.dst_filter     = EVFILT_MACHPORT,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_DISPATCH|EV_VANISHED,
	.dst_fflags     = 0,
	.dst_action     = DISPATCH_UNOTE_ACTION_SOURCE_OR_FFLAGS,
	.dst_size       = sizeof(struct dispatch_source_refs_s),
	.dst_strict     = false,

	.dst_create     = _dispatch_unote_create_with_handle,
	.dst_merge_evt  = _dispatch_source_merge_evt,
	.dst_merge_msg  = NULL, // never receives messages directly

	.dst_per_trigger_qos = true,
};

static void
_dispatch_mach_notification_event(dispatch_unote_t du, uint32_t flags DISPATCH_UNUSED,
		uintptr_t data DISPATCH_UNUSED, pthread_priority_t pp DISPATCH_UNUSED)
{
	DISPATCH_CLIENT_CRASH(du._du->du_ident, "Unexpected non message event");
}

const dispatch_source_type_s _dispatch_mach_type_notification = {
	.dst_kind       = "mach_notification",
	.dst_filter     = EVFILT_MACHPORT,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_DISPATCH|EV_VANISHED,
	.dst_fflags     = DISPATCH_MACH_RCV_OPTIONS,
	.dst_action     = DISPATCH_UNOTE_ACTION_PASS_FFLAGS,
	.dst_size       = sizeof(struct dispatch_unote_class_s),
	.dst_strict     = false,

	.dst_create     = _dispatch_unote_create_with_handle,
	.dst_merge_evt  = _dispatch_mach_notification_event,
	.dst_merge_msg  = _dispatch_mach_notification_merge_msg,

	.dst_per_trigger_qos = true,
};

static void
_dispatch_mach_recv_direct_merge_evt(dispatch_unote_t du, uint32_t flags,
		uintptr_t data, pthread_priority_t pp)
{
	if (flags & EV_VANISHED) {
		DISPATCH_CLIENT_CRASH(du._du->du_ident,
				"Unexpected EV_VANISHED (do not destroy random mach ports)");
	}
	return _dispatch_source_merge_evt(du, flags, data, pp);
}

const dispatch_source_type_s _dispatch_mach_type_recv = {
	.dst_kind       = "mach_recv (channel)",
	.dst_filter     = EVFILT_MACHPORT,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_DISPATCH|EV_VANISHED,
	.dst_fflags     = DISPATCH_MACH_RCV_OPTIONS,
	.dst_action     = DISPATCH_UNOTE_ACTION_PASS_FFLAGS,
	.dst_size       = sizeof(struct dispatch_mach_recv_refs_s),
	.dst_strict     = false,

	// without handle because the mach code will set the ident after connect
	.dst_create     = _dispatch_unote_create_without_handle,
	.dst_merge_evt  = _dispatch_mach_recv_direct_merge_evt,
	.dst_merge_msg  = _dispatch_mach_merge_msg,

	.dst_per_trigger_qos = true,
};

DISPATCH_NORETURN
static void
_dispatch_mach_reply_merge_evt(dispatch_unote_t du,
		uint32_t flags DISPATCH_UNUSED, uintptr_t data DISPATCH_UNUSED,
		pthread_priority_t pp DISPATCH_UNUSED)
{
	DISPATCH_INTERNAL_CRASH(du._du->du_ident, "Unexpected event");
}

const dispatch_source_type_s _dispatch_mach_type_reply = {
	.dst_kind       = "mach reply",
	.dst_filter     = EVFILT_MACHPORT,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_DISPATCH|EV_ONESHOT|EV_VANISHED,
	.dst_fflags     = DISPATCH_MACH_RCV_OPTIONS,
	.dst_action     = DISPATCH_UNOTE_ACTION_PASS_FFLAGS,
	.dst_size       = sizeof(struct dispatch_mach_reply_refs_s),
	.dst_strict     = false,

	.dst_create     = _dispatch_unote_create_with_handle,
	.dst_merge_evt  = _dispatch_mach_reply_merge_evt,
	.dst_merge_msg  = _dispatch_mach_reply_merge_msg,
};

#pragma mark Mach channel SIGTERM notification (for XPC channels only)

const dispatch_source_type_s _dispatch_xpc_type_sigterm = {
	.dst_kind       = "sigterm (xpc)",
	.dst_filter     = EVFILT_SIGNAL,
	.dst_flags      = DISPATCH_EV_DIRECT|EV_CLEAR|EV_ONESHOT,
	.dst_fflags     = 0,
	.dst_action     = DISPATCH_UNOTE_ACTION_PASS_DATA,
	.dst_size       = sizeof(struct dispatch_xpc_term_refs_s),
	.dst_strict     = false,

	.dst_create     = _dispatch_unote_create_with_handle,
	.dst_merge_evt  = _dispatch_xpc_sigterm_merge_evt,
};

#endif // HAVE_MACH

#endif // DISPATCH_EVENT_BACKEND_KEVENT
