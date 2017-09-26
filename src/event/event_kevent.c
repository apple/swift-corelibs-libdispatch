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
	TAILQ_ENTRY(dispatch_muxnote_s) dmn_list;
	TAILQ_HEAD(, dispatch_unote_linkage_s) dmn_unotes_head;
	dispatch_wlh_t dmn_wlh;
	dispatch_kevent_s dmn_kev;
} *dispatch_muxnote_t;

static bool _dispatch_timers_force_max_leeway;
static int _dispatch_kq = -1;
static struct {
	dispatch_once_t pred;
	dispatch_unfair_lock_s lock;
} _dispatch_muxnotes;
#if !DISPATCH_USE_KEVENT_WORKQUEUE
#define _dispatch_muxnotes_lock() \
		_dispatch_unfair_lock_lock(&_dispatch_muxnotes.lock)
#define _dispatch_muxnotes_unlock() \
		_dispatch_unfair_lock_unlock(&_dispatch_muxnotes.lock)
#else
#define _dispatch_muxnotes_lock()
#define _dispatch_muxnotes_unlock()
#endif // !DISPATCH_USE_KEVENT_WORKQUEUE

DISPATCH_CACHELINE_ALIGN
static TAILQ_HEAD(dispatch_muxnote_bucket_s, dispatch_muxnote_s)
_dispatch_sources[DSL_HASH_SIZE];

#define DISPATCH_NOTE_CLOCK_WALL NOTE_MACH_CONTINUOUS_TIME
#define DISPATCH_NOTE_CLOCK_MACH 0

static const uint32_t _dispatch_timer_index_to_fflags[] = {
#define DISPATCH_TIMER_FFLAGS_INIT(kind, qos, note) \
	[DISPATCH_TIMER_INDEX(DISPATCH_CLOCK_##kind, DISPATCH_TIMER_QOS_##qos)] = \
			DISPATCH_NOTE_CLOCK_##kind | NOTE_ABSOLUTE | \
			NOTE_NSECONDS | NOTE_LEEWAY | (note)
	DISPATCH_TIMER_FFLAGS_INIT(WALL, NORMAL, 0),
	DISPATCH_TIMER_FFLAGS_INIT(MACH, NORMAL, 0),
#if DISPATCH_HAVE_TIMER_QOS
	DISPATCH_TIMER_FFLAGS_INIT(WALL, CRITICAL, NOTE_CRITICAL),
	DISPATCH_TIMER_FFLAGS_INIT(MACH, CRITICAL, NOTE_CRITICAL),
	DISPATCH_TIMER_FFLAGS_INIT(WALL, BACKGROUND, NOTE_BACKGROUND),
	DISPATCH_TIMER_FFLAGS_INIT(MACH, BACKGROUND, NOTE_BACKGROUND),
#endif
#undef DISPATCH_TIMER_FFLAGS_INIT
};

static void _dispatch_kevent_timer_drain(dispatch_kevent_t ke);

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

static dispatch_once_t _dispatch_mach_host_port_pred;
static mach_port_t _dispatch_mach_host_port;

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
#define DISPATCH_MACH_NOTIFICATION_ARMED(dk) ((dk)->ext[0])
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
		if (!_dispatch_unote_registered(_dispatch_kevent_get_unote(ke))) {
			ke->flags |= EV_ADD;
		}
	}

#if HAVE_MACH
	if (ke->filter == EVFILT_MACHPORT && ke->data == ENOTSUP &&
			(ke->flags & EV_ADD) && (ke->fflags & MACH_RCV_MSG)) {
		DISPATCH_INTERNAL_CRASH(ke->ident,
				"Missing EVFILT_MACHPORT support for ports");
	}
#endif

	if (ke->data) {
		// log the unexpected error
		_dispatch_bug_kevent_client("kevent", _evfiltstr(ke->filter),
				!ke->udata ? NULL :
				ke->flags & EV_DELETE ? "delete" :
				ke->flags & EV_ADD ? "add" :
				ke->flags & EV_ENABLE ? "enable" : "monitor",
				(int)ke->data);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_kevent_merge(dispatch_unote_t du, dispatch_kevent_t ke)
{
	uintptr_t data;
	uintptr_t status = 0;
	pthread_priority_t pp = 0;
#if DISPATCH_USE_KEVENT_QOS
	pp = ((pthread_priority_t)ke->qos) & ~_PTHREAD_PRIORITY_FLAGS_MASK;
#endif
	dispatch_unote_action_t action = du._du->du_data_action;
	if (action == DISPATCH_UNOTE_ACTION_DATA_SET) {
		// ke->data is signed and "negative available data" makes no sense
		// zero bytes happens when EV_EOF is set
		dispatch_assert(ke->data >= 0l);
		data = ~(unsigned long)ke->data;
#if HAVE_MACH
	} else if (du._du->du_filter == EVFILT_MACHPORT) {
		data = DISPATCH_MACH_RECV_MESSAGE;
#endif
	} else if (action == DISPATCH_UNOTE_ACTION_DATA_ADD) {
		data = (unsigned long)ke->data;
	} else if (action == DISPATCH_UNOTE_ACTION_DATA_OR) {
		data = ke->fflags & du._du->du_fflags;
	} else if (action == DISPATCH_UNOTE_ACTION_DATA_OR_STATUS_SET) {
		data = ke->fflags & du._du->du_fflags;
		status = (unsigned long)ke->data;
	} else {
		DISPATCH_INTERNAL_CRASH(action, "Corrupt unote action");
	}
	return dux_merge_evt(du._du, ke->flags, data, status, pp);
}

DISPATCH_NOINLINE
static void
_dispatch_kevent_merge_muxed(dispatch_kevent_t ke)
{
	dispatch_muxnote_t dmn = _dispatch_kevent_get_muxnote(ke);
	dispatch_unote_linkage_t dul, dul_next;

	TAILQ_FOREACH_SAFE(dul, &dmn->dmn_unotes_head, du_link, dul_next) {
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
			// EVFILT_PROC may fail with ESRCH when the process exists but is a zombie
			// <rdar://problem/5067725>. As a workaround, we simulate an exit event for
			// any EVFILT_PROC with an invalid pid <rdar://problem/6626350>.
			ke->flags &= ~(EV_ERROR | EV_ADD | EV_ENABLE | EV_UDATA_SPECIFIC);
			ke->flags |= EV_ONESHOT;
			ke->fflags = NOTE_EXIT;
			ke->data = 0;
			_dispatch_kevent_debug("synthetic NOTE_EXIT", ke);
		} else {
			return _dispatch_kevent_print_error(ke);
		}
	}
	if (ke->filter == EVFILT_TIMER) {
		return _dispatch_kevent_timer_drain(ke);
	}

#if HAVE_MACH
	if (ke->filter == EVFILT_MACHPORT) {
		if (_dispatch_kevent_mach_msg_size(ke)) {
			return _dispatch_kevent_mach_msg_drain(ke);
		}
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
static int
_dispatch_kq_create(const void *guard_ptr)
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
	guardid_t guard = (uintptr_t)guard_ptr;
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
	return kqfd;
}
#endif

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
		int kqfd = _dispatch_kq;
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
	_dispatch_kq = _dispatch_kq_create(&_dispatch_mgr_q);
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
	static dispatch_once_t pred;
	bool kq_initialized = false;
	int r = 0;

	dispatch_once_f(&pred, &kq_initialized, _dispatch_kq_init);
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
	if (wlh == DISPATCH_WLH_ANON) {
		int kqfd = _dispatch_kq;
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
	dispatch_source_type_t dst = du->du_type;
	uint16_t flags = dst->dst_flags | action;

	if ((flags & EV_VANISHED) && !(flags & EV_ADD)) {
		flags &= ~EV_VANISHED;
	}
	pthread_priority_t pp = _dispatch_priority_to_pp(du->du_priority);
	*dk = (dispatch_kevent_s){
		.ident  = du->du_ident,
		.filter = dst->dst_filter,
		.flags  = flags,
		.udata  = (uintptr_t)du,
		.fflags = du->du_fflags | dst->dst_fflags,
		.data   = (typeof(dk->data))dst->dst_data,
#if DISPATCH_USE_KEVENT_QOS
		.qos    = (typeof(dk->qos))pp,
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

	if (ddi && ddi->ddi_maxevents && wlh == _dispatch_get_wlh()) {
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
	if (ddi && wlh == _dispatch_get_wlh()) {
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
		// see du_wlh already set, else it will not unregister the kevent
		dispatch_assert(du->du_wlh == NULL);
		_dispatch_wlh_retain(wlh);
		du->du_wlh = wlh;
	}

	if (ddi && wlh == _dispatch_get_wlh()) {
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
			_dispatch_wlh_release(du->du_wlh);
			du->du_wlh = NULL;
		}
		return r == 0;
	}

	if (action_flags & EV_DELETE) {
		if (r == EINPROGRESS) {
			return false;
		}
		_dispatch_wlh_release(du->du_wlh);
		du->du_wlh = NULL;
	}

	dispatch_assume_zero(r);
	return true;
}

#pragma mark dispatch_muxnote_t

static void
_dispatch_muxnotes_init(void *ctxt DISPATCH_UNUSED)
{
	uint32_t i;
	for (i = 0; i < DSL_HASH_SIZE; i++) {
		TAILQ_INIT(&_dispatch_sources[i]);
	}
}

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

	dispatch_once_f(&_dispatch_muxnotes.pred, NULL, _dispatch_muxnotes_init);
	return &_dispatch_sources[DSL_HASH((uintptr_t)ident)];
}
#define _dispatch_unote_muxnote_bucket(du) \
	_dispatch_muxnote_bucket(du._du->du_ident, du._du->du_filter)

DISPATCH_ALWAYS_INLINE
static inline dispatch_muxnote_t
_dispatch_muxnote_find(struct dispatch_muxnote_bucket_s *dmb,
		dispatch_wlh_t wlh, uint64_t ident, int16_t filter)
{
	dispatch_muxnote_t dmn;
	_dispatch_muxnotes_lock();
	TAILQ_FOREACH(dmn, dmb, dmn_list) {
		if (dmn->dmn_wlh == wlh && dmn->dmn_kev.ident == ident &&
				dmn->dmn_kev.filter == filter) {
			break;
		}
	}
	_dispatch_muxnotes_unlock();
	return dmn;
}
#define _dispatch_unote_muxnote_find(dmb, du, wlh) \
		_dispatch_muxnote_find(dmb, wlh, du._du->du_ident, du._du->du_filter)

DISPATCH_ALWAYS_INLINE
static inline dispatch_muxnote_t
_dispatch_mach_muxnote_find(mach_port_t name, int16_t filter)
{
	struct dispatch_muxnote_bucket_s *dmb;
	dmb = _dispatch_muxnote_bucket(name, filter);
	return _dispatch_muxnote_find(dmb, DISPATCH_WLH_ANON, name, filter);
}

DISPATCH_NOINLINE
static bool
_dispatch_unote_register_muxed(dispatch_unote_t du, dispatch_wlh_t wlh)
{
	struct dispatch_muxnote_bucket_s *dmb = _dispatch_unote_muxnote_bucket(du);
	dispatch_muxnote_t dmn;
	bool installed = true;

	dmn = _dispatch_unote_muxnote_find(dmb, du, wlh);
	if (dmn) {
		uint32_t flags = du._du->du_fflags & ~dmn->dmn_kev.fflags;
		if (flags) {
			dmn->dmn_kev.fflags |= flags;
			if (unlikely(du._du->du_type->dst_update_mux)) {
				installed = du._du->du_type->dst_update_mux(dmn);
			} else {
				installed = !_dispatch_kq_immediate_update(dmn->dmn_wlh,
						&dmn->dmn_kev);
			}
			if (!installed) dmn->dmn_kev.fflags &= ~flags;
		}
	} else {
		dmn = _dispatch_calloc(1, sizeof(struct dispatch_muxnote_s));
		TAILQ_INIT(&dmn->dmn_unotes_head);
		_dispatch_kq_unote_set_kevent(du, &dmn->dmn_kev, EV_ADD | EV_ENABLE);
#if DISPATCH_USE_KEVENT_QOS
		dmn->dmn_kev.qos = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;
#endif
		dmn->dmn_kev.udata = (uintptr_t)dmn | DISPATCH_KEVENT_MUXED_MARKER;
		dmn->dmn_wlh = wlh;
		if (unlikely(du._du->du_type->dst_update_mux)) {
			installed = du._du->du_type->dst_update_mux(dmn);
		} else {
			installed = !_dispatch_kq_immediate_update(dmn->dmn_wlh,
					&dmn->dmn_kev);
		}
		if (installed) {
			dmn->dmn_kev.flags &= ~(EV_ADD | EV_VANISHED);
			_dispatch_muxnotes_lock();
			TAILQ_INSERT_TAIL(dmb, dmn, dmn_list);
			_dispatch_muxnotes_unlock();
		} else {
			free(dmn);
		}
	}

	if (installed) {
		dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);
		TAILQ_INSERT_TAIL(&dmn->dmn_unotes_head, dul, du_link);
		dul->du_muxnote = dmn;

		if (du._du->du_filter == DISPATCH_EVFILT_MACH_NOTIFICATION) {
			bool armed = DISPATCH_MACH_NOTIFICATION_ARMED(&dmn->dmn_kev);
			os_atomic_store2o(du._dmsr, dmsr_notification_armed, armed,relaxed);
		}
		du._du->du_wlh = DISPATCH_WLH_ANON;
	}
	return installed;
}

bool
_dispatch_unote_register(dispatch_unote_t du, dispatch_wlh_t wlh,
		dispatch_priority_t pri)
{
	dispatch_assert(!_dispatch_unote_registered(du));
	du._du->du_priority = pri;
	switch (du._du->du_filter) {
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
	case DISPATCH_EVFILT_CUSTOM_REPLACE:
		du._du->du_wlh = DISPATCH_WLH_ANON;
		return true;
	}
	if (!du._du->du_is_direct) {
		return _dispatch_unote_register_muxed(du, DISPATCH_WLH_ANON);
	}
	return _dispatch_kq_unote_update(wlh, du, EV_ADD | EV_ENABLE);
}

void
_dispatch_unote_resume(dispatch_unote_t du)
{
	dispatch_assert(_dispatch_unote_registered(du));

	if (du._du->du_is_direct) {
		dispatch_wlh_t wlh = du._du->du_wlh;
		_dispatch_kq_unote_update(wlh, du, EV_ENABLE);
	} else if (unlikely(du._du->du_type->dst_update_mux)) {
		dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);
		du._du->du_type->dst_update_mux(dul->du_muxnote);
	} else {
		dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);
		dispatch_muxnote_t dmn = dul->du_muxnote;
		_dispatch_kq_deferred_update(dmn->dmn_wlh, &dmn->dmn_kev);
	}
}

DISPATCH_NOINLINE
static bool
_dispatch_unote_unregister_muxed(dispatch_unote_t du, uint32_t flags)
{
	dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);
	dispatch_muxnote_t dmn = dul->du_muxnote;
	bool update = false, dispose = false;

	if (dmn->dmn_kev.filter == DISPATCH_EVFILT_MACH_NOTIFICATION) {
		os_atomic_store2o(du._dmsr, dmsr_notification_armed, false, relaxed);
	}
	dispatch_assert(du._du->du_wlh == DISPATCH_WLH_ANON);
	du._du->du_wlh = NULL;
	TAILQ_REMOVE(&dmn->dmn_unotes_head, dul, du_link);
	_TAILQ_TRASH_ENTRY(dul, du_link);
	dul->du_muxnote = NULL;

	if (TAILQ_EMPTY(&dmn->dmn_unotes_head)) {
		dmn->dmn_kev.flags |= EV_DELETE;
		update = dispose = true;
	} else {
		uint32_t fflags = du._du->du_type->dst_fflags;
		TAILQ_FOREACH(dul, &dmn->dmn_unotes_head, du_link) {
			du = _dispatch_unote_linkage_get_unote(dul);
			fflags |= du._du->du_fflags;
		}
		if (dmn->dmn_kev.fflags & ~fflags) {
			dmn->dmn_kev.fflags &= fflags;
			update = true;
		}
	}
	if (update && !(flags & DU_UNREGISTER_ALREADY_DELETED)) {
		if (unlikely(du._du->du_type->dst_update_mux)) {
			dispatch_assume(du._du->du_type->dst_update_mux(dmn));
		} else {
			_dispatch_kq_deferred_update(dmn->dmn_wlh, &dmn->dmn_kev);
		}
	}
	if (dispose) {
		struct dispatch_muxnote_bucket_s *dmb;
		dmb = _dispatch_muxnote_bucket(dmn->dmn_kev.ident, dmn->dmn_kev.filter);
		_dispatch_muxnotes_lock();
		TAILQ_REMOVE(dmb, dmn, dmn_list);
		_dispatch_muxnotes_unlock();
		free(dmn);
	}
	return true;
}

bool
_dispatch_unote_unregister(dispatch_unote_t du, uint32_t flags)
{
	switch (du._du->du_filter) {
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
	case DISPATCH_EVFILT_CUSTOM_REPLACE:
		du._du->du_wlh = NULL;
		return true;
	}
	dispatch_wlh_t wlh = du._du->du_wlh;
	if (wlh) {
		if (!du._du->du_is_direct) {
			return _dispatch_unote_unregister_muxed(du, flags);
		}
		uint16_t action_flags;
		if (flags & DU_UNREGISTER_ALREADY_DELETED) {
			action_flags = 0;
		} else if (flags & DU_UNREGISTER_IMMEDIATE_DELETE) {
			action_flags = EV_DELETE | EV_ENABLE;
		} else {
			action_flags = EV_DELETE;
		}
		return _dispatch_kq_unote_update(wlh, du, action_flags);
	}
	return true;
}

#pragma mark -
#pragma mark dispatch_event_loop

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
	dispatch_wlh_t wlh = _dispatch_get_wlh();
	dispatch_deferred_items_t ddi = _dispatch_deferred_items_get();
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
	dispatch_kevent_s kev[nevents];

	// now we can re-use the whole event list, but we need to save one slot
	// for the event loop poke
	memcpy(kev, events, sizeof(kev));
	ddi->ddi_maxevents = DISPATCH_DEFERRED_ITEMS_EVENT_COUNT - 1;

	for (int i = 0; i < nevents; i++) {
		_dispatch_kevent_drain(&kev[i]);
	}

	dispatch_wlh_t wlh = _dispatch_get_wlh();
	if (wlh == DISPATCH_WLH_ANON && ddi->ddi_stashed_dou._do) {
		if (ddi->ddi_nevents) {
			// We will drain the stashed item and not return to the kernel
			// right away. As a consequence, do not delay these updates.
			_dispatch_event_loop_drain(KEVENT_FLAG_IMMEDIATE |
					KEVENT_FLAG_ERROR_EVENTS);
		}
		_dispatch_trace_continuation_push(ddi->ddi_stashed_rq,
				ddi->ddi_stashed_dou);
	}
}

void
_dispatch_event_loop_leave_immediate(dispatch_wlh_t wlh, uint64_t dq_state)
{
	(void)wlh; (void)dq_state;
}

void
_dispatch_event_loop_leave_deferred(dispatch_wlh_t wlh, uint64_t dq_state)
{
	(void)wlh; (void)dq_state;
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
_dispatch_kevent_timer_drain(dispatch_kevent_t ke)
{
	dispatch_assert(ke->data > 0);
	dispatch_assert((ke->ident & DISPATCH_KEVENT_TIMEOUT_IDENT_MASK) ==
			DISPATCH_KEVENT_TIMEOUT_IDENT_MASK);
	uint32_t tidx = ke->ident & ~DISPATCH_KEVENT_TIMEOUT_IDENT_MASK;

	dispatch_assert(tidx < DISPATCH_TIMER_COUNT);
	_dispatch_timers_expired = true;
	_dispatch_timers_processing_mask |= 1 << tidx;
	_dispatch_timers_heap[tidx].dth_flags &= ~DTH_ARMED;
#if DISPATCH_USE_DTRACE
	_dispatch_timers_will_wake |= 1 << DISPATCH_TIMER_QOS(tidx);
#endif
}

DISPATCH_NOINLINE
static void
_dispatch_event_loop_timer_program(uint32_t tidx,
		uint64_t target, uint64_t leeway, uint16_t action)
{
	dispatch_kevent_s ke = {
		.ident = DISPATCH_KEVENT_TIMEOUT_IDENT_MASK | tidx,
		.filter = EVFILT_TIMER,
		.flags = action | EV_ONESHOT,
		.fflags = _dispatch_timer_index_to_fflags[tidx],
		.data = (int64_t)target,
		.udata = (uintptr_t)&_dispatch_timers_heap[tidx],
#if DISPATCH_HAVE_TIMER_COALESCING
		.ext[1] = leeway,
#endif
#if DISPATCH_USE_KEVENT_QOS
		.qos = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG,
#endif
	};

	_dispatch_kq_deferred_update(DISPATCH_WLH_ANON, &ke);
}

void
_dispatch_event_loop_timer_arm(uint32_t tidx, dispatch_timer_delay_s range,
		dispatch_clock_now_cache_t nows)
{
	if (unlikely(_dispatch_timers_force_max_leeway)) {
		range.delay += range.leeway;
		range.leeway = 0;
	}
#if HAVE_MACH
	if (DISPATCH_TIMER_CLOCK(tidx) == DISPATCH_CLOCK_WALL) {
		_dispatch_mach_host_calendar_change_register();
	}
#endif

	// <rdar://problem/13186331> EVFILT_TIMER NOTE_ABSOLUTE always expects
	// a WALL deadline
	uint64_t now = _dispatch_time_now_cached(DISPATCH_CLOCK_WALL, nows);
	_dispatch_timers_heap[tidx].dth_flags |= DTH_ARMED;
	_dispatch_event_loop_timer_program(tidx, now + range.delay, range.leeway,
			EV_ADD | EV_ENABLE);
}

void
_dispatch_event_loop_timer_delete(uint32_t tidx)
{
	_dispatch_timers_heap[tidx].dth_flags &= ~DTH_ARMED;
	_dispatch_event_loop_timer_program(tidx, 0, 0, EV_DELETE);
}

#pragma mark -
#pragma mark kevent specific sources

static dispatch_unote_t
_dispatch_source_proc_create(dispatch_source_type_t dst DISPATCH_UNUSED,
		uintptr_t handle, unsigned long mask DISPATCH_UNUSED)
{
	dispatch_unote_t du = _dispatch_unote_create_with_handle(dst, handle, mask);
	if (du._du && (mask & DISPATCH_PROC_EXIT_STATUS)) {
		du._du->du_data_action = DISPATCH_UNOTE_ACTION_DATA_OR_STATUS_SET;
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
	.dst_size       = sizeof(struct dispatch_source_refs_s),

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
	.dst_size       = sizeof(struct dispatch_source_refs_s),

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
	.dst_size       = sizeof(struct dispatch_source_refs_s),

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
	.dst_size       = sizeof(struct dispatch_source_refs_s),

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
	.dst_size       = sizeof(struct dispatch_source_refs_s),
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
			DISPATCH_MEMORYPRESSURE_SOURCE_MASK, &_dispatch_mgr_q);
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
	.dst_size       = sizeof(struct dispatch_source_refs_s),

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
	.dst_size       = sizeof(struct dispatch_source_refs_s),

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

static mach_port_t _dispatch_mach_notify_port;
static dispatch_source_t _dispatch_mach_notify_source;

static void
_dispatch_timers_calendar_change(void)
{
	uint32_t qos;

	// calendar change may have gone past the wallclock deadline
	_dispatch_timers_expired = true;
	for (qos = 0; qos < DISPATCH_TIMER_QOS_COUNT; qos++) {
		_dispatch_timers_processing_mask |=
				1 << DISPATCH_TIMER_INDEX(DISPATCH_CLOCK_WALL, qos);
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
_dispatch_mach_notify_source_invoke(mach_msg_header_t *hdr)
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
	if (hdr->msgh_id <= MACH_NOTIFY_LAST
			&& dispatch_assume_zero(tlr->msgh_audit.val[
			DISPATCH_MACH_AUDIT_TOKEN_PID])) {
		mach_msg_destroy(hdr);
		return;
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
}

DISPATCH_NOINLINE
static void
_dispatch_mach_notify_port_init(void *context DISPATCH_UNUSED)
{
	kern_return_t kr;
#if HAVE_MACH_PORT_CONSTRUCT
	mach_port_options_t opts = { .flags = MPO_CONTEXT_AS_GUARD | MPO_STRICT };
#if DISPATCH_SIZEOF_PTR == 8
	const mach_port_context_t guard = 0xfeed09071f1ca7edull;
#else
	const mach_port_context_t guard = 0xff1ca7edull;
#endif
	kr = mach_port_construct(mach_task_self(), &opts, guard,
			&_dispatch_mach_notify_port);
#else
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			&_dispatch_mach_notify_port);
#endif
	DISPATCH_VERIFY_MIG(kr);
	if (unlikely(kr)) {
		DISPATCH_CLIENT_CRASH(kr,
				"mach_port_construct() failed: cannot create receive right");
	}

	static const struct dispatch_continuation_s dc = {
		.dc_func = (void*)_dispatch_mach_notify_source_invoke,
	};
	_dispatch_mach_notify_source = _dispatch_source_create_mach_msg_direct_recv(
			_dispatch_mach_notify_port, &dc);
	dispatch_assert(_dispatch_mach_notify_source);
	dispatch_activate(_dispatch_mach_notify_source);
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
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_mach_notify_port_init);
	return _dispatch_mach_notify_port;
}

static void
_dispatch_mach_host_notify_update(void *context DISPATCH_UNUSED)
{
	static int notify_type = HOST_NOTIFY_CALENDAR_SET;
	kern_return_t kr;
	_dispatch_debug("registering for calendar-change notification");
retry:
	kr = host_request_notification(_dispatch_get_mach_host_port(),
			notify_type, _dispatch_get_mach_notify_port());
	// Fallback when missing support for newer _SET variant, fires strictly more
	if (kr == KERN_INVALID_ARGUMENT &&
			notify_type != HOST_NOTIFY_CALENDAR_CHANGE) {
		notify_type = HOST_NOTIFY_CALENDAR_CHANGE;
		goto retry;
	}
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_mach_host_calendar_change_register(void)
{
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_mach_host_notify_update);
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

	_dispatch_debug_machport(name);
	dmn = _dispatch_mach_muxnote_find(name, DISPATCH_EVFILT_MACH_NOTIFICATION);
	if (!dmn) {
		return;
	}

	dmn->dmn_kev.data &= ~_DISPATCH_MACH_SP_FLAGS;
	if (!final) {
		// Re-register for notification before delivery
		final = !_dispatch_kevent_mach_notify_resume(dmn, data, 0);
	}

	uint32_t flags = final ? EV_ONESHOT : EV_ENABLE;
	DISPATCH_MACH_NOTIFICATION_ARMED(&dmn->dmn_kev) = 0;
	TAILQ_FOREACH_SAFE(dul, &dmn->dmn_unotes_head, du_link, dul_next) {
		dispatch_unote_t du = _dispatch_unote_linkage_get_unote(dul);
		os_atomic_store2o(du._dmsr, dmsr_notification_armed, false, relaxed);
		dux_merge_evt(du._du, flags, (data & du._du->du_fflags), 0, 0);
		if (!dul_next || DISPATCH_MACH_NOTIFICATION_ARMED(&dmn->dmn_kev)) {
			// current merge is last in list (dmn might have been freed)
			// or it re-armed the notification
			break;
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
	dispatch_unote_t du;

	if (!_dispatch_unote_registered(dmsr)) {
		return;
	}

	DISPATCH_MACH_NOTIFICATION_ARMED(&dmn->dmn_kev) = true;
	TAILQ_FOREACH(dul, &dmn->dmn_unotes_head, du_link) {
		du = _dispatch_unote_linkage_get_unote(dul);
		os_atomic_store2o(du._dmsr, dmsr_notification_armed, true, relaxed);
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
	.dst_size       = sizeof(struct dispatch_source_refs_s),

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
		TAILQ_INIT(&du._dmsr->dmsr_replies);
	}
	return du;
}

const dispatch_source_type_s _dispatch_mach_type_send = {
	.dst_kind       = "mach_send (mach)",
	.dst_filter     = DISPATCH_EVFILT_MACH_NOTIFICATION,
	.dst_flags      = EV_CLEAR,
	.dst_mask       = DISPATCH_MACH_SEND_DEAD|DISPATCH_MACH_SEND_POSSIBLE,
	.dst_size       = sizeof(struct dispatch_mach_send_refs_s),

	.dst_create     = _dispatch_mach_send_create,
	.dst_update_mux = _dispatch_mach_send_update,
	.dst_merge_evt  = _dispatch_mach_merge_notification,
};

#endif // HAVE_MACH
#pragma mark mach recv / reply
#if HAVE_MACH

static void
_dispatch_kevent_mach_msg_recv(dispatch_unote_t du, uint32_t flags,
		mach_msg_header_t *hdr)
{
	mach_msg_size_t siz = hdr->msgh_size + DISPATCH_MACH_TRAILER_SIZE;
	mach_port_t name = hdr->msgh_local_port;

	if (!dispatch_assume(hdr->msgh_size <= UINT_MAX -
			DISPATCH_MACH_TRAILER_SIZE)) {
		_dispatch_bug_client("_dispatch_kevent_mach_msg_recv: "
				"received overlarge message");
	} else if (!dispatch_assume(name)) {
		_dispatch_bug_client("_dispatch_kevent_mach_msg_recv: "
				"received message with MACH_PORT_NULL port");
	} else {
		_dispatch_debug_machport(name);
		if (likely(du._du)) {
			return dux_merge_msg(du._du, flags, hdr, siz);
		}
		_dispatch_bug_client("_dispatch_kevent_mach_msg_recv: "
				"received message with no listeners");
	}

	mach_msg_destroy(hdr);
	if (flags & DISPATCH_EV_MSG_NEEDS_FREE) {
		free(hdr);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_kevent_mach_msg_drain(dispatch_kevent_t ke)
{
	mach_msg_header_t *hdr = _dispatch_kevent_mach_msg_buf(ke);
	mach_msg_size_t siz;
	mach_msg_return_t kr = (mach_msg_return_t)ke->fflags;
	uint32_t flags = ke->flags;
	dispatch_unote_t du = _dispatch_kevent_get_unote(ke);

	if (unlikely(!hdr)) {
		DISPATCH_INTERNAL_CRASH(kr, "EVFILT_MACHPORT with no message");
	}
	if (likely(!kr)) {
		_dispatch_kevent_mach_msg_recv(du, flags, hdr);
		goto out;
	} else if (kr != MACH_RCV_TOO_LARGE) {
		goto out;
	} else if (!ke->data) {
		DISPATCH_INTERNAL_CRASH(0, "MACH_RCV_LARGE_IDENTITY with no identity");
	}
	if (unlikely(ke->ext[1] > (UINT_MAX - DISPATCH_MACH_TRAILER_SIZE))) {
		DISPATCH_INTERNAL_CRASH(ke->ext[1],
				"EVFILT_MACHPORT with overlarge message");
	}
	siz = _dispatch_kevent_mach_msg_size(ke) + DISPATCH_MACH_TRAILER_SIZE;
	hdr = malloc(siz);
	if (dispatch_assume(hdr)) {
		flags |= DISPATCH_EV_MSG_NEEDS_FREE;
	} else {
		// Kernel will discard message too large to fit
		hdr = NULL;
		siz = 0;
	}
	mach_port_t name = (mach_port_name_t)ke->data;
	const mach_msg_option_t options = ((DISPATCH_MACH_RCV_OPTIONS |
			MACH_RCV_TIMEOUT) & ~MACH_RCV_LARGE);
	kr = mach_msg(hdr, options, 0, siz, name, MACH_MSG_TIMEOUT_NONE,
			MACH_PORT_NULL);
	if (likely(!kr)) {
		_dispatch_kevent_mach_msg_recv(du, flags, hdr);
		goto out;
	} else if (kr == MACH_RCV_TOO_LARGE) {
		_dispatch_log("BUG in libdispatch client: "
				"_dispatch_kevent_mach_msg_drain: dropped message too "
				"large to fit in memory: id = 0x%x, size = %u",
				hdr->msgh_id, _dispatch_kevent_mach_msg_size(ke));
		kr = MACH_MSG_SUCCESS;
	}
	if (flags & DISPATCH_EV_MSG_NEEDS_FREE) {
		free(hdr);
	}
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
	.dst_size       = sizeof(struct dispatch_source_refs_s),

	.dst_create     = _dispatch_unote_create_with_handle,
	.dst_merge_evt  = _dispatch_source_merge_evt,
	.dst_merge_msg  = NULL, // never receives messages directly

	.dst_per_trigger_qos = true,
};

static void
_dispatch_source_mach_recv_direct_merge_msg(dispatch_unote_t du, uint32_t flags,
		mach_msg_header_t *msg, mach_msg_size_t msgsz DISPATCH_UNUSED)
{
	dispatch_continuation_t dc = du._dr->ds_handler[DS_EVENT_HANDLER];
	dispatch_source_t ds = _dispatch_source_from_refs(du._dr);
	dispatch_queue_t cq = _dispatch_queue_get_current();

	// see firehose_client_push_notify_async
	_dispatch_queue_set_current(ds->_as_dq);
	dc->dc_func(msg);
	_dispatch_queue_set_current(cq);
	if (flags & DISPATCH_EV_MSG_NEEDS_FREE) {
		free(msg);
	}
	if ((ds->dq_atomic_flags & DSF_CANCELED) ||
			(flags & (EV_ONESHOT | EV_DELETE))) {
		return _dispatch_source_merge_evt(du, flags, 0, 0, 0);
	}
	if (_dispatch_unote_needs_rearm(du)) {
		return _dispatch_unote_resume(du);
	}
}

static void
_dispatch_mach_recv_direct_merge(dispatch_unote_t du,
		uint32_t flags, uintptr_t data,
		uintptr_t status DISPATCH_UNUSED,
		pthread_priority_t pp)
{
	if (flags & EV_VANISHED) {
		DISPATCH_CLIENT_CRASH(du._du->du_ident,
				"Unexpected EV_VANISHED (do not destroy random mach ports)");
	}
	return _dispatch_source_merge_evt(du, flags, data, 0, pp);
}

const dispatch_source_type_s _dispatch_source_type_mach_recv_direct = {
	.dst_kind       = "direct mach_recv",
	.dst_filter     = EVFILT_MACHPORT,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_DISPATCH|EV_VANISHED,
	.dst_fflags     = DISPATCH_MACH_RCV_OPTIONS,
	.dst_size       = sizeof(struct dispatch_source_refs_s),

	.dst_create     = _dispatch_unote_create_with_handle,
	.dst_merge_evt  = _dispatch_mach_recv_direct_merge,
	.dst_merge_msg  = _dispatch_source_mach_recv_direct_merge_msg,

	.dst_per_trigger_qos = true,
};

const dispatch_source_type_s _dispatch_mach_type_recv = {
	.dst_kind       = "mach_recv (channel)",
	.dst_filter     = EVFILT_MACHPORT,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_DISPATCH|EV_VANISHED,
	.dst_fflags     = DISPATCH_MACH_RCV_OPTIONS,
	.dst_size       = sizeof(struct dispatch_mach_recv_refs_s),

	 // without handle because the mach code will set the ident after connect
	.dst_create     = _dispatch_unote_create_without_handle,
	.dst_merge_evt  = _dispatch_mach_recv_direct_merge,
	.dst_merge_msg  = _dispatch_mach_merge_msg,

	.dst_per_trigger_qos = true,
};

DISPATCH_NORETURN
static void
_dispatch_mach_reply_merge_evt(dispatch_unote_t du,
		uint32_t flags DISPATCH_UNUSED, uintptr_t data DISPATCH_UNUSED,
		uintptr_t status DISPATCH_UNUSED,
		pthread_priority_t pp DISPATCH_UNUSED)
{
	DISPATCH_INTERNAL_CRASH(du._du->du_ident, "Unexpected event");
}

const dispatch_source_type_s _dispatch_mach_type_reply = {
	.dst_kind       = "mach reply",
	.dst_filter     = EVFILT_MACHPORT,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_DISPATCH|EV_ONESHOT|EV_VANISHED,
	.dst_fflags     = DISPATCH_MACH_RCV_OPTIONS,
	.dst_size       = sizeof(struct dispatch_mach_reply_refs_s),

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
	.dst_size       = sizeof(struct dispatch_xpc_term_refs_s),

	.dst_create     = _dispatch_unote_create_with_handle,
	.dst_merge_evt  = _dispatch_xpc_sigterm_merge,
};

#endif // HAVE_MACH

#endif // DISPATCH_EVENT_BACKEND_KEVENT
