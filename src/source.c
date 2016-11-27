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
#if HAVE_MACH
#include "protocol.h"
#include "protocolServer.h"
#endif
#include <sys/mount.h>

#define DKEV_DISPOSE_IMMEDIATE_DELETE 0x1
#define DKEV_UNREGISTER_DISCONNECTED 0x2
#define DKEV_UNREGISTER_REPLY_REMOVE 0x4
#define DKEV_UNREGISTER_WAKEUP 0x8

static void _dispatch_source_handler_free(dispatch_source_t ds, long kind);
static void _dispatch_source_merge_kevent(dispatch_source_t ds,
		const _dispatch_kevent_qos_s *ke);
static bool _dispatch_kevent_register(dispatch_kevent_t *dkp,
		pthread_priority_t pp, uint32_t *flgp);
static long _dispatch_kevent_unregister(dispatch_kevent_t dk, uint32_t flg,
		unsigned int options);
static long _dispatch_kevent_resume(dispatch_kevent_t dk, uint32_t new_flags,
		uint32_t del_flags);
static void _dispatch_kevent_drain(_dispatch_kevent_qos_s *ke);
static void _dispatch_kevent_merge(_dispatch_kevent_qos_s *ke);
static void _dispatch_timers_kevent(_dispatch_kevent_qos_s *ke);
static void _dispatch_timers_unregister(dispatch_source_t ds,
		dispatch_kevent_t dk);
static void _dispatch_timers_update(dispatch_source_t ds);
static void _dispatch_timer_aggregates_check(void);
static void _dispatch_timer_aggregates_register(dispatch_source_t ds);
static void _dispatch_timer_aggregates_update(dispatch_source_t ds,
		unsigned int tidx);
static void _dispatch_timer_aggregates_unregister(dispatch_source_t ds,
		unsigned int tidx);
static inline unsigned long _dispatch_source_timer_data(
		dispatch_source_refs_t dr, unsigned long prev);
static void _dispatch_kq_deferred_update(const _dispatch_kevent_qos_s *ke);
static long _dispatch_kq_immediate_update(_dispatch_kevent_qos_s *ke);
static void _dispatch_memorypressure_init(void);
#if HAVE_MACH
static void _dispatch_mach_host_calendar_change_register(void);
#if DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK
static void _dispatch_mach_recv_msg_buf_init(void);
static kern_return_t _dispatch_kevent_machport_resume(dispatch_kevent_t dk,
		uint32_t new_flags, uint32_t del_flags);
#endif
static kern_return_t _dispatch_kevent_mach_notify_resume(dispatch_kevent_t dk,
		uint32_t new_flags, uint32_t del_flags);
static void _dispatch_mach_kevent_merge(_dispatch_kevent_qos_s *ke);
static mach_msg_size_t _dispatch_kevent_mach_msg_size(
		_dispatch_kevent_qos_s *ke);
#else
static inline void _dispatch_mach_host_calendar_change_register(void) {}
static inline void _dispatch_mach_recv_msg_buf_init(void) {}
#endif
static const char * _evfiltstr(short filt);
#if DISPATCH_DEBUG
static void dispatch_kevent_debug(const char *verb,
		const _dispatch_kevent_qos_s *kev, int i, int n,
		const char *function, unsigned int line);
static void _dispatch_kevent_debugger(void *context);
#define DISPATCH_ASSERT_ON_MANAGER_QUEUE() \
	dispatch_assert(_dispatch_queue_get_current() == &_dispatch_mgr_q)
#else
static inline void
dispatch_kevent_debug(const char *verb, const _dispatch_kevent_qos_s *kev,
		int i, int n, const char *function, unsigned int line)
{
	(void)verb; (void)kev; (void)i; (void)n; (void)function; (void)line;
}
#define DISPATCH_ASSERT_ON_MANAGER_QUEUE()
#endif
#define _dispatch_kevent_debug(verb, _kev) \
		dispatch_kevent_debug(verb, _kev, 0, 1, __FUNCTION__, __LINE__)
#define _dispatch_kevent_debug_n(verb, _kev, i, n) \
		dispatch_kevent_debug(verb, _kev, i, n, __FUNCTION__, __LINE__)
#ifndef DISPATCH_MGR_QUEUE_DEBUG
#define DISPATCH_MGR_QUEUE_DEBUG 0
#endif
#if DISPATCH_MGR_QUEUE_DEBUG
#define _dispatch_kevent_mgr_debug _dispatch_kevent_debug
#else
static inline void
_dispatch_kevent_mgr_debug(_dispatch_kevent_qos_s* kev DISPATCH_UNUSED) {}
#endif

#pragma mark -
#pragma mark dispatch_source_t

dispatch_source_t
dispatch_source_create(dispatch_source_type_t type, uintptr_t handle,
		unsigned long mask, dispatch_queue_t dq)
{
	// ensure _dispatch_evfilt_machport_direct_enabled is initialized
	_dispatch_root_queues_init();
	const _dispatch_kevent_qos_s *proto_kev = &type->ke;
	dispatch_source_t ds;
	dispatch_kevent_t dk;

	// input validation
	if (type == NULL || (mask & ~type->mask)) {
		return DISPATCH_BAD_INPUT;
	}
	if (type->mask && !mask) {
		// expect a non-zero mask when the type declares one ... except
		switch (type->ke.filter) {
		case DISPATCH_EVFILT_TIMER:
			break; // timers don't need masks
#if DISPATCH_USE_VM_PRESSURE
		case EVFILT_VM:
			break; // type->init forces the only acceptable mask
#endif
		case DISPATCH_EVFILT_MACH_NOTIFICATION:
			break; // type->init handles zero mask as a legacy case
		default:
			// otherwise reject as invalid input
			return DISPATCH_BAD_INPUT;
		}
	}

	switch (type->ke.filter) {
	case EVFILT_SIGNAL:
		if (handle >= NSIG) {
			return DISPATCH_BAD_INPUT;
		}
		break;
	case EVFILT_FS:
#if DISPATCH_USE_VM_PRESSURE
	case EVFILT_VM:
#endif
#if DISPATCH_USE_MEMORYSTATUS
	case EVFILT_MEMORYSTATUS:
#endif
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
		if (handle) {
			return DISPATCH_BAD_INPUT;
		}
		break;
	case DISPATCH_EVFILT_TIMER:
		if ((handle == 0) != (type->ke.ident == 0)) {
			return DISPATCH_BAD_INPUT;
		}
		break;
	default:
		break;
	}

	ds = _dispatch_alloc(DISPATCH_VTABLE(source),
			sizeof(struct dispatch_source_s));
	// Initialize as a queue first, then override some settings below.
	_dispatch_queue_init(ds->_as_dq, DQF_NONE, 1, true);
	ds->dq_label = "source";
	ds->do_ref_cnt++; // the reference the manager queue holds

	switch (type->ke.filter) {
	case DISPATCH_EVFILT_CUSTOM_OR:
		dk = DISPATCH_KEV_CUSTOM_OR;
		break;
	case DISPATCH_EVFILT_CUSTOM_ADD:
		dk = DISPATCH_KEV_CUSTOM_ADD;
		break;
	default:
		dk = _dispatch_calloc(1ul, sizeof(struct dispatch_kevent_s));
		dk->dk_kevent = *proto_kev;
		dk->dk_kevent.ident = handle;
		dk->dk_kevent.flags |= EV_ADD|EV_ENABLE;
		dk->dk_kevent.fflags |= (uint32_t)mask;
		dk->dk_kevent.udata = (_dispatch_kevent_qos_udata_t)dk;
		TAILQ_INIT(&dk->dk_sources);

		ds->ds_pending_data_mask = dk->dk_kevent.fflags;
		ds->ds_ident_hack = (uintptr_t)dk->dk_kevent.ident;
		if (EV_UDATA_SPECIFIC & proto_kev->flags) {
			dk->dk_kevent.flags |= EV_DISPATCH;
			ds->ds_is_direct_kevent = true;
			ds->ds_needs_rearm = true;
		}
		break;
	}
	ds->ds_dkev = dk;

	if ((EV_DISPATCH|EV_ONESHOT) & proto_kev->flags) {
		ds->ds_needs_rearm = true;
	} else if (!(EV_CLEAR & proto_kev->flags)) {
		// we cheat and use EV_CLEAR to mean a "flag thingy"
		ds->ds_is_adder = true;
	}
	// Some sources require special processing
	if (type->init != NULL) {
		type->init(ds, type, handle, mask, dq);
	}
	dispatch_assert(!(ds->ds_is_level && ds->ds_is_adder));
	if (!ds->ds_is_custom_source && (dk->dk_kevent.flags & EV_VANISHED)) {
		// see _dispatch_source_merge_kevent
		dispatch_assert(!(dk->dk_kevent.flags & EV_ONESHOT));
		dispatch_assert(dk->dk_kevent.flags & EV_DISPATCH);
		dispatch_assert(dk->dk_kevent.flags & EV_UDATA_SPECIFIC);
	}

	if (fastpath(!ds->ds_refs)) {
		ds->ds_refs = _dispatch_calloc(1ul,
				sizeof(struct dispatch_source_refs_s));
	}
	ds->ds_refs->dr_source_wref = _dispatch_ptr2wref(ds);

	if (slowpath(!dq)) {
		dq = _dispatch_get_root_queue(_DISPATCH_QOS_CLASS_DEFAULT, true);
	} else {
		_dispatch_retain(dq);
	}
	ds->do_targetq = dq;
	_dispatch_object_debug(ds, "%s", __func__);
	return ds;
}

void
_dispatch_source_dispose(dispatch_source_t ds)
{
	_dispatch_object_debug(ds, "%s", __func__);
	_dispatch_source_handler_free(ds, DS_REGISTN_HANDLER);
	_dispatch_source_handler_free(ds, DS_EVENT_HANDLER);
	_dispatch_source_handler_free(ds, DS_CANCEL_HANDLER);
	free(ds->ds_refs);
	_dispatch_queue_destroy(ds->_as_dq);
}

void
_dispatch_source_xref_dispose(dispatch_source_t ds)
{
	dx_wakeup(ds, 0, DISPATCH_WAKEUP_FLUSH);
}

long
dispatch_source_testcancel(dispatch_source_t ds)
{
	return (bool)(ds->dq_atomic_flags & DSF_CANCELED);
}

unsigned long
dispatch_source_get_mask(dispatch_source_t ds)
{
	unsigned long mask = ds->ds_pending_data_mask;
	if (ds->ds_vmpressure_override) {
		mask = NOTE_VM_PRESSURE;
	}
#if TARGET_IPHONE_SIMULATOR
	else if (ds->ds_memorypressure_override) {
		mask = NOTE_MEMORYSTATUS_PRESSURE_WARN;
	}
#endif
	return mask;
}

uintptr_t
dispatch_source_get_handle(dispatch_source_t ds)
{
	unsigned int handle = (unsigned int)ds->ds_ident_hack;
#if TARGET_IPHONE_SIMULATOR
	if (ds->ds_memorypressure_override) {
		handle = 0;
	}
#endif
	return handle;
}

unsigned long
dispatch_source_get_data(dispatch_source_t ds)
{
	unsigned long data = ds->ds_data;
	if (ds->ds_vmpressure_override) {
		data = NOTE_VM_PRESSURE;
	}
#if TARGET_IPHONE_SIMULATOR
	else if (ds->ds_memorypressure_override) {
		data = NOTE_MEMORYSTATUS_PRESSURE_WARN;
	}
#endif
	return data;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_source_merge_data2(dispatch_source_t ds,
		pthread_priority_t pp, unsigned long val)
{
	_dispatch_kevent_qos_s kev = {
		.fflags = (typeof(kev.fflags))val,
		.data = (typeof(kev.data))val,
#if DISPATCH_USE_KEVENT_QOS
		.qos = (_dispatch_kevent_priority_t)pp,
#endif
	};
#if !DISPATCH_USE_KEVENT_QOS
	(void)pp;
#endif

	dispatch_assert(ds->ds_dkev == DISPATCH_KEV_CUSTOM_OR ||
			ds->ds_dkev == DISPATCH_KEV_CUSTOM_ADD);
	_dispatch_kevent_debug("synthetic data", &kev);
	_dispatch_source_merge_kevent(ds, &kev);
}

void
dispatch_source_merge_data(dispatch_source_t ds, unsigned long val)
{
	_dispatch_source_merge_data2(ds, 0, val);
}

void
_dispatch_source_merge_data(dispatch_source_t ds, pthread_priority_t pp,
		unsigned long val)
{
	_dispatch_source_merge_data2(ds, pp, val);
}

#pragma mark -
#pragma mark dispatch_source_handler

DISPATCH_ALWAYS_INLINE
static inline dispatch_continuation_t
_dispatch_source_get_handler(dispatch_source_refs_t dr, long kind)
{
	return os_atomic_load(&dr->ds_handler[kind], relaxed);
}
#define _dispatch_source_get_event_handler(dr) \
	_dispatch_source_get_handler(dr, DS_EVENT_HANDLER)
#define _dispatch_source_get_cancel_handler(dr) \
	_dispatch_source_get_handler(dr, DS_CANCEL_HANDLER)
#define _dispatch_source_get_registration_handler(dr) \
	_dispatch_source_get_handler(dr, DS_REGISTN_HANDLER)

DISPATCH_ALWAYS_INLINE
static inline dispatch_continuation_t
_dispatch_source_handler_alloc(dispatch_source_t ds, void *func, long kind,
		bool block)
{
	// sources don't propagate priority by default
	const dispatch_block_flags_t flags =
			DISPATCH_BLOCK_HAS_PRIORITY | DISPATCH_BLOCK_NO_VOUCHER;
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	if (func) {
		uintptr_t dc_flags = 0;

		if (kind != DS_EVENT_HANDLER) {
			dc_flags |= DISPATCH_OBJ_CONSUME_BIT;
		}
		if (block) {
#ifdef __BLOCKS__
			_dispatch_continuation_init(dc, ds, func, 0, flags, dc_flags);
#endif /* __BLOCKS__ */
		} else {
			dc_flags |= DISPATCH_OBJ_CTXT_FETCH_BIT;
			_dispatch_continuation_init_f(dc, ds, ds->do_ctxt, func,
					0, flags, dc_flags);
		}
		_dispatch_trace_continuation_push(ds->_as_dq, dc);
	} else {
		dc->dc_flags = 0;
		dc->dc_func = NULL;
	}
	return dc;
}

DISPATCH_NOINLINE
static void
_dispatch_source_handler_dispose(dispatch_continuation_t dc)
{
#ifdef __BLOCKS__
	if (dc->dc_flags & DISPATCH_OBJ_BLOCK_BIT) {
		Block_release(dc->dc_ctxt);
	}
#endif /* __BLOCKS__ */
	if (dc->dc_voucher) {
		_voucher_release(dc->dc_voucher);
		dc->dc_voucher = VOUCHER_INVALID;
	}
	_dispatch_continuation_free(dc);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_continuation_t
_dispatch_source_handler_take(dispatch_source_t ds, long kind)
{
	return os_atomic_xchg(&ds->ds_refs->ds_handler[kind], NULL, relaxed);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_source_handler_free(dispatch_source_t ds, long kind)
{
	dispatch_continuation_t dc = _dispatch_source_handler_take(ds, kind);
	if (dc) _dispatch_source_handler_dispose(dc);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_source_handler_replace(dispatch_source_t ds, long kind,
		dispatch_continuation_t dc)
{
	if (!dc->dc_func) {
		_dispatch_continuation_free(dc);
		dc = NULL;
	} else if (dc->dc_flags & DISPATCH_OBJ_CTXT_FETCH_BIT) {
		dc->dc_ctxt = ds->do_ctxt;
	}
	dc = os_atomic_xchg(&ds->ds_refs->ds_handler[kind], dc, release);
	if (dc) _dispatch_source_handler_dispose(dc);
}

DISPATCH_NOINLINE
static void
_dispatch_source_set_handler_slow(void *context)
{
	dispatch_source_t ds = (dispatch_source_t)_dispatch_queue_get_current();
	dispatch_assert(dx_type(ds) == DISPATCH_SOURCE_KEVENT_TYPE);

	dispatch_continuation_t dc = context;
	long kind = (long)dc->dc_data;
	dc->dc_data = NULL;
	_dispatch_source_handler_replace(ds, kind, dc);
}

DISPATCH_NOINLINE
static void
_dispatch_source_set_handler(dispatch_source_t ds, long kind,
		dispatch_continuation_t dc)
{
	dispatch_assert(dx_type(ds) == DISPATCH_SOURCE_KEVENT_TYPE);
	if (_dispatch_queue_try_inactive_suspend(ds->_as_dq)) {
		_dispatch_source_handler_replace(ds, kind, dc);
		return dx_vtable(ds)->do_resume(ds, false);
	}
	_dispatch_ktrace1(DISPATCH_PERF_post_activate_mutation, ds);
	if (kind == DS_REGISTN_HANDLER) {
		_dispatch_bug_deprecated("Setting registration handler after "
				"the source has been activated");
	}
	dc->dc_data = (void *)kind;
	_dispatch_barrier_trysync_or_async_f(ds->_as_dq, dc,
			_dispatch_source_set_handler_slow);
}

#ifdef __BLOCKS__
void
dispatch_source_set_event_handler(dispatch_source_t ds,
		dispatch_block_t handler)
{
	dispatch_continuation_t dc;
	dc = _dispatch_source_handler_alloc(ds, handler, DS_EVENT_HANDLER, true);
	_dispatch_source_set_handler(ds, DS_EVENT_HANDLER, dc);
}
#endif /* __BLOCKS__ */

void
dispatch_source_set_event_handler_f(dispatch_source_t ds,
		dispatch_function_t handler)
{
	dispatch_continuation_t dc;
	dc = _dispatch_source_handler_alloc(ds, handler, DS_EVENT_HANDLER, false);
	_dispatch_source_set_handler(ds, DS_EVENT_HANDLER, dc);
}

void
_dispatch_source_set_event_handler_continuation(dispatch_source_t ds,
		dispatch_continuation_t dc)
{
	_dispatch_trace_continuation_push(ds->_as_dq, dc);
	_dispatch_source_set_handler(ds, DS_EVENT_HANDLER, dc);
}

#ifdef __BLOCKS__
void
dispatch_source_set_cancel_handler(dispatch_source_t ds,
		dispatch_block_t handler)
{
	dispatch_continuation_t dc;
	dc = _dispatch_source_handler_alloc(ds, handler, DS_CANCEL_HANDLER, true);
	_dispatch_source_set_handler(ds, DS_CANCEL_HANDLER, dc);
}
#endif /* __BLOCKS__ */

void
dispatch_source_set_cancel_handler_f(dispatch_source_t ds,
		dispatch_function_t handler)
{
	dispatch_continuation_t dc;
	dc = _dispatch_source_handler_alloc(ds, handler, DS_CANCEL_HANDLER, false);
	_dispatch_source_set_handler(ds, DS_CANCEL_HANDLER, dc);
}

#ifdef __BLOCKS__
void
dispatch_source_set_registration_handler(dispatch_source_t ds,
		dispatch_block_t handler)
{
	dispatch_continuation_t dc;
	dc = _dispatch_source_handler_alloc(ds, handler, DS_REGISTN_HANDLER, true);
	_dispatch_source_set_handler(ds, DS_REGISTN_HANDLER, dc);
}
#endif /* __BLOCKS__ */

void
dispatch_source_set_registration_handler_f(dispatch_source_t ds,
	dispatch_function_t handler)
{
	dispatch_continuation_t dc;
	dc = _dispatch_source_handler_alloc(ds, handler, DS_REGISTN_HANDLER, false);
	_dispatch_source_set_handler(ds, DS_REGISTN_HANDLER, dc);
}

#pragma mark -
#pragma mark dispatch_source_invoke

static void
_dispatch_source_registration_callout(dispatch_source_t ds, dispatch_queue_t cq,
		dispatch_invoke_flags_t flags)
{
	dispatch_continuation_t dc;

	dc = _dispatch_source_handler_take(ds, DS_REGISTN_HANDLER);
	if (ds->dq_atomic_flags & (DSF_CANCELED | DQF_RELEASED)) {
		// no registration callout if source is canceled rdar://problem/8955246
		return _dispatch_source_handler_dispose(dc);
	}
	if (dc->dc_flags & DISPATCH_OBJ_CTXT_FETCH_BIT) {
		dc->dc_ctxt = ds->do_ctxt;
	}
	_dispatch_continuation_pop(dc, cq, flags);
}

static void
_dispatch_source_cancel_callout(dispatch_source_t ds, dispatch_queue_t cq,
		dispatch_invoke_flags_t flags)
{
	dispatch_continuation_t dc;

	dc = _dispatch_source_handler_take(ds, DS_CANCEL_HANDLER);
	ds->ds_pending_data_mask = 0;
	ds->ds_pending_data = 0;
	ds->ds_data = 0;
	_dispatch_source_handler_free(ds, DS_EVENT_HANDLER);
	_dispatch_source_handler_free(ds, DS_REGISTN_HANDLER);
	if (!dc) {
		return;
	}
	if (!(ds->dq_atomic_flags & DSF_CANCELED)) {
		return _dispatch_source_handler_dispose(dc);
	}
	if (dc->dc_flags & DISPATCH_OBJ_CTXT_FETCH_BIT) {
		dc->dc_ctxt = ds->do_ctxt;
	}
	_dispatch_continuation_pop(dc, cq, flags);
}

static void
_dispatch_source_latch_and_call(dispatch_source_t ds, dispatch_queue_t cq,
		dispatch_invoke_flags_t flags)
{
	unsigned long prev;

	dispatch_source_refs_t dr = ds->ds_refs;
	dispatch_continuation_t dc = _dispatch_source_get_handler(dr, DS_EVENT_HANDLER);
	prev = os_atomic_xchg2o(ds, ds_pending_data, 0, relaxed);
	if (ds->ds_is_level) {
		ds->ds_data = ~prev;
	} else if (ds->ds_is_timer && ds_timer(dr).target && prev) {
		ds->ds_data = _dispatch_source_timer_data(dr, prev);
	} else {
		ds->ds_data = prev;
	}
	if (!dispatch_assume(prev) || !dc) {
		return;
	}
	_dispatch_continuation_pop(dc, cq, flags);
	if (ds->ds_is_timer && (ds_timer(dr).flags & DISPATCH_TIMER_AFTER)) {
		_dispatch_source_handler_free(ds, DS_EVENT_HANDLER);
		dispatch_release(ds); // dispatch_after sources are one-shot
	}
}

static void
_dispatch_source_kevent_unregister(dispatch_source_t ds)
{
	_dispatch_object_debug(ds, "%s", __func__);
	uint32_t flags = (uint32_t)ds->ds_pending_data_mask;
	dispatch_kevent_t dk = ds->ds_dkev;
	dispatch_queue_flags_t dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	if (ds->ds_is_custom_source) {
		ds->ds_dkev = NULL;
		goto done;
	}

	if (ds->ds_is_direct_kevent &&
			((dqf & DSF_DELETED) || !(ds->ds_is_installed))) {
		dk->dk_kevent.flags |= EV_DELETE; // already deleted
		dk->dk_kevent.flags &= ~(EV_ADD|EV_ENABLE|EV_VANISHED);
	}
	if (dk->dk_kevent.filter == DISPATCH_EVFILT_TIMER) {
		ds->ds_dkev = NULL;
		if (ds->ds_is_installed) {
			_dispatch_timers_unregister(ds, dk);
		}
	} else if (!ds->ds_is_direct_kevent) {
		ds->ds_dkev = NULL;
		dispatch_assert((bool)ds->ds_is_installed);
		TAILQ_REMOVE(&dk->dk_sources, ds->ds_refs, dr_list);
		_dispatch_kevent_unregister(dk, flags, 0);
	} else {
		unsigned int dkev_dispose_options = 0;
		if (ds->ds_needs_rearm && !(dqf & DSF_ARMED)) {
			dkev_dispose_options |= DKEV_DISPOSE_IMMEDIATE_DELETE;
		} else if (dx_type(ds) == DISPATCH_MACH_CHANNEL_TYPE) {
			if (!ds->ds_is_direct_kevent) {
				dkev_dispose_options |= DKEV_DISPOSE_IMMEDIATE_DELETE;
			}
		}
		long r = _dispatch_kevent_unregister(dk, flags, dkev_dispose_options);
		if (r == EINPROGRESS) {
			_dispatch_debug("kevent-source[%p]: deferred delete kevent[%p]",
					ds, dk);
			_dispatch_queue_atomic_flags_set(ds->_as_dq, DSF_DEFERRED_DELETE);
			return; // deferred unregistration
#if DISPATCH_KEVENT_TREAT_ENOENT_AS_EINPROGRESS
		} else if (r == ENOENT) {
			_dispatch_debug("kevent-source[%p]: ENOENT delete kevent[%p]",
					ds, dk);
			_dispatch_queue_atomic_flags_set(ds->_as_dq, DSF_DEFERRED_DELETE);
			return; // potential concurrent EV_DELETE delivery rdar://22047283
#endif
		} else {
			dispatch_assume_zero(r);
		}
		ds->ds_dkev = NULL;
		_TAILQ_TRASH_ENTRY(ds->ds_refs, dr_list);
	}
done:
	dqf = _dispatch_queue_atomic_flags_set_and_clear_orig(ds->_as_dq,
			DSF_DELETED, DSF_ARMED | DSF_DEFERRED_DELETE | DSF_CANCEL_WAITER);
	if (dqf & DSF_CANCEL_WAITER) {
		_dispatch_wake_by_address(&ds->dq_atomic_flags);
	}
	ds->ds_is_installed = true;
	ds->ds_needs_rearm = false; // re-arm is pointless and bad now
	_dispatch_debug("kevent-source[%p]: disarmed kevent[%p]", ds, dk);
	_dispatch_release(ds); // the retain is done at creation time
}

DISPATCH_ALWAYS_INLINE
static bool
_dispatch_source_tryarm(dispatch_source_t ds)
{
	dispatch_queue_flags_t oqf, nqf;
	return os_atomic_rmw_loop2o(ds, dq_atomic_flags, oqf, nqf, relaxed, {
		if (oqf & (DSF_DEFERRED_DELETE | DSF_DELETED)) {
			// the test is inside the loop because it's convenient but the
			// result should not change for the duration of the rmw_loop
			os_atomic_rmw_loop_give_up(break);
		}
		nqf = oqf | DSF_ARMED;
	});
}

static bool
_dispatch_source_kevent_resume(dispatch_source_t ds, uint32_t new_flags)
{
	switch (ds->ds_dkev->dk_kevent.filter) {
	case DISPATCH_EVFILT_TIMER:
		_dispatch_timers_update(ds);
		_dispatch_queue_atomic_flags_set(ds->_as_dq, DSF_ARMED);
		_dispatch_debug("kevent-source[%p]: rearmed kevent[%p]", ds,
				ds->ds_dkev);
		return true;
#if HAVE_MACH
	case EVFILT_MACHPORT:
		if ((ds->ds_pending_data_mask & DISPATCH_MACH_RECV_MESSAGE) &&
				!ds->ds_is_direct_kevent) {
			new_flags |= DISPATCH_MACH_RECV_MESSAGE; // emulate EV_DISPATCH
		}
		break;
#endif
	}
	if (unlikely(!_dispatch_source_tryarm(ds))) {
		return false;
	}
	if (unlikely(_dispatch_kevent_resume(ds->ds_dkev, new_flags, 0))) {
		_dispatch_queue_atomic_flags_set_and_clear(ds->_as_dq, DSF_DELETED,
				DSF_ARMED);
		return false;
	}
	_dispatch_debug("kevent-source[%p]: armed kevent[%p]", ds, ds->ds_dkev);
	return true;
}

static void
_dispatch_source_kevent_register(dispatch_source_t ds, pthread_priority_t pp)
{
	dispatch_assert_zero((bool)ds->ds_is_installed);
	switch (ds->ds_dkev->dk_kevent.filter) {
	case DISPATCH_EVFILT_TIMER:
		_dispatch_timers_update(ds);
		_dispatch_queue_atomic_flags_set(ds->_as_dq, DSF_ARMED);
		_dispatch_debug("kevent-source[%p]: armed kevent[%p]", ds, ds->ds_dkev);
		return;
	}
	uint32_t flags;
	bool do_resume = _dispatch_kevent_register(&ds->ds_dkev, pp, &flags);
	TAILQ_INSERT_TAIL(&ds->ds_dkev->dk_sources, ds->ds_refs, dr_list);
	ds->ds_is_installed = true;
	if (do_resume || ds->ds_needs_rearm) {
		if (unlikely(!_dispatch_source_kevent_resume(ds, flags))) {
			_dispatch_source_kevent_unregister(ds);
		}
	} else {
		_dispatch_queue_atomic_flags_set(ds->_as_dq, DSF_ARMED);
	}
	_dispatch_object_debug(ds, "%s", __func__);
}

static void
_dispatch_source_set_event_handler_context(void *ctxt)
{
	dispatch_source_t ds = ctxt;
	dispatch_continuation_t dc = _dispatch_source_get_event_handler(ds->ds_refs);

	if (dc && (dc->dc_flags & DISPATCH_OBJ_CTXT_FETCH_BIT)) {
		dc->dc_ctxt = ds->do_ctxt;
	}
}

static pthread_priority_t
_dispatch_source_compute_kevent_priority(dispatch_source_t ds)
{
	pthread_priority_t p = ds->dq_priority & ~_PTHREAD_PRIORITY_FLAGS_MASK;
	dispatch_queue_t tq = ds->do_targetq;
	pthread_priority_t tqp = tq->dq_priority & ~_PTHREAD_PRIORITY_FLAGS_MASK;

	while (unlikely(tq->do_targetq)) {
		if (unlikely(tq == &_dispatch_mgr_q)) {
			return _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;
		}
		if (unlikely(_dispatch_queue_is_thread_bound(tq))) {
			// thread bound hierarchies are weird, we need to install
			// from the context of the thread this hierarchy is bound to
			return 0;
		}
		if (unlikely(DISPATCH_QUEUE_IS_SUSPENDED(tq))) {
			// this queue may not be activated yet, so the queue graph may not
			// have stabilized yet
			_dispatch_ktrace1(DISPATCH_PERF_delayed_registration, ds);
			return 0;
		}
		if (unlikely(!_dispatch_queue_has_immutable_target(tq))) {
			if (!_dispatch_is_in_root_queues_array(tq->do_targetq)) {
				// we're not allowed to dereference tq->do_targetq
				_dispatch_ktrace1(DISPATCH_PERF_delayed_registration, ds);
				return 0;
			}
		}
		if (!(tq->dq_priority & _PTHREAD_PRIORITY_INHERIT_FLAG)) {
			if (p < tqp) p = tqp;
		}
		tq = tq->do_targetq;
		tqp = tq->dq_priority & ~_PTHREAD_PRIORITY_FLAGS_MASK;
	}

	if (unlikely(!tqp)) {
		// pthread root queues opt out of QoS
		return 0;
	}
	return _dispatch_priority_inherit_from_root_queue(p, tq);
}

void
_dispatch_source_finalize_activation(dispatch_source_t ds)
{
	dispatch_continuation_t dc;

	if (unlikely(ds->ds_is_direct_kevent &&
			(_dispatch_queue_atomic_flags(ds->_as_dq) & DSF_CANCELED))) {
		return _dispatch_source_kevent_unregister(ds);
	}

	dc = _dispatch_source_get_event_handler(ds->ds_refs);
	if (dc) {
		if (_dispatch_object_is_barrier(dc)) {
			_dispatch_queue_atomic_flags_set(ds->_as_dq, DQF_BARRIER_BIT);
		}
		ds->dq_priority = dc->dc_priority & ~_PTHREAD_PRIORITY_FLAGS_MASK;
		if (dc->dc_flags & DISPATCH_OBJ_CTXT_FETCH_BIT) {
			_dispatch_barrier_async_detached_f(ds->_as_dq, ds,
					_dispatch_source_set_event_handler_context);
		}
	}

	// call "super"
	_dispatch_queue_finalize_activation(ds->_as_dq);

	if (ds->ds_is_direct_kevent && !ds->ds_is_installed) {
		pthread_priority_t pp = _dispatch_source_compute_kevent_priority(ds);
		if (pp) _dispatch_source_kevent_register(ds, pp);
	}
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_t
_dispatch_source_invoke2(dispatch_object_t dou, dispatch_invoke_flags_t flags,
		uint64_t *owned, struct dispatch_object_s **dc_ptr DISPATCH_UNUSED)
{
	dispatch_source_t ds = dou._ds;
	dispatch_queue_t retq = NULL;
	dispatch_queue_t dq = _dispatch_queue_get_current();

	if (_dispatch_queue_class_probe(ds)) {
		// Intentionally always drain even when on the manager queue
		// and not the source's regular target queue: we need to be able
		// to drain timer setting and the like there.
		retq = _dispatch_queue_serial_drain(ds->_as_dq, flags, owned, NULL);
	}

	// This function performs all source actions. Each action is responsible
	// for verifying that it takes place on the appropriate queue. If the
	// current queue is not the correct queue for this action, the correct queue
	// will be returned and the invoke will be re-driven on that queue.

	// The order of tests here in invoke and in wakeup should be consistent.

	dispatch_source_refs_t dr = ds->ds_refs;
	dispatch_queue_t dkq = &_dispatch_mgr_q;

	if (ds->ds_is_direct_kevent) {
		dkq = ds->do_targetq;
	}

	if (!ds->ds_is_installed) {
		// The source needs to be installed on the kevent queue.
		if (dq != dkq) {
			return dkq;
		}
		_dispatch_source_kevent_register(ds, _dispatch_get_defaultpriority());
	}

	if (unlikely(DISPATCH_QUEUE_IS_SUSPENDED(ds))) {
		// Source suspended by an item drained from the source queue.
		return ds->do_targetq;
	}

	if (_dispatch_source_get_registration_handler(dr)) {
		// The source has been registered and the registration handler needs
		// to be delivered on the target queue.
		if (dq != ds->do_targetq) {
			return ds->do_targetq;
		}
		// clears ds_registration_handler
		_dispatch_source_registration_callout(ds, dq, flags);
	}

	dispatch_queue_flags_t dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	bool prevent_starvation = false;

	if ((dqf & DSF_DEFERRED_DELETE) &&
			((dqf & DSF_DELETED) || !(dqf & DSF_ARMED))) {
unregister_event:
		// DSF_DELETE: Pending source kevent unregistration has been completed
		// !DSF_ARMED: event was delivered and can safely be unregistered
		if (dq != dkq) {
			return dkq;
		}
		_dispatch_source_kevent_unregister(ds);
		dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	}

	if (!(dqf & (DSF_CANCELED | DQF_RELEASED)) && ds->ds_pending_data) {
		// The source has pending data to deliver via the event handler callback
		// on the target queue. Some sources need to be rearmed on the kevent
		// queue after event delivery.
		if (dq == ds->do_targetq) {
			_dispatch_source_latch_and_call(ds, dq, flags);
			dqf = _dispatch_queue_atomic_flags(ds->_as_dq);

			// starvation avoidance: if the source triggers itself then force a
			// re-queue to give other things already queued on the target queue
			// a chance to run.
			//
			// however, if the source is directly targetting an overcommit root
			// queue, this would requeue the source and ask for a new overcommit
			// thread right away.
			prevent_starvation = dq->do_targetq ||
					!(dq->dq_priority & _PTHREAD_PRIORITY_OVERCOMMIT_FLAG);
			if (prevent_starvation && ds->ds_pending_data) {
				retq = ds->do_targetq;
			}
		} else {
			// there is no point trying to be eager, the next thing to do is
			// to deliver the event
			return ds->do_targetq;
		}
	}

	if ((dqf & (DSF_CANCELED | DQF_RELEASED)) && !(dqf & DSF_DEFERRED_DELETE)) {
		// The source has been cancelled and needs to be uninstalled from the
		// kevent queue. After uninstallation, the cancellation handler needs
		// to be delivered to the target queue.
		if (!(dqf & DSF_DELETED)) {
			if (dq != dkq) {
				return dkq;
			}
			_dispatch_source_kevent_unregister(ds);
			dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
			if (unlikely(dqf & DSF_DEFERRED_DELETE)) {
				if (!(dqf & DSF_ARMED)) {
					goto unregister_event;
				}
				// we need to wait for the EV_DELETE
				return retq;
			}
		}
		if (dq != ds->do_targetq && (_dispatch_source_get_event_handler(dr) ||
				_dispatch_source_get_cancel_handler(dr) ||
				_dispatch_source_get_registration_handler(dr))) {
			retq = ds->do_targetq;
		} else {
			_dispatch_source_cancel_callout(ds, dq, flags);
			dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
		}
		prevent_starvation = false;
	}

	if (ds->ds_needs_rearm && !(dqf & DSF_ARMED)) {
		// The source needs to be rearmed on the kevent queue.
		if (dq != dkq) {
			return dkq;
		}
		if (unlikely(dqf & DSF_DEFERRED_DELETE)) {
			// no need for resume when we can directly unregister the kevent
			goto unregister_event;
		}
		if (prevent_starvation) {
			// keep the old behavior to force re-enqueue to our target queue
			// for the rearm. It is inefficient though and we should
			// improve this <rdar://problem/24635615>.
			//
			// if the handler didn't run, or this is a pending delete
			// or our target queue is a global queue, then starvation is
			// not a concern and we can rearm right away.
			return ds->do_targetq;
		}
		if (unlikely(!_dispatch_source_kevent_resume(ds, 0))) {
			dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
			goto unregister_event;
		}
	}

	return retq;
}

DISPATCH_NOINLINE
void
_dispatch_source_invoke(dispatch_source_t ds, dispatch_invoke_flags_t flags)
{
	_dispatch_queue_class_invoke(ds->_as_dq, flags, _dispatch_source_invoke2);
}

void
_dispatch_source_wakeup(dispatch_source_t ds, pthread_priority_t pp,
		dispatch_wakeup_flags_t flags)
{
	// This function determines whether the source needs to be invoked.
	// The order of tests here in wakeup and in invoke should be consistent.

	dispatch_source_refs_t dr = ds->ds_refs;
	dispatch_queue_wakeup_target_t dkq = DISPATCH_QUEUE_WAKEUP_MGR;
	dispatch_queue_wakeup_target_t tq = DISPATCH_QUEUE_WAKEUP_NONE;
	dispatch_queue_flags_t dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	bool deferred_delete = (dqf & DSF_DEFERRED_DELETE);

	if (ds->ds_is_direct_kevent) {
		dkq = DISPATCH_QUEUE_WAKEUP_TARGET;
	}

	if (!ds->ds_is_installed) {
		// The source needs to be installed on the kevent queue.
		tq = dkq;
	} else if (_dispatch_source_get_registration_handler(dr)) {
		// The registration handler needs to be delivered to the target queue.
		tq = DISPATCH_QUEUE_WAKEUP_TARGET;
	} else if (deferred_delete && ((dqf & DSF_DELETED) || !(dqf & DSF_ARMED))) {
		// Pending source kevent unregistration has been completed
		// or EV_ONESHOT event can be acknowledged
		tq = dkq;
	} else if (!(dqf & (DSF_CANCELED | DQF_RELEASED)) && ds->ds_pending_data) {
		// The source has pending data to deliver to the target queue.
		tq = DISPATCH_QUEUE_WAKEUP_TARGET;
	} else if ((dqf & (DSF_CANCELED | DQF_RELEASED)) && !deferred_delete) {
		// The source needs to be uninstalled from the kevent queue, or the
		// cancellation handler needs to be delivered to the target queue.
		// Note: cancellation assumes installation.
		if (!(dqf & DSF_DELETED)) {
			tq = dkq;
		} else if (_dispatch_source_get_event_handler(dr) ||
				_dispatch_source_get_cancel_handler(dr) ||
				_dispatch_source_get_registration_handler(dr)) {
			tq = DISPATCH_QUEUE_WAKEUP_TARGET;
		}
	} else if (ds->ds_needs_rearm && !(dqf & DSF_ARMED)) {
		// The source needs to be rearmed on the kevent queue.
		tq = dkq;
	}
	if (!tq && _dispatch_queue_class_probe(ds)) {
		tq = DISPATCH_QUEUE_WAKEUP_TARGET;
	}

	if (tq) {
		return _dispatch_queue_class_wakeup(ds->_as_dq, pp, flags, tq);
	} else if (pp) {
		return _dispatch_queue_class_override_drainer(ds->_as_dq, pp, flags);
	} else if (flags & DISPATCH_WAKEUP_CONSUME) {
		return _dispatch_release_tailcall(ds);
	}
}

void
dispatch_source_cancel(dispatch_source_t ds)
{
	_dispatch_object_debug(ds, "%s", __func__);
	// Right after we set the cancel flag, someone else
	// could potentially invoke the source, do the cancellation,
	// unregister the source, and deallocate it. We would
	// need to therefore retain/release before setting the bit
	_dispatch_retain(ds);

	dispatch_queue_t q = ds->_as_dq;
	if (_dispatch_queue_atomic_flags_set_orig(q, DSF_CANCELED) & DSF_CANCELED) {
		_dispatch_release_tailcall(ds);
	} else {
		dx_wakeup(ds, 0, DISPATCH_WAKEUP_FLUSH | DISPATCH_WAKEUP_CONSUME);
	}
}

void
dispatch_source_cancel_and_wait(dispatch_source_t ds)
{
	dispatch_queue_flags_t old_dqf, dqf, new_dqf;
	pthread_priority_t pp;

	if (unlikely(_dispatch_source_get_cancel_handler(ds->ds_refs))) {
		DISPATCH_CLIENT_CRASH(ds, "Source has a cancel handler");
	}

	_dispatch_object_debug(ds, "%s", __func__);
	os_atomic_rmw_loop2o(ds, dq_atomic_flags, old_dqf, new_dqf, relaxed, {
		new_dqf = old_dqf | DSF_CANCELED;
		if (old_dqf & DSF_CANCEL_WAITER) {
			os_atomic_rmw_loop_give_up(break);
		}
		if ((old_dqf & DSF_STATE_MASK) == DSF_DELETED) {
			// just add DSF_CANCELED
		} else if ((old_dqf & DSF_DEFERRED_DELETE) || !ds->ds_is_direct_kevent){
			new_dqf |= DSF_CANCEL_WAITER;
		}
	});
	dqf = new_dqf;

	if (old_dqf & DQF_RELEASED) {
		DISPATCH_CLIENT_CRASH(ds, "Dispatch source used after last release");
	}
	if ((old_dqf & DSF_STATE_MASK) == DSF_DELETED) {
		return;
	}
	if (dqf & DSF_CANCEL_WAITER) {
		goto override;
	}

	// simplified version of _dispatch_queue_drain_try_lock
	// that also sets the DIRTY bit on failure to lock
	dispatch_lock_owner tid_self = _dispatch_tid_self();
	uint64_t xor_owner_and_set_full_width = tid_self |
			DISPATCH_QUEUE_WIDTH_FULL_BIT | DISPATCH_QUEUE_IN_BARRIER;
	uint64_t old_state, new_state;

	os_atomic_rmw_loop2o(ds, dq_state, old_state, new_state, seq_cst, {
		new_state = old_state;
		if (likely(_dq_state_is_runnable(old_state) &&
				!_dq_state_drain_locked(old_state))) {
			new_state &= DISPATCH_QUEUE_DRAIN_PRESERVED_BITS_MASK;
			new_state ^= xor_owner_and_set_full_width;
		} else if (old_dqf & DSF_CANCELED) {
			os_atomic_rmw_loop_give_up(break);
		} else {
			// this case needs a release barrier, hence the seq_cst above
			new_state |= DISPATCH_QUEUE_DIRTY;
		}
	});

	if (unlikely(_dq_state_is_suspended(old_state))) {
		if (unlikely(_dq_state_suspend_cnt(old_state))) {
			DISPATCH_CLIENT_CRASH(ds, "Source is suspended");
		}
		// inactive sources have never been registered and there is no need
		// to wait here because activation will notice and mark the source
		// as deleted without ever trying to use the fd or mach port.
		return dispatch_activate(ds);
	}

	if (likely(_dq_state_is_runnable(old_state) &&
			!_dq_state_drain_locked(old_state))) {
		// same thing _dispatch_source_invoke2() does when handling cancellation
		dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
		if (!(dqf & (DSF_DEFERRED_DELETE | DSF_DELETED))) {
			_dispatch_source_kevent_unregister(ds);
			dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
			if (likely((dqf & DSF_STATE_MASK) == DSF_DELETED)) {
				_dispatch_source_cancel_callout(ds, NULL, DISPATCH_INVOKE_NONE);
			}
		}
		_dispatch_try_lock_transfer_or_wakeup(ds->_as_dq);
	} else if (unlikely(_dq_state_drain_locked_by(old_state, tid_self))) {
		DISPATCH_CLIENT_CRASH(ds, "dispatch_source_cancel_and_wait "
				"called from a source handler");
	} else {
override:
		pp = _dispatch_get_priority() & _PTHREAD_PRIORITY_QOS_CLASS_MASK;
		if (pp) dx_wakeup(ds, pp, DISPATCH_WAKEUP_OVERRIDING);
		dispatch_activate(ds);
	}

	dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	while (unlikely((dqf & DSF_STATE_MASK) != DSF_DELETED)) {
		if (unlikely(!(dqf & DSF_CANCEL_WAITER))) {
			if (!os_atomic_cmpxchgvw2o(ds, dq_atomic_flags,
					dqf, dqf | DSF_CANCEL_WAITER, &dqf, relaxed)) {
				continue;
			}
			dqf |= DSF_CANCEL_WAITER;
		}
		_dispatch_wait_on_address(&ds->dq_atomic_flags, dqf, DLOCK_LOCK_NONE);
		dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	}
}

static void
_dispatch_source_merge_kevent(dispatch_source_t ds,
		const _dispatch_kevent_qos_s *ke)
{
	_dispatch_object_debug(ds, "%s", __func__);
	dispatch_wakeup_flags_t flags = 0;
	dispatch_queue_flags_t dqf;
	pthread_priority_t pp = 0;

	if (ds->ds_needs_rearm || (ke->flags & (EV_DELETE | EV_ONESHOT))) {
		// once we modify the queue atomic flags below, it will allow concurrent
		// threads running _dispatch_source_invoke2 to dispose of the source,
		// so we can't safely borrow the reference we get from the knote udata
		// anymore, and need our own
		flags = DISPATCH_WAKEUP_CONSUME;
		_dispatch_retain(ds); // rdar://20382435
	}

	if ((ke->flags & EV_UDATA_SPECIFIC) && (ke->flags & EV_ONESHOT) &&
			!(ke->flags & EV_DELETE)) {
		dqf = _dispatch_queue_atomic_flags_set_and_clear(ds->_as_dq,
				DSF_DEFERRED_DELETE, DSF_ARMED);
		if (ke->flags & EV_VANISHED) {
			_dispatch_bug_kevent_client("kevent", _evfiltstr(ke->filter),
					"monitored resource vanished before the source "
					"cancel handler was invoked", 0);
		}
		_dispatch_debug("kevent-source[%p]: %s kevent[%p]", ds,
				(ke->flags & EV_VANISHED) ? "vanished" :
				"deferred delete oneshot", (void*)ke->udata);
	} else if ((ke->flags & EV_DELETE) || (ke->flags & EV_ONESHOT)) {
		dqf = _dispatch_queue_atomic_flags_set_and_clear(ds->_as_dq,
				DSF_DELETED, DSF_ARMED);
		_dispatch_debug("kevent-source[%p]: delete kevent[%p]",
				ds, (void*)ke->udata);
		if (ke->flags & EV_DELETE) goto done;
	} else if (ds->ds_needs_rearm) {
		dqf = _dispatch_queue_atomic_flags_clear(ds->_as_dq, DSF_ARMED);
		_dispatch_debug("kevent-source[%p]: disarmed kevent[%p] ",
				ds, (void*)ke->udata);
	} else {
		dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	}

	if (dqf & (DSF_CANCELED | DQF_RELEASED)) {
		goto done; // rdar://20204025
	}
#if HAVE_MACH
	if (ke->filter == EVFILT_MACHPORT &&
			dx_type(ds) == DISPATCH_MACH_CHANNEL_TYPE) {
		DISPATCH_INTERNAL_CRASH(ke->flags,"Unexpected kevent for mach channel");
	}
#endif

	unsigned long data;
	if ((ke->flags & EV_UDATA_SPECIFIC) && (ke->flags & EV_ONESHOT) &&
			(ke->flags & EV_VANISHED)) {
		// if the resource behind the ident vanished, the event handler can't
		// do anything useful anymore, so do not try to call it at all
		//
		// Note: if the kernel doesn't support EV_VANISHED we always get it
		// back unchanged from the flags passed at EV_ADD (registration) time
		// Since we never ask for both EV_ONESHOT and EV_VANISHED for sources,
		// if we get both bits it was a real EV_VANISHED delivery
		os_atomic_store2o(ds, ds_pending_data, 0, relaxed);
#if HAVE_MACH
	} else if (ke->filter == EVFILT_MACHPORT) {
		data = DISPATCH_MACH_RECV_MESSAGE;
		os_atomic_store2o(ds, ds_pending_data, data, relaxed);
#endif
	} else if (ds->ds_is_level) {
		// ke->data is signed and "negative available data" makes no sense
		// zero bytes happens when EV_EOF is set
		dispatch_assert(ke->data >= 0l);
		data = ~(unsigned long)ke->data;
		os_atomic_store2o(ds, ds_pending_data, data, relaxed);
	} else if (ds->ds_is_adder) {
		data = (unsigned long)ke->data;
		os_atomic_add2o(ds, ds_pending_data, data, relaxed);
	} else if (ke->fflags & ds->ds_pending_data_mask) {
		data = ke->fflags & ds->ds_pending_data_mask;
		os_atomic_or2o(ds, ds_pending_data, data, relaxed);
	}

done:
#if DISPATCH_USE_KEVENT_QOS
	pp = ((pthread_priority_t)ke->qos) & ~_PTHREAD_PRIORITY_FLAGS_MASK;
#endif
	dx_wakeup(ds, pp, flags | DISPATCH_WAKEUP_FLUSH);
}

#pragma mark -
#pragma mark dispatch_kevent_t

#if DISPATCH_USE_GUARDED_FD_CHANGE_FDGUARD
static void _dispatch_kevent_guard(dispatch_kevent_t dk);
static void _dispatch_kevent_unguard(dispatch_kevent_t dk);
#else
static inline void _dispatch_kevent_guard(dispatch_kevent_t dk) { (void)dk; }
static inline void _dispatch_kevent_unguard(dispatch_kevent_t dk) { (void)dk; }
#endif

#if !DISPATCH_USE_EV_UDATA_SPECIFIC
static struct dispatch_kevent_s _dispatch_kevent_data_or = {
	.dk_kevent = {
		.filter = DISPATCH_EVFILT_CUSTOM_OR,
		.flags = EV_CLEAR,
	},
	.dk_sources = TAILQ_HEAD_INITIALIZER(_dispatch_kevent_data_or.dk_sources),
};
static struct dispatch_kevent_s _dispatch_kevent_data_add = {
	.dk_kevent = {
		.filter = DISPATCH_EVFILT_CUSTOM_ADD,
	},
	.dk_sources = TAILQ_HEAD_INITIALIZER(_dispatch_kevent_data_add.dk_sources),
};
#endif // !DISPATCH_USE_EV_UDATA_SPECIFIC

#define DSL_HASH(x) ((x) & (DSL_HASH_SIZE - 1))

DISPATCH_CACHELINE_ALIGN
static TAILQ_HEAD(, dispatch_kevent_s) _dispatch_sources[DSL_HASH_SIZE];

static void
_dispatch_kevent_init()
{
	unsigned int i;
	for (i = 0; i < DSL_HASH_SIZE; i++) {
		TAILQ_INIT(&_dispatch_sources[i]);
	}

#if !DISPATCH_USE_EV_UDATA_SPECIFIC
	TAILQ_INSERT_TAIL(&_dispatch_sources[0],
			&_dispatch_kevent_data_or, dk_list);
	TAILQ_INSERT_TAIL(&_dispatch_sources[0],
			&_dispatch_kevent_data_add, dk_list);
	_dispatch_kevent_data_or.dk_kevent.udata =
			(_dispatch_kevent_qos_udata_t)&_dispatch_kevent_data_or;
	_dispatch_kevent_data_add.dk_kevent.udata =
			(_dispatch_kevent_qos_udata_t)&_dispatch_kevent_data_add;
#endif // !DISPATCH_USE_EV_UDATA_SPECIFIC
}

static inline uintptr_t
_dispatch_kevent_hash(uint64_t ident, short filter)
{
	uint64_t value;
#if HAVE_MACH
	value = (filter == EVFILT_MACHPORT ||
			filter == DISPATCH_EVFILT_MACH_NOTIFICATION ?
			MACH_PORT_INDEX(ident) : ident);
#else
	value = ident;
	(void)filter;
#endif
	return DSL_HASH((uintptr_t)value);
}

static dispatch_kevent_t
_dispatch_kevent_find(uint64_t ident, short filter)
{
	uintptr_t hash = _dispatch_kevent_hash(ident, filter);
	dispatch_kevent_t dki;

	TAILQ_FOREACH(dki, &_dispatch_sources[hash], dk_list) {
		if (dki->dk_kevent.ident == ident && dki->dk_kevent.filter == filter) {
			break;
		}
	}
	return dki;
}

static void
_dispatch_kevent_insert(dispatch_kevent_t dk)
{
	if (dk->dk_kevent.flags & EV_UDATA_SPECIFIC) return;
	_dispatch_kevent_guard(dk);
	uintptr_t hash = _dispatch_kevent_hash(dk->dk_kevent.ident,
			dk->dk_kevent.filter);
	TAILQ_INSERT_TAIL(&_dispatch_sources[hash], dk, dk_list);
}

// Find existing kevents, and merge any new flags if necessary
static bool
_dispatch_kevent_register(dispatch_kevent_t *dkp, pthread_priority_t pp,
		uint32_t *flgp)
{
	dispatch_kevent_t dk = NULL, ds_dkev = *dkp;
	uint32_t new_flags;
	bool do_resume = false;

	if (!(ds_dkev->dk_kevent.flags & EV_UDATA_SPECIFIC)) {
		dk = _dispatch_kevent_find(ds_dkev->dk_kevent.ident,
				ds_dkev->dk_kevent.filter);
	}
	if (dk) {
		// If an existing dispatch kevent is found, check to see if new flags
		// need to be added to the existing kevent
		new_flags = ~dk->dk_kevent.fflags & ds_dkev->dk_kevent.fflags;
		dk->dk_kevent.fflags |= ds_dkev->dk_kevent.fflags;
		free(ds_dkev);
		*dkp = dk;
		do_resume = new_flags;
	} else {
		dk = ds_dkev;
#if DISPATCH_USE_KEVENT_WORKQUEUE
		if (!_dispatch_kevent_workqueue_enabled) {
			// do nothing
		} else if (!(dk->dk_kevent.flags & EV_UDATA_SPECIFIC)) {
			dk->dk_kevent.qos = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;
		} else {
			pp &= (~_PTHREAD_PRIORITY_FLAGS_MASK |
					_PTHREAD_PRIORITY_OVERCOMMIT_FLAG);
			if (!pp) pp = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;
			_dispatch_assert_is_valid_qos_class(pp);
			dk->dk_kevent.qos = (_dispatch_kevent_priority_t)pp;
		}
#else
		(void)pp;
#endif
		_dispatch_kevent_insert(dk);
		new_flags = dk->dk_kevent.fflags;
		do_resume = true;
	}
	// Re-register the kevent with the kernel if new flags were added
	// by the dispatch kevent
	if (do_resume) {
		dk->dk_kevent.flags |= EV_ADD;
	}
	*flgp = new_flags;
	return do_resume;
}

static long
_dispatch_kevent_resume(dispatch_kevent_t dk, uint32_t new_flags,
		uint32_t del_flags)
{
	long r;
	bool oneshot;
	if (dk->dk_kevent.flags & EV_DELETE) {
		return 0;
	}
	switch (dk->dk_kevent.filter) {
	case DISPATCH_EVFILT_TIMER:
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
		// these types not registered with kevent
		return 0;
#if HAVE_MACH
	case DISPATCH_EVFILT_MACH_NOTIFICATION:
		return _dispatch_kevent_mach_notify_resume(dk, new_flags, del_flags);
#if DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK
	case EVFILT_MACHPORT:
		if (!(dk->dk_kevent.flags & EV_UDATA_SPECIFIC)) {
			return _dispatch_kevent_machport_resume(dk, new_flags, del_flags);
		}
		// fall through
#endif
#endif // HAVE_MACH
	default:
		// oneshot dk may be freed by the time we return from
		// _dispatch_kq_immediate_update if the event was delivered (and then
		// unregistered) concurrently.
		oneshot = (dk->dk_kevent.flags & EV_ONESHOT);
		r = _dispatch_kq_immediate_update(&dk->dk_kevent);
		if (r && (dk->dk_kevent.flags & EV_ADD) &&
				(dk->dk_kevent.flags & EV_UDATA_SPECIFIC)) {
			dk->dk_kevent.flags |= EV_DELETE;
			dk->dk_kevent.flags &= ~(EV_ADD|EV_ENABLE|EV_VANISHED);
		} else if (!oneshot && (dk->dk_kevent.flags & EV_DISPATCH)) {
			// we can safely skip doing this for ONESHOT events because
			// the next kq update we will do is _dispatch_kevent_dispose()
			// which also clears EV_ADD.
			dk->dk_kevent.flags &= ~(EV_ADD|EV_VANISHED);
		}
		return r;
	}
	(void)new_flags; (void)del_flags;
}

static long
_dispatch_kevent_dispose(dispatch_kevent_t dk, unsigned int options)
{
	long r = 0;
	switch (dk->dk_kevent.filter) {
	case DISPATCH_EVFILT_TIMER:
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
		if (dk->dk_kevent.flags & EV_UDATA_SPECIFIC) {
			free(dk);
		} else {
			// these sources live on statically allocated lists
		}
		return r;
	}
	if (!(dk->dk_kevent.flags & EV_DELETE)) {
		dk->dk_kevent.flags |= EV_DELETE;
		dk->dk_kevent.flags &= ~(EV_ADD|EV_ENABLE|EV_VANISHED);
		if (options & DKEV_DISPOSE_IMMEDIATE_DELETE) {
			dk->dk_kevent.flags |= EV_ENABLE;
		}
		switch (dk->dk_kevent.filter) {
#if HAVE_MACH
		case DISPATCH_EVFILT_MACH_NOTIFICATION:
			r = _dispatch_kevent_mach_notify_resume(dk, 0,dk->dk_kevent.fflags);
			break;
#if DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK
		case EVFILT_MACHPORT:
			if (!(dk->dk_kevent.flags & EV_UDATA_SPECIFIC)) {
				r = _dispatch_kevent_machport_resume(dk,0,dk->dk_kevent.fflags);
				break;
			}
			// fall through
#endif
#endif
		default:
			if (options & DKEV_DISPOSE_IMMEDIATE_DELETE) {
				_dispatch_kq_deferred_update(&dk->dk_kevent);
			} else {
				r = _dispatch_kq_immediate_update(&dk->dk_kevent);
			}
			break;
		}
		if (options & DKEV_DISPOSE_IMMEDIATE_DELETE) {
			dk->dk_kevent.flags &= ~EV_ENABLE;
		}
	}
	if (dk->dk_kevent.flags & EV_UDATA_SPECIFIC) {
		bool deferred_delete = (r == EINPROGRESS);
#if DISPATCH_KEVENT_TREAT_ENOENT_AS_EINPROGRESS
		if (r == ENOENT) deferred_delete = true;
#endif
		if (deferred_delete) {
			// deferred EV_DELETE or concurrent concurrent EV_DELETE delivery
			dk->dk_kevent.flags &= ~EV_DELETE;
			dk->dk_kevent.flags |= EV_ENABLE;
			return r;
		}
	} else {
		uintptr_t hash = _dispatch_kevent_hash(dk->dk_kevent.ident,
				dk->dk_kevent.filter);
		TAILQ_REMOVE(&_dispatch_sources[hash], dk, dk_list);
	}
	_dispatch_kevent_unguard(dk);
	free(dk);
	return r;
}

static long
_dispatch_kevent_unregister(dispatch_kevent_t dk, uint32_t flg,
		unsigned int options)
{
	dispatch_source_refs_t dri;
	uint32_t del_flags, fflags = 0;
	long r = 0;

	if (TAILQ_EMPTY(&dk->dk_sources) ||
			(dk->dk_kevent.flags & EV_UDATA_SPECIFIC)) {
		r = _dispatch_kevent_dispose(dk, options);
	} else {
		TAILQ_FOREACH(dri, &dk->dk_sources, dr_list) {
			dispatch_source_t dsi = _dispatch_source_from_refs(dri);
			uint32_t mask = (uint32_t)dsi->ds_pending_data_mask;
			fflags |= mask;
		}
		del_flags = flg & ~fflags;
		if (del_flags) {
			dk->dk_kevent.flags |= EV_ADD;
			dk->dk_kevent.fflags &= ~del_flags;
			r = _dispatch_kevent_resume(dk, 0, del_flags);
		}
	}
	return r;
}

DISPATCH_NOINLINE
static void
_dispatch_kevent_proc_exit(_dispatch_kevent_qos_s *ke)
{
	// EVFILT_PROC may fail with ESRCH when the process exists but is a zombie
	// <rdar://problem/5067725>. As a workaround, we simulate an exit event for
	// any EVFILT_PROC with an invalid pid <rdar://problem/6626350>.
	_dispatch_kevent_qos_s fake;
	fake = *ke;
	fake.flags &= ~EV_ERROR;
	fake.flags |= EV_ONESHOT;
	fake.fflags = NOTE_EXIT;
	fake.data = 0;
	_dispatch_kevent_debug("synthetic NOTE_EXIT", ke);
	_dispatch_kevent_merge(&fake);
}

DISPATCH_NOINLINE
static void
_dispatch_kevent_error(_dispatch_kevent_qos_s *ke)
{
	_dispatch_kevent_qos_s *kev = NULL;

	if (ke->flags & EV_DELETE) {
		if (ke->flags & EV_UDATA_SPECIFIC) {
			if (ke->data == EINPROGRESS) {
				// deferred EV_DELETE
				return;
			}
#if DISPATCH_KEVENT_TREAT_ENOENT_AS_EINPROGRESS
			if (ke->data == ENOENT) {
				// deferred EV_DELETE
				return;
			}
#endif
		}
		// for EV_DELETE if the update was deferred we may have reclaimed
		// our dispatch_kevent_t, and it is unsafe to dereference it now.
	} else if (ke->udata) {
		kev = &((dispatch_kevent_t)ke->udata)->dk_kevent;
		ke->flags |= kev->flags;
	}

#if HAVE_MACH
	if (ke->filter == EVFILT_MACHPORT && ke->data == ENOTSUP &&
			(ke->flags & EV_ADD) && _dispatch_evfilt_machport_direct_enabled &&
			kev && (kev->fflags & MACH_RCV_MSG)) {
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

static void
_dispatch_kevent_drain(_dispatch_kevent_qos_s *ke)
{
#if DISPATCH_DEBUG
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_kevent_debugger);
#endif
	if (ke->filter == EVFILT_USER) {
		_dispatch_kevent_mgr_debug(ke);
		return;
	}
	if (slowpath(ke->flags & EV_ERROR)) {
		if (ke->filter == EVFILT_PROC && ke->data == ESRCH) {
			_dispatch_debug("kevent[0x%llx]: ESRCH from EVFILT_PROC: "
					"generating fake NOTE_EXIT", (unsigned long long)ke->udata);
			return _dispatch_kevent_proc_exit(ke);
		}
		_dispatch_debug("kevent[0x%llx]: handling error",
				(unsigned long long)ke->udata);
		return _dispatch_kevent_error(ke);
	}
	if (ke->filter == EVFILT_TIMER) {
		_dispatch_debug("kevent[0x%llx]: handling timer",
				(unsigned long long)ke->udata);
		return _dispatch_timers_kevent(ke);
	}
#if HAVE_MACH
	if (ke->filter == EVFILT_MACHPORT) {
		_dispatch_debug("kevent[0x%llx]: handling mach port",
				(unsigned long long)ke->udata);
		return _dispatch_mach_kevent_merge(ke);
	}
#endif
	return _dispatch_kevent_merge(ke);
}

DISPATCH_NOINLINE
static void
_dispatch_kevent_merge(_dispatch_kevent_qos_s *ke)
{
	dispatch_kevent_t dk = (void*)ke->udata;
	dispatch_source_refs_t dri, dr_next;

	TAILQ_FOREACH_SAFE(dri, &dk->dk_sources, dr_list, dr_next) {
		_dispatch_source_merge_kevent(_dispatch_source_from_refs(dri), ke);
	}
}

#if DISPATCH_USE_GUARDED_FD_CHANGE_FDGUARD
static void
_dispatch_kevent_guard(dispatch_kevent_t dk)
{
	guardid_t guard;
	const unsigned int guard_flags = GUARD_CLOSE;
	int r, fd_flags = 0;
	switch (dk->dk_kevent.filter) {
	case EVFILT_READ:
	case EVFILT_WRITE:
	case EVFILT_VNODE:
		guard = &dk->dk_kevent;
		r = change_fdguard_np((int)dk->dk_kevent.ident, NULL, 0,
				&guard, guard_flags, &fd_flags);
		if (slowpath(r == -1)) {
			int err = errno;
			if (err != EPERM) {
				(void)dispatch_assume_zero(err);
			}
			return;
		}
		dk->dk_kevent.ext[0] = guard_flags;
		dk->dk_kevent.ext[1] = fd_flags;
		break;
	}
}

static void
_dispatch_kevent_unguard(dispatch_kevent_t dk)
{
	guardid_t guard;
	unsigned int guard_flags;
	int r, fd_flags;
	switch (dk->dk_kevent.filter) {
	case EVFILT_READ:
	case EVFILT_WRITE:
	case EVFILT_VNODE:
		guard_flags = (unsigned int)dk->dk_kevent.ext[0];
		if (!guard_flags) {
			return;
		}
		guard = &dk->dk_kevent;
		fd_flags = (int)dk->dk_kevent.ext[1];
		r = change_fdguard_np((int)dk->dk_kevent.ident, &guard,
				guard_flags, NULL, 0, &fd_flags);
		if (slowpath(r == -1)) {
			(void)dispatch_assume_zero(errno);
			return;
		}
		dk->dk_kevent.ext[0] = 0;
		break;
	}
}
#endif // DISPATCH_USE_GUARDED_FD_CHANGE_FDGUARD

#pragma mark -
#pragma mark dispatch_source_timer

#if DISPATCH_USE_DTRACE
static dispatch_source_refs_t
		_dispatch_trace_next_timer[DISPATCH_TIMER_QOS_COUNT];
#define _dispatch_trace_next_timer_set(x, q) \
		_dispatch_trace_next_timer[(q)] = (x)
#define _dispatch_trace_next_timer_program(d, q) \
		_dispatch_trace_timer_program(_dispatch_trace_next_timer[(q)], (d))
#define _dispatch_trace_next_timer_wake(q) \
		_dispatch_trace_timer_wake(_dispatch_trace_next_timer[(q)])
#else
#define _dispatch_trace_next_timer_set(x, q)
#define _dispatch_trace_next_timer_program(d, q)
#define _dispatch_trace_next_timer_wake(q)
#endif

#define _dispatch_source_timer_telemetry_enabled() false

DISPATCH_NOINLINE
static void
_dispatch_source_timer_telemetry_slow(dispatch_source_t ds,
		uintptr_t ident, struct dispatch_timer_source_s *values)
{
	if (_dispatch_trace_timer_configure_enabled()) {
		_dispatch_trace_timer_configure(ds, ident, values);
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_source_timer_telemetry(dispatch_source_t ds, uintptr_t ident,
		struct dispatch_timer_source_s *values)
{
	if (_dispatch_trace_timer_configure_enabled() ||
			_dispatch_source_timer_telemetry_enabled()) {
		_dispatch_source_timer_telemetry_slow(ds, ident, values);
		asm(""); // prevent tailcall
	}
}

// approx 1 year (60s * 60m * 24h * 365d)
#define FOREVER_NSEC 31536000000000000ull

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dispatch_source_timer_now(uint64_t nows[], unsigned int tidx)
{
	unsigned int tk = DISPATCH_TIMER_KIND(tidx);
	if (nows && fastpath(nows[tk] != 0)) {
		return nows[tk];
	}
	uint64_t now;
	switch (tk) {
	case DISPATCH_TIMER_KIND_MACH:
		now = _dispatch_absolute_time();
		break;
	case DISPATCH_TIMER_KIND_WALL:
		now = _dispatch_get_nanoseconds();
		break;
	}
	if (nows) {
		nows[tk] = now;
	}
	return now;
}

static inline unsigned long
_dispatch_source_timer_data(dispatch_source_refs_t dr, unsigned long prev)
{
	// calculate the number of intervals since last fire
	unsigned long data, missed;
	uint64_t now;
	now = _dispatch_source_timer_now(NULL, _dispatch_source_timer_idx(dr));
	missed = (unsigned long)((now - ds_timer(dr).last_fire) /
			ds_timer(dr).interval);
	// correct for missed intervals already delivered last time
	data = prev - ds_timer(dr).missed + missed;
	ds_timer(dr).missed = missed;
	return data;
}

struct dispatch_set_timer_params {
	dispatch_source_t ds;
	uintptr_t ident;
	struct dispatch_timer_source_s values;
};

static void
_dispatch_source_set_timer3(void *context)
{
	// Called on the _dispatch_mgr_q
	struct dispatch_set_timer_params *params = context;
	dispatch_source_t ds = params->ds;
	ds->ds_ident_hack = params->ident;
	ds_timer(ds->ds_refs) = params->values;
	// Clear any pending data that might have accumulated on
	// older timer params <rdar://problem/8574886>
	ds->ds_pending_data = 0;
	// Re-arm in case we got disarmed because of pending set_timer suspension
	_dispatch_queue_atomic_flags_set(ds->_as_dq, DSF_ARMED);
	_dispatch_debug("kevent-source[%p]: rearmed kevent[%p]", ds, ds->ds_dkev);
	dispatch_resume(ds);
	// Must happen after resume to avoid getting disarmed due to suspension
	_dispatch_timers_update(ds);
	dispatch_release(ds);
	if (params->values.flags & DISPATCH_TIMER_WALL_CLOCK) {
		_dispatch_mach_host_calendar_change_register();
	}
	free(params);
}

static void
_dispatch_source_set_timer2(void *context)
{
	// Called on the source queue
	struct dispatch_set_timer_params *params = context;
	dispatch_suspend(params->ds);
	_dispatch_barrier_async_detached_f(&_dispatch_mgr_q, params,
			_dispatch_source_set_timer3);
}

DISPATCH_NOINLINE
static struct dispatch_set_timer_params *
_dispatch_source_timer_params(dispatch_source_t ds, dispatch_time_t start,
		uint64_t interval, uint64_t leeway)
{
	struct dispatch_set_timer_params *params;
	params = _dispatch_calloc(1ul, sizeof(struct dispatch_set_timer_params));
	params->ds = ds;
	params->values.flags = ds_timer(ds->ds_refs).flags;

	if (interval == 0) {
		// we use zero internally to mean disabled
		interval = 1;
	} else if ((int64_t)interval < 0) {
		// 6866347 - make sure nanoseconds won't overflow
		interval = INT64_MAX;
	}
	if ((int64_t)leeway < 0) {
		leeway = INT64_MAX;
	}
	if (start == DISPATCH_TIME_NOW) {
		start = _dispatch_absolute_time();
	} else if (start == DISPATCH_TIME_FOREVER) {
		start = INT64_MAX;
	}

	if ((int64_t)start < 0) {
		// wall clock
		start = (dispatch_time_t)-((int64_t)start);
		params->values.flags |= DISPATCH_TIMER_WALL_CLOCK;
	} else {
		// absolute clock
		interval = _dispatch_time_nano2mach(interval);
		if (interval < 1) {
			// rdar://problem/7287561 interval must be at least one in
			// in order to avoid later division by zero when calculating
			// the missed interval count. (NOTE: the wall clock's
			// interval is already "fixed" to be 1 or more)
			interval = 1;
		}
		leeway = _dispatch_time_nano2mach(leeway);
		params->values.flags &= ~(unsigned long)DISPATCH_TIMER_WALL_CLOCK;
	}
	params->ident = DISPATCH_TIMER_IDENT(params->values.flags);
	params->values.target = start;
	params->values.deadline = (start < UINT64_MAX - leeway) ?
			start + leeway : UINT64_MAX;
	params->values.interval = interval;
	params->values.leeway = (interval == INT64_MAX || leeway < interval / 2) ?
			leeway : interval / 2;
	return params;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_source_set_timer(dispatch_source_t ds, dispatch_time_t start,
		uint64_t interval, uint64_t leeway, bool source_sync)
{
	if (slowpath(!ds->ds_is_timer) ||
			slowpath(ds_timer(ds->ds_refs).flags & DISPATCH_TIMER_INTERVAL)) {
		DISPATCH_CLIENT_CRASH(ds, "Attempt to set timer on a non-timer source");
	}

	struct dispatch_set_timer_params *params;
	params = _dispatch_source_timer_params(ds, start, interval, leeway);

	_dispatch_source_timer_telemetry(ds, params->ident, &params->values);
	// Suspend the source so that it doesn't fire with pending changes
	// The use of suspend/resume requires the external retain/release
	dispatch_retain(ds);
	if (source_sync) {
		return _dispatch_barrier_trysync_or_async_f(ds->_as_dq, params,
				_dispatch_source_set_timer2);
	} else {
		return _dispatch_source_set_timer2(params);
	}
}

void
dispatch_source_set_timer(dispatch_source_t ds, dispatch_time_t start,
		uint64_t interval, uint64_t leeway)
{
	_dispatch_source_set_timer(ds, start, interval, leeway, true);
}

void
_dispatch_source_set_runloop_timer_4CF(dispatch_source_t ds,
		dispatch_time_t start, uint64_t interval, uint64_t leeway)
{
	// Don't serialize through the source queue for CF timers <rdar://13833190>
	_dispatch_source_set_timer(ds, start, interval, leeway, false);
}

void
_dispatch_source_set_interval(dispatch_source_t ds, uint64_t interval)
{
	dispatch_source_refs_t dr = ds->ds_refs;
	#define NSEC_PER_FRAME (NSEC_PER_SEC/60)
	const bool animation = ds_timer(dr).flags & DISPATCH_INTERVAL_UI_ANIMATION;
	if (fastpath(interval <= (animation ? FOREVER_NSEC/NSEC_PER_FRAME :
			FOREVER_NSEC/NSEC_PER_MSEC))) {
		interval *= animation ? NSEC_PER_FRAME : NSEC_PER_MSEC;
	} else {
		interval = FOREVER_NSEC;
	}
	interval = _dispatch_time_nano2mach(interval);
	uint64_t target = _dispatch_absolute_time() + interval;
	target = (target / interval) * interval;
	const uint64_t leeway = animation ?
			_dispatch_time_nano2mach(NSEC_PER_FRAME) : interval / 2;
	ds_timer(dr).target = target;
	ds_timer(dr).deadline = target + leeway;
	ds_timer(dr).interval = interval;
	ds_timer(dr).leeway = leeway;
	_dispatch_source_timer_telemetry(ds, ds->ds_ident_hack, &ds_timer(dr));
}

#pragma mark -
#pragma mark dispatch_timers

#define DISPATCH_TIMER_STRUCT(refs) \
	uint64_t target, deadline; \
	TAILQ_HEAD(, refs) dt_sources

typedef struct dispatch_timer_s {
	DISPATCH_TIMER_STRUCT(dispatch_timer_source_refs_s);
} *dispatch_timer_t;

#define DISPATCH_TIMER_INITIALIZER(tidx) \
	[tidx] = { \
		.target = UINT64_MAX, \
		.deadline = UINT64_MAX, \
		.dt_sources = TAILQ_HEAD_INITIALIZER( \
				_dispatch_timer[tidx].dt_sources), \
	}
#define DISPATCH_TIMER_INIT(kind, qos) \
		DISPATCH_TIMER_INITIALIZER(DISPATCH_TIMER_INDEX( \
		DISPATCH_TIMER_KIND_##kind, DISPATCH_TIMER_QOS_##qos))

struct dispatch_timer_s _dispatch_timer[] =  {
	DISPATCH_TIMER_INIT(WALL, NORMAL),
	DISPATCH_TIMER_INIT(WALL, CRITICAL),
	DISPATCH_TIMER_INIT(WALL, BACKGROUND),
	DISPATCH_TIMER_INIT(MACH, NORMAL),
	DISPATCH_TIMER_INIT(MACH, CRITICAL),
	DISPATCH_TIMER_INIT(MACH, BACKGROUND),
};
#define DISPATCH_TIMER_COUNT \
		((sizeof(_dispatch_timer) / sizeof(_dispatch_timer[0])))

#if __linux__
#define DISPATCH_KEVENT_TIMER_UDATA(tidx) \
		(void*)&_dispatch_kevent_timer[tidx]
#else
#define DISPATCH_KEVENT_TIMER_UDATA(tidx) \
		(uintptr_t)&_dispatch_kevent_timer[tidx]
#endif
#ifdef __LP64__
#define DISPATCH_KEVENT_TIMER_UDATA_INITIALIZER(tidx) \
		.udata = DISPATCH_KEVENT_TIMER_UDATA(tidx)
#else // __LP64__
// dynamic initialization in _dispatch_timers_init()
#define DISPATCH_KEVENT_TIMER_UDATA_INITIALIZER(tidx) \
		.udata = 0
#endif // __LP64__
#define DISPATCH_KEVENT_TIMER_INITIALIZER(tidx) \
	[tidx] = { \
		.dk_kevent = { \
			.ident = tidx, \
			.filter = DISPATCH_EVFILT_TIMER, \
			DISPATCH_KEVENT_TIMER_UDATA_INITIALIZER(tidx), \
		}, \
		.dk_sources = TAILQ_HEAD_INITIALIZER( \
				_dispatch_kevent_timer[tidx].dk_sources), \
	}
#define DISPATCH_KEVENT_TIMER_INIT(kind, qos) \
		DISPATCH_KEVENT_TIMER_INITIALIZER(DISPATCH_TIMER_INDEX( \
		DISPATCH_TIMER_KIND_##kind, DISPATCH_TIMER_QOS_##qos))

struct dispatch_kevent_s _dispatch_kevent_timer[] = {
	DISPATCH_KEVENT_TIMER_INIT(WALL, NORMAL),
	DISPATCH_KEVENT_TIMER_INIT(WALL, CRITICAL),
	DISPATCH_KEVENT_TIMER_INIT(WALL, BACKGROUND),
	DISPATCH_KEVENT_TIMER_INIT(MACH, NORMAL),
	DISPATCH_KEVENT_TIMER_INIT(MACH, CRITICAL),
	DISPATCH_KEVENT_TIMER_INIT(MACH, BACKGROUND),
	DISPATCH_KEVENT_TIMER_INITIALIZER(DISPATCH_TIMER_INDEX_DISARM),
};
#define DISPATCH_KEVENT_TIMER_COUNT \
		((sizeof(_dispatch_kevent_timer) / sizeof(_dispatch_kevent_timer[0])))

#define DISPATCH_KEVENT_TIMEOUT_IDENT_MASK (~0ull << 8)
#define DISPATCH_KEVENT_TIMEOUT_INITIALIZER(tidx, note) \
	[tidx] = { \
		.ident = DISPATCH_KEVENT_TIMEOUT_IDENT_MASK|(tidx), \
		.filter = EVFILT_TIMER, \
		.flags = EV_ONESHOT, \
		.fflags = NOTE_ABSOLUTE|NOTE_NSECONDS|NOTE_LEEWAY|(note), \
	}
#define DISPATCH_KEVENT_TIMEOUT_INIT(kind, qos, note) \
		DISPATCH_KEVENT_TIMEOUT_INITIALIZER(DISPATCH_TIMER_INDEX( \
		DISPATCH_TIMER_KIND_##kind, DISPATCH_TIMER_QOS_##qos), note)

_dispatch_kevent_qos_s _dispatch_kevent_timeout[] = {
	DISPATCH_KEVENT_TIMEOUT_INIT(WALL, NORMAL, NOTE_MACH_CONTINUOUS_TIME),
	DISPATCH_KEVENT_TIMEOUT_INIT(WALL, CRITICAL, NOTE_MACH_CONTINUOUS_TIME | NOTE_CRITICAL),
	DISPATCH_KEVENT_TIMEOUT_INIT(WALL, BACKGROUND, NOTE_MACH_CONTINUOUS_TIME | NOTE_BACKGROUND),
	DISPATCH_KEVENT_TIMEOUT_INIT(MACH, NORMAL, 0),
	DISPATCH_KEVENT_TIMEOUT_INIT(MACH, CRITICAL, NOTE_CRITICAL),
	DISPATCH_KEVENT_TIMEOUT_INIT(MACH, BACKGROUND, NOTE_BACKGROUND),
};
#define DISPATCH_KEVENT_TIMEOUT_COUNT \
		((sizeof(_dispatch_kevent_timeout) / sizeof(_dispatch_kevent_timeout[0])))
#if __has_feature(c_static_assert)
_Static_assert(DISPATCH_KEVENT_TIMEOUT_COUNT == DISPATCH_TIMER_INDEX_COUNT - 1,
		"should have a kevent for everything but disarm (ddt assumes this)");
#endif

#define DISPATCH_KEVENT_COALESCING_WINDOW_INIT(qos, ms) \
		[DISPATCH_TIMER_QOS_##qos] = 2ull * (ms) * NSEC_PER_MSEC

static const uint64_t _dispatch_kevent_coalescing_window[] = {
	DISPATCH_KEVENT_COALESCING_WINDOW_INIT(NORMAL, 75),
	DISPATCH_KEVENT_COALESCING_WINDOW_INIT(CRITICAL, 1),
	DISPATCH_KEVENT_COALESCING_WINDOW_INIT(BACKGROUND, 100),
};

#define _dispatch_timers_insert(tidx, dra, dr, dr_list, dta, dt, dt_list) ({ \
	typeof(dr) dri = NULL; typeof(dt) dti; \
	if (tidx != DISPATCH_TIMER_INDEX_DISARM) { \
		TAILQ_FOREACH(dri, &dra[tidx].dk_sources, dr_list) { \
			if (ds_timer(dr).target < ds_timer(dri).target) { \
				break; \
			} \
		} \
		TAILQ_FOREACH(dti, &dta[tidx].dt_sources, dt_list) { \
			if (ds_timer(dt).deadline < ds_timer(dti).deadline) { \
				break; \
			} \
		} \
		if (dti) { \
			TAILQ_INSERT_BEFORE(dti, dt, dt_list); \
		} else { \
			TAILQ_INSERT_TAIL(&dta[tidx].dt_sources, dt, dt_list); \
		} \
	} \
	if (dri) { \
		TAILQ_INSERT_BEFORE(dri, dr, dr_list); \
	} else { \
		TAILQ_INSERT_TAIL(&dra[tidx].dk_sources, dr, dr_list); \
	} \
	})

#define _dispatch_timers_remove(tidx, dk, dra, dr, dr_list, dta, dt, dt_list) \
	({ \
	if (tidx != DISPATCH_TIMER_INDEX_DISARM) { \
		TAILQ_REMOVE(&dta[tidx].dt_sources, dt, dt_list); \
	} \
	TAILQ_REMOVE(dk ? &(*(dk)).dk_sources : &dra[tidx].dk_sources, dr, \
			dr_list); })

#define _dispatch_timers_check(dra, dta) ({ \
	unsigned int timerm = _dispatch_timers_mask; \
	bool update = false; \
	unsigned int tidx; \
	for (tidx = 0; tidx < DISPATCH_TIMER_COUNT; tidx++) { \
		if (!(timerm & (1 << tidx))){ \
			continue; \
		} \
		dispatch_timer_source_refs_t dr = (dispatch_timer_source_refs_t) \
				TAILQ_FIRST(&dra[tidx].dk_sources); \
		dispatch_timer_source_refs_t dt = (dispatch_timer_source_refs_t) \
				TAILQ_FIRST(&dta[tidx].dt_sources); \
		uint64_t target = dr ? ds_timer(dr).target : UINT64_MAX; \
		uint64_t deadline = dr ? ds_timer(dt).deadline : UINT64_MAX; \
		if (target != dta[tidx].target) { \
			dta[tidx].target = target; \
			update = true; \
		} \
		if (deadline != dta[tidx].deadline) { \
			dta[tidx].deadline = deadline; \
			update = true; \
		} \
	} \
	update; })

static bool _dispatch_timers_reconfigure, _dispatch_timer_expired;
static unsigned int _dispatch_timers_mask;
static bool _dispatch_timers_force_max_leeway;

static void
_dispatch_timers_init(void)
{
#ifndef __LP64__
	unsigned int tidx;
	for (tidx = 0; tidx < DISPATCH_TIMER_COUNT; tidx++) {
		_dispatch_kevent_timer[tidx].dk_kevent.udata =
				DISPATCH_KEVENT_TIMER_UDATA(tidx);
	}
#endif // __LP64__
	if (slowpath(getenv("LIBDISPATCH_TIMERS_FORCE_MAX_LEEWAY"))) {
		_dispatch_timers_force_max_leeway = true;
	}
}

static inline void
_dispatch_timers_unregister(dispatch_source_t ds, dispatch_kevent_t dk)
{
	dispatch_source_refs_t dr = ds->ds_refs;
	unsigned int tidx = (unsigned int)dk->dk_kevent.ident;

	if (slowpath(ds_timer_aggregate(ds))) {
		_dispatch_timer_aggregates_unregister(ds, tidx);
	}
	_dispatch_timers_remove(tidx, dk, _dispatch_kevent_timer, dr, dr_list,
			_dispatch_timer, (dispatch_timer_source_refs_t)dr, dt_list);
	if (tidx != DISPATCH_TIMER_INDEX_DISARM) {
		_dispatch_timers_reconfigure = true;
		_dispatch_timers_mask |= 1 << tidx;
	}
}

// Updates the ordered list of timers based on next fire date for changes to ds.
// Should only be called from the context of _dispatch_mgr_q.
static void
_dispatch_timers_update(dispatch_source_t ds)
{
	dispatch_kevent_t dk = ds->ds_dkev;
	dispatch_source_refs_t dr = ds->ds_refs;
	unsigned int tidx;

	DISPATCH_ASSERT_ON_MANAGER_QUEUE();

	// Do not reschedule timers unregistered with _dispatch_kevent_unregister()
	if (slowpath(!dk)) {
		return;
	}
	// Move timers that are disabled, suspended or have missed intervals to the
	// disarmed list, rearm after resume resp. source invoke will reenable them
	if (!ds_timer(dr).target || DISPATCH_QUEUE_IS_SUSPENDED(ds) ||
			ds->ds_pending_data) {
		tidx = DISPATCH_TIMER_INDEX_DISARM;
		_dispatch_queue_atomic_flags_clear(ds->_as_dq, DSF_ARMED);
		_dispatch_debug("kevent-source[%p]: disarmed kevent[%p]", ds,
				ds->ds_dkev);
	} else {
		tidx = _dispatch_source_timer_idx(dr);
	}
	if (slowpath(ds_timer_aggregate(ds))) {
		_dispatch_timer_aggregates_register(ds);
	}
	if (slowpath(!ds->ds_is_installed)) {
		ds->ds_is_installed = true;
		if (tidx != DISPATCH_TIMER_INDEX_DISARM) {
			_dispatch_queue_atomic_flags_set(ds->_as_dq, DSF_ARMED);
			_dispatch_debug("kevent-source[%p]: rearmed kevent[%p]", ds,
					ds->ds_dkev);
		}
		_dispatch_object_debug(ds, "%s", __func__);
		ds->ds_dkev = NULL;
		free(dk);
	} else {
		_dispatch_timers_unregister(ds, dk);
	}
	if (tidx != DISPATCH_TIMER_INDEX_DISARM) {
		_dispatch_timers_reconfigure = true;
		_dispatch_timers_mask |= 1 << tidx;
	}
	if (dk != &_dispatch_kevent_timer[tidx]){
		ds->ds_dkev = &_dispatch_kevent_timer[tidx];
	}
	_dispatch_timers_insert(tidx, _dispatch_kevent_timer, dr, dr_list,
			_dispatch_timer, (dispatch_timer_source_refs_t)dr, dt_list);
	if (slowpath(ds_timer_aggregate(ds))) {
		_dispatch_timer_aggregates_update(ds, tidx);
	}
}

static inline void
_dispatch_timers_run2(uint64_t nows[], unsigned int tidx)
{
	dispatch_source_refs_t dr;
	dispatch_source_t ds;
	uint64_t now, missed;

	now = _dispatch_source_timer_now(nows, tidx);
	while ((dr = TAILQ_FIRST(&_dispatch_kevent_timer[tidx].dk_sources))) {
		ds = _dispatch_source_from_refs(dr);
		// We may find timers on the wrong list due to a pending update from
		// dispatch_source_set_timer. Force an update of the list in that case.
		if (tidx != ds->ds_ident_hack) {
			_dispatch_timers_update(ds);
			continue;
		}
		if (!ds_timer(dr).target) {
			// No configured timers on the list
			break;
		}
		if (ds_timer(dr).target > now) {
			// Done running timers for now.
			break;
		}
		// Remove timers that are suspended or have missed intervals from the
		// list, rearm after resume resp. source invoke will reenable them
		if (DISPATCH_QUEUE_IS_SUSPENDED(ds) || ds->ds_pending_data) {
			_dispatch_timers_update(ds);
			continue;
		}
		// Calculate number of missed intervals.
		missed = (now - ds_timer(dr).target) / ds_timer(dr).interval;
		if (++missed > INT_MAX) {
			missed = INT_MAX;
		}
		if (ds_timer(dr).interval < INT64_MAX) {
			ds_timer(dr).target += missed * ds_timer(dr).interval;
			ds_timer(dr).deadline = ds_timer(dr).target + ds_timer(dr).leeway;
		} else {
			ds_timer(dr).target = UINT64_MAX;
			ds_timer(dr).deadline = UINT64_MAX;
		}
		_dispatch_timers_update(ds);
		ds_timer(dr).last_fire = now;

		unsigned long data;
		data = os_atomic_add2o(ds, ds_pending_data,
				(unsigned long)missed, relaxed);
		_dispatch_trace_timer_fire(dr, data, (unsigned long)missed);
		dx_wakeup(ds, 0, DISPATCH_WAKEUP_FLUSH);
		if (ds_timer(dr).flags & DISPATCH_TIMER_AFTER) {
			_dispatch_source_kevent_unregister(ds);
		}
	}
}

DISPATCH_NOINLINE
static void
_dispatch_timers_run(uint64_t nows[])
{
	unsigned int tidx;
	for (tidx = 0; tidx < DISPATCH_TIMER_COUNT; tidx++) {
		if (!TAILQ_EMPTY(&_dispatch_kevent_timer[tidx].dk_sources)) {
			_dispatch_timers_run2(nows, tidx);
		}
	}
}

static inline unsigned int
_dispatch_timers_get_delay(uint64_t nows[], struct dispatch_timer_s timer[],
		uint64_t *delay, uint64_t *leeway, int qos, int kind)
{
	unsigned int tidx, ridx = DISPATCH_TIMER_COUNT;
	uint64_t tmp, delta = UINT64_MAX, dldelta = UINT64_MAX;

	for (tidx = 0; tidx < DISPATCH_TIMER_COUNT; tidx++) {
		if (qos >= 0 && qos != DISPATCH_TIMER_QOS(tidx)){
			continue;
		}
		if (kind >= 0 && kind != DISPATCH_TIMER_KIND(tidx)){
			continue;
		}
		uint64_t target = timer[tidx].target;
		if (target == UINT64_MAX) {
			continue;
		}
		uint64_t deadline = timer[tidx].deadline;
		if (qos >= 0) {
			// Timer pre-coalescing <rdar://problem/13222034>
			uint64_t window = _dispatch_kevent_coalescing_window[qos];
			uint64_t latest = deadline > window ? deadline - window : 0;
			dispatch_source_refs_t dri;
			TAILQ_FOREACH(dri, &_dispatch_kevent_timer[tidx].dk_sources,
					dr_list) {
				tmp = ds_timer(dri).target;
				if (tmp > latest) break;
				target = tmp;
			}
		}
		uint64_t now = _dispatch_source_timer_now(nows, tidx);
		if (target <= now) {
			delta = 0;
			break;
		}
		tmp = target - now;
		if (DISPATCH_TIMER_KIND(tidx) != DISPATCH_TIMER_KIND_WALL) {
			tmp = _dispatch_time_mach2nano(tmp);
		}
		if (tmp < INT64_MAX && tmp < delta) {
			ridx = tidx;
			delta = tmp;
		}
		dispatch_assert(target <= deadline);
		tmp = deadline - now;
		if (DISPATCH_TIMER_KIND(tidx) != DISPATCH_TIMER_KIND_WALL) {
			tmp = _dispatch_time_mach2nano(tmp);
		}
		if (tmp < INT64_MAX && tmp < dldelta) {
			dldelta = tmp;
		}
	}
	*delay = delta;
	*leeway = delta && delta < UINT64_MAX ? dldelta - delta : UINT64_MAX;
	return ridx;
}


#ifdef __linux__
// in linux we map the _dispatch_kevent_qos_s  to struct kevent instead
// of struct kevent64. We loose the kevent.ext[] members and the time
// out is based on relavite msec based time vs. absolute nsec based time.
// For now we make the adjustments right here until the solution
// to either extend libkqueue with a proper kevent64 API or removing kevent
// all together and move to a lower API (e.g. epoll or kernel_module.
// Also leeway is ignored.

static void
_dispatch_kevent_timer_set_delay(_dispatch_kevent_qos_s *ke, uint64_t delay,
		uint64_t leeway, uint64_t nows[])
{
	// call to update nows[]
	_dispatch_source_timer_now(nows, DISPATCH_TIMER_KIND_WALL);
#ifdef KEVENT_NSEC_NOT_SUPPORTED
	// adjust nsec based delay to msec based and ignore leeway
	delay /= 1000000L;
	if ((int64_t)(delay) <= 0) {
		delay = 1; // if value <= 0 the dispatch will stop
	}
#else
	ke->fflags |= NOTE_NSECONDS;
#endif
	ke->data = (int64_t)delay;
}

#else
static void
_dispatch_kevent_timer_set_delay(_dispatch_kevent_qos_s *ke, uint64_t delay,
		uint64_t leeway, uint64_t nows[])
{
	delay += _dispatch_source_timer_now(nows, DISPATCH_TIMER_KIND_WALL);
	if (slowpath(_dispatch_timers_force_max_leeway)) {
		ke->data = (int64_t)(delay + leeway);
		ke->ext[1] = 0;
	} else {
		ke->data = (int64_t)delay;
		ke->ext[1] = leeway;
	}
}
#endif // __linux__

static bool
_dispatch_timers_program2(uint64_t nows[], _dispatch_kevent_qos_s *ke,
		unsigned int tidx)
{
	bool poll;
	uint64_t delay, leeway;

	_dispatch_timers_get_delay(nows, _dispatch_timer, &delay, &leeway,
			(int)DISPATCH_TIMER_QOS(tidx), (int)DISPATCH_TIMER_KIND(tidx));
	poll = (delay == 0);
	if (poll || delay == UINT64_MAX) {
		_dispatch_trace_next_timer_set(NULL, DISPATCH_TIMER_QOS(tidx));
		if (!ke->data) {
			return poll;
		}
		ke->data = 0;
		ke->flags |= EV_DELETE;
		ke->flags &= ~(EV_ADD|EV_ENABLE);
	} else {
		_dispatch_trace_next_timer_set(
				TAILQ_FIRST(&_dispatch_kevent_timer[tidx].dk_sources), DISPATCH_TIMER_QOS(tidx));
		_dispatch_trace_next_timer_program(delay, DISPATCH_TIMER_QOS(tidx));
		_dispatch_kevent_timer_set_delay(ke, delay, leeway, nows);
		ke->flags |= EV_ADD|EV_ENABLE;
		ke->flags &= ~EV_DELETE;
#if DISPATCH_USE_KEVENT_WORKQUEUE
		if (_dispatch_kevent_workqueue_enabled) {
			ke->qos = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;
		}
#endif
	}
	_dispatch_kq_deferred_update(ke);
	return poll;
}

DISPATCH_NOINLINE
static bool
_dispatch_timers_program(uint64_t nows[])
{
	bool poll = false;
	unsigned int tidx, timerm = _dispatch_timers_mask;
	for (tidx = 0; tidx < DISPATCH_KEVENT_TIMEOUT_COUNT; tidx++) {
		if (!(timerm & 1 << tidx)){
			continue;
		}
		poll |= _dispatch_timers_program2(nows, &_dispatch_kevent_timeout[tidx],
				tidx);
	}
	return poll;
}

DISPATCH_NOINLINE
static bool
_dispatch_timers_configure(void)
{
	_dispatch_timer_aggregates_check();
	// Find out if there is a new target/deadline on the timer lists
	return _dispatch_timers_check(_dispatch_kevent_timer, _dispatch_timer);
}

#if HAVE_MACH
static void
_dispatch_timers_calendar_change(void)
{
	unsigned int qos;

	// calendar change may have gone past the wallclock deadline
	_dispatch_timer_expired = true;
	for (qos = 0; qos < DISPATCH_TIMER_QOS_COUNT; qos++) {
		_dispatch_timers_mask |=
				1 << DISPATCH_TIMER_INDEX(DISPATCH_TIMER_KIND_WALL, qos);
	}
}
#endif

static void
_dispatch_timers_kevent(_dispatch_kevent_qos_s *ke)
{
	dispatch_assert(ke->data > 0);
	dispatch_assert((ke->ident & DISPATCH_KEVENT_TIMEOUT_IDENT_MASK) ==
			DISPATCH_KEVENT_TIMEOUT_IDENT_MASK);
	unsigned int tidx = ke->ident & ~DISPATCH_KEVENT_TIMEOUT_IDENT_MASK;
	dispatch_assert(tidx < DISPATCH_KEVENT_TIMEOUT_COUNT);
	dispatch_assert(_dispatch_kevent_timeout[tidx].data != 0);
	_dispatch_kevent_timeout[tidx].data = 0; // kevent deleted via EV_ONESHOT
	_dispatch_timer_expired = true;
	_dispatch_timers_mask |= 1 << tidx;
	_dispatch_trace_next_timer_wake(DISPATCH_TIMER_QOS(tidx));
}

static inline bool
_dispatch_mgr_timers(void)
{
	uint64_t nows[DISPATCH_TIMER_KIND_COUNT] = {};
	bool expired = slowpath(_dispatch_timer_expired);
	if (expired) {
		_dispatch_timers_run(nows);
	}
	bool reconfigure = slowpath(_dispatch_timers_reconfigure);
	if (reconfigure || expired) {
		if (reconfigure) {
			reconfigure = _dispatch_timers_configure();
			_dispatch_timers_reconfigure = false;
		}
		if (reconfigure || expired) {
			expired = _dispatch_timer_expired = _dispatch_timers_program(nows);
			expired = expired || _dispatch_mgr_q.dq_items_tail;
		}
		_dispatch_timers_mask = 0;
	}
	return expired;
}

#pragma mark -
#pragma mark dispatch_timer_aggregate

typedef struct {
	TAILQ_HEAD(, dispatch_timer_source_aggregate_refs_s) dk_sources;
} dispatch_timer_aggregate_refs_s;

typedef struct dispatch_timer_aggregate_s {
	DISPATCH_QUEUE_HEADER(queue);
	TAILQ_ENTRY(dispatch_timer_aggregate_s) dta_list;
	dispatch_timer_aggregate_refs_s
			dta_kevent_timer[DISPATCH_KEVENT_TIMER_COUNT];
	struct {
		DISPATCH_TIMER_STRUCT(dispatch_timer_source_aggregate_refs_s);
	} dta_timer[DISPATCH_TIMER_COUNT];
	struct dispatch_timer_s dta_timer_data[DISPATCH_TIMER_COUNT];
	unsigned int dta_refcount;
} DISPATCH_QUEUE_ALIGN dispatch_timer_aggregate_s;

typedef TAILQ_HEAD(, dispatch_timer_aggregate_s) dispatch_timer_aggregates_s;
static dispatch_timer_aggregates_s _dispatch_timer_aggregates =
		TAILQ_HEAD_INITIALIZER(_dispatch_timer_aggregates);

dispatch_timer_aggregate_t
dispatch_timer_aggregate_create(void)
{
	unsigned int tidx;
	dispatch_timer_aggregate_t dta = _dispatch_alloc(DISPATCH_VTABLE(queue),
			sizeof(struct dispatch_timer_aggregate_s));
	_dispatch_queue_init(dta->_as_dq, DQF_NONE,
			DISPATCH_QUEUE_WIDTH_MAX, false);
	dta->do_targetq = _dispatch_get_root_queue(
			_DISPATCH_QOS_CLASS_USER_INITIATED, true);
	//FIXME: aggregates need custom vtable
	//dta->dq_label = "timer-aggregate";
	for (tidx = 0; tidx < DISPATCH_KEVENT_TIMER_COUNT; tidx++) {
		TAILQ_INIT(&dta->dta_kevent_timer[tidx].dk_sources);
	}
	for (tidx = 0; tidx < DISPATCH_TIMER_COUNT; tidx++) {
		TAILQ_INIT(&dta->dta_timer[tidx].dt_sources);
		dta->dta_timer[tidx].target = UINT64_MAX;
		dta->dta_timer[tidx].deadline = UINT64_MAX;
		dta->dta_timer_data[tidx].target = UINT64_MAX;
		dta->dta_timer_data[tidx].deadline = UINT64_MAX;
	}
	return (dispatch_timer_aggregate_t)_dispatch_introspection_queue_create(
			dta->_as_dq);
}

typedef struct dispatch_timer_delay_s {
	dispatch_timer_t timer;
	uint64_t delay, leeway;
} *dispatch_timer_delay_t;

static void
_dispatch_timer_aggregate_get_delay(void *ctxt)
{
	dispatch_timer_delay_t dtd = ctxt;
	struct { uint64_t nows[DISPATCH_TIMER_KIND_COUNT]; } dtn = {};
	_dispatch_timers_get_delay(dtn.nows, dtd->timer, &dtd->delay, &dtd->leeway,
			-1, -1);
}

uint64_t
dispatch_timer_aggregate_get_delay(dispatch_timer_aggregate_t dta,
		uint64_t *leeway_ptr)
{
	struct dispatch_timer_delay_s dtd = {
		.timer = dta->dta_timer_data,
	};
	dispatch_sync_f(dta->_as_dq, &dtd, _dispatch_timer_aggregate_get_delay);
	if (leeway_ptr) {
		*leeway_ptr = dtd.leeway;
	}
	return dtd.delay;
}

static void
_dispatch_timer_aggregate_update(void *ctxt)
{
	dispatch_timer_aggregate_t dta = (void*)_dispatch_queue_get_current();
	dispatch_timer_t dtau = ctxt;
	unsigned int tidx;
	for (tidx = 0; tidx < DISPATCH_TIMER_COUNT; tidx++) {
		dta->dta_timer_data[tidx].target = dtau[tidx].target;
		dta->dta_timer_data[tidx].deadline = dtau[tidx].deadline;
	}
	free(dtau);
}

DISPATCH_NOINLINE
static void
_dispatch_timer_aggregates_configure(void)
{
	dispatch_timer_aggregate_t dta;
	dispatch_timer_t dtau;
	TAILQ_FOREACH(dta, &_dispatch_timer_aggregates, dta_list) {
		if (!_dispatch_timers_check(dta->dta_kevent_timer, dta->dta_timer)) {
			continue;
		}
		dtau = _dispatch_calloc(DISPATCH_TIMER_COUNT, sizeof(*dtau));
		memcpy(dtau, dta->dta_timer, sizeof(dta->dta_timer));
		_dispatch_barrier_async_detached_f(dta->_as_dq, dtau,
				_dispatch_timer_aggregate_update);
	}
}

static inline void
_dispatch_timer_aggregates_check(void)
{
	if (fastpath(TAILQ_EMPTY(&_dispatch_timer_aggregates))) {
		return;
	}
	_dispatch_timer_aggregates_configure();
}

static void
_dispatch_timer_aggregates_register(dispatch_source_t ds)
{
	dispatch_timer_aggregate_t dta = ds_timer_aggregate(ds);
	if (!dta->dta_refcount++) {
		TAILQ_INSERT_TAIL(&_dispatch_timer_aggregates, dta, dta_list);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_timer_aggregates_update(dispatch_source_t ds, unsigned int tidx)
{
	dispatch_timer_aggregate_t dta = ds_timer_aggregate(ds);
	dispatch_timer_source_aggregate_refs_t dr;
	dr = (dispatch_timer_source_aggregate_refs_t)ds->ds_refs;
	_dispatch_timers_insert(tidx, dta->dta_kevent_timer, dr, dra_list,
			dta->dta_timer, dr, dta_list);
}

DISPATCH_NOINLINE
static void
_dispatch_timer_aggregates_unregister(dispatch_source_t ds, unsigned int tidx)
{
	dispatch_timer_aggregate_t dta = ds_timer_aggregate(ds);
	dispatch_timer_source_aggregate_refs_t dr;
	dr = (dispatch_timer_source_aggregate_refs_t)ds->ds_refs;
	_dispatch_timers_remove(tidx, (dispatch_timer_aggregate_refs_s*)NULL,
			dta->dta_kevent_timer, dr, dra_list, dta->dta_timer, dr, dta_list);
	if (!--dta->dta_refcount) {
		TAILQ_REMOVE(&_dispatch_timer_aggregates, dta, dta_list);
	}
}

#pragma mark -
#pragma mark dispatch_kqueue

static int _dispatch_kq;

#if DISPATCH_DEBUG_QOS && DISPATCH_USE_KEVENT_WORKQUEUE
#define _dispatch_kevent_assert_valid_qos(ke)  ({ \
		if (_dispatch_kevent_workqueue_enabled) { \
			const _dispatch_kevent_qos_s *_ke = (ke); \
			if (_ke->flags & (EV_ADD|EV_ENABLE)) { \
				_dispatch_assert_is_valid_qos_class(\
						(pthread_priority_t)_ke->qos); \
				dispatch_assert(_ke->qos); \
			} \
		} \
	})
#else
#define _dispatch_kevent_assert_valid_qos(ke)  ((void)ke)
#endif


static void
_dispatch_kq_init(void *context DISPATCH_UNUSED)
{
	_dispatch_fork_becomes_unsafe();
#if DISPATCH_USE_KEVENT_WORKQUEUE
	_dispatch_kevent_workqueue_init();
	if (_dispatch_kevent_workqueue_enabled) {
		int r;
		const _dispatch_kevent_qos_s kev[] = {
			[0] = {
				.ident = 1,
				.filter = EVFILT_USER,
				.flags = EV_ADD|EV_CLEAR,
				.qos = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG,
			},
			[1] = {
				.ident = 1,
				.filter = EVFILT_USER,
				.fflags = NOTE_TRIGGER,
			},
		};
		_dispatch_kq = -1;
retry:
		r = kevent_qos(-1, kev, 2, NULL, 0, NULL, NULL,
				KEVENT_FLAG_WORKQ|KEVENT_FLAG_IMMEDIATE);
		if (slowpath(r == -1)) {
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
	static const _dispatch_kevent_qos_s kev = {
		.ident = 1,
		.filter = EVFILT_USER,
		.flags = EV_ADD|EV_CLEAR,
	};

	_dispatch_fork_becomes_unsafe();
#if DISPATCH_USE_GUARDED_FD
	guardid_t guard = (uintptr_t)&kev;
	_dispatch_kq = guarded_kqueue_np(&guard, GUARD_CLOSE | GUARD_DUP);
#else
	_dispatch_kq = kqueue();
#endif
	if (_dispatch_kq == -1) {
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
	(void)dispatch_assume_zero(kevent_qos(_dispatch_kq, &kev, 1, NULL, 0, NULL,
			NULL, 0));
	_dispatch_queue_push(_dispatch_mgr_q.do_targetq, &_dispatch_mgr_q, 0);
#endif // DISPATCH_USE_MGR_THREAD
}

DISPATCH_NOINLINE
static long
_dispatch_kq_update(const _dispatch_kevent_qos_s *ke, int n)
{
	int i, r;
	_dispatch_kevent_qos_s kev_error[n];
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_kq_init);

	for (i = 0; i < n; i++) {
		if (ke[i].filter != EVFILT_USER || DISPATCH_MGR_QUEUE_DEBUG) {
			_dispatch_kevent_debug_n("updating", ke + i, i, n);
		}
	}

	unsigned int flags = KEVENT_FLAG_ERROR_EVENTS;
#if DISPATCH_USE_KEVENT_WORKQUEUE
	if (_dispatch_kevent_workqueue_enabled) {
		flags |= KEVENT_FLAG_WORKQ;
	}
#endif

retry:
	r = kevent_qos(_dispatch_kq, ke, n, kev_error, n, NULL, NULL, flags);
	if (slowpath(r == -1)) {
		int err = errno;
		switch (err) {
		case EINTR:
			goto retry;
		case EBADF:
			DISPATCH_CLIENT_CRASH(err, "Do not close random Unix descriptors");
			break;
		default:
			(void)dispatch_assume_zero(err);
			break;
		}
		return err;
	}
	for (i = 0, n = r; i < n; i++) {
		if (kev_error[i].flags & EV_ERROR) {
			_dispatch_kevent_debug("returned error", &kev_error[i]);
			_dispatch_kevent_drain(&kev_error[i]);
			r = (int)kev_error[i].data;
		} else {
			_dispatch_kevent_mgr_debug(&kev_error[i]);
			r = 0;
		}
	}
	return r;
}

DISPATCH_ALWAYS_INLINE
static void
_dispatch_kq_update_all(const _dispatch_kevent_qos_s *kev, int n)
{
	(void)_dispatch_kq_update(kev, n);
}

DISPATCH_ALWAYS_INLINE
static long
_dispatch_kq_update_one(const _dispatch_kevent_qos_s *kev)
{
	return _dispatch_kq_update(kev, 1);
}

static inline bool
_dispatch_kevent_maps_to_same_knote(const _dispatch_kevent_qos_s *e1,
		const _dispatch_kevent_qos_s *e2)
{
	return e1->filter == e2->filter &&
			e1->ident == e2->ident &&
			e1->udata == e2->udata;
}

static inline int
_dispatch_deferred_event_find_slot(dispatch_deferred_items_t ddi,
		const _dispatch_kevent_qos_s *ke)
{
	_dispatch_kevent_qos_s *events = ddi->ddi_eventlist;
	int i;

	for (i = 0; i < ddi->ddi_nevents; i++) {
		if (_dispatch_kevent_maps_to_same_knote(&events[i], ke)) {
			break;
		}
	}
	return i;
}

static void
_dispatch_kq_deferred_update(const _dispatch_kevent_qos_s *ke)
{
	dispatch_deferred_items_t ddi = _dispatch_deferred_items_get();
	int slot;

	_dispatch_kevent_assert_valid_qos(ke);
	if (ddi) {
		if (unlikely(ddi->ddi_nevents == ddi->ddi_maxevents)) {
			_dispatch_deferred_items_set(NULL);
			_dispatch_kq_update_all(ddi->ddi_eventlist, ddi->ddi_nevents);
			ddi->ddi_nevents = 0;
			_dispatch_deferred_items_set(ddi);
		}
		if (ke->filter != EVFILT_USER || DISPATCH_MGR_QUEUE_DEBUG) {
			_dispatch_kevent_debug("deferred", ke);
		}
		bool needs_enable = false;
		slot = _dispatch_deferred_event_find_slot(ddi, ke);
		if (slot == ddi->ddi_nevents) {
			ddi->ddi_nevents++;
		} else if (ke->flags & EV_DELETE) {
			// <rdar://problem/26202376> when deleting and an enable is pending,
			// we must merge EV_ENABLE to do an immediate deletion
			needs_enable = (ddi->ddi_eventlist[slot].flags & EV_ENABLE);
		}
		ddi->ddi_eventlist[slot] = *ke;
		if (needs_enable) {
			ddi->ddi_eventlist[slot].flags |= EV_ENABLE;
		}
	} else {
		_dispatch_kq_update_one(ke);
	}
}

static long
_dispatch_kq_immediate_update(_dispatch_kevent_qos_s *ke)
{
	dispatch_deferred_items_t ddi = _dispatch_deferred_items_get();
	int slot, last;

	_dispatch_kevent_assert_valid_qos(ke);
	if (ddi) {
		_dispatch_kevent_qos_s *events = ddi->ddi_eventlist;
		slot = _dispatch_deferred_event_find_slot(ddi, ke);
		if (slot < ddi->ddi_nevents) {
			// <rdar://problem/26202376> when deleting and an enable is pending,
			// we must merge EV_ENABLE to do an immediate deletion
			if ((ke->flags & EV_DELETE) && (events[slot].flags & EV_ENABLE)) {
				ke->flags |= EV_ENABLE;
			}
			last = --ddi->ddi_nevents;
			if (slot != last) {
				events[slot] = events[last];
			}
		}
	}
	return _dispatch_kq_update_one(ke);
}

#pragma mark -
#pragma mark dispatch_mgr

DISPATCH_NOINLINE
static void
_dispatch_mgr_queue_poke(dispatch_queue_t dq DISPATCH_UNUSED,
		pthread_priority_t pp DISPATCH_UNUSED)
{
	static const _dispatch_kevent_qos_s kev = {
		.ident = 1,
		.filter = EVFILT_USER,
		.fflags = NOTE_TRIGGER,
	};

#if DISPATCH_DEBUG && DISPATCH_MGR_QUEUE_DEBUG
	_dispatch_debug("waking up the dispatch manager queue: %p", dq);
#endif
	_dispatch_kq_deferred_update(&kev);
}

void
_dispatch_mgr_queue_wakeup(dispatch_queue_t dq, pthread_priority_t pp,
		dispatch_wakeup_flags_t flags)
{
	if (flags & DISPATCH_WAKEUP_FLUSH) {
		os_atomic_or2o(dq, dq_state, DISPATCH_QUEUE_DIRTY, release);
	}

	if (_dispatch_queue_get_current() == &_dispatch_mgr_q) {
		return;
	}

	if (!_dispatch_queue_class_probe(&_dispatch_mgr_q)) {
		return;
	}

	_dispatch_mgr_queue_poke(dq, pp);
}

DISPATCH_NOINLINE
static void
_dispatch_event_init(void)
{
	_dispatch_kevent_init();
	_dispatch_timers_init();
#if DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK
	_dispatch_mach_recv_msg_buf_init();
#endif
	_dispatch_memorypressure_init();
	_voucher_activity_debug_channel_init();
}

#if DISPATCH_USE_MGR_THREAD
DISPATCH_NOINLINE
static void
_dispatch_mgr_init(void)
{
	uint64_t owned = DISPATCH_QUEUE_SERIAL_DRAIN_OWNED;
	_dispatch_queue_set_current(&_dispatch_mgr_q);
	if (_dispatch_queue_drain_try_lock(&_dispatch_mgr_q,
			DISPATCH_INVOKE_STEALING, NULL) != owned) {
		DISPATCH_INTERNAL_CRASH(0, "Locking the manager should not fail");
	}
	_dispatch_mgr_priority_init();
	_dispatch_event_init();
}

DISPATCH_NOINLINE
static bool
_dispatch_mgr_wait_for_event(dispatch_deferred_items_t ddi, bool poll)
{
	int r;
	dispatch_assert((size_t)ddi->ddi_maxevents < countof(ddi->ddi_eventlist));

retry:
	r = kevent_qos(_dispatch_kq, ddi->ddi_eventlist, ddi->ddi_nevents,
			ddi->ddi_eventlist + ddi->ddi_maxevents, 1, NULL, NULL,
			poll ? KEVENT_FLAG_IMMEDIATE : KEVENT_FLAG_NONE);
	if (slowpath(r == -1)) {
		int err = errno;
		switch (err) {
		case EINTR:
			goto retry;
		case EBADF:
			DISPATCH_CLIENT_CRASH(err, "Do not close random Unix descriptors");
			break;
		default:
			(void)dispatch_assume_zero(err);
			break;
		}
	}
	ddi->ddi_nevents = 0;
	return r > 0;
}

DISPATCH_NOINLINE DISPATCH_NORETURN
static void
_dispatch_mgr_invoke(void)
{
	dispatch_deferred_items_s ddi;
	bool poll;

	ddi.ddi_magic = DISPATCH_DEFERRED_ITEMS_MAGIC;
	ddi.ddi_stashed_pp = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;
	ddi.ddi_nevents = 0;
	ddi.ddi_maxevents = 1;

	_dispatch_deferred_items_set(&ddi);

	for (;;) {
		_dispatch_mgr_queue_drain();
		poll = _dispatch_mgr_timers();
		poll = poll || _dispatch_queue_class_probe(&_dispatch_mgr_q);
		if (_dispatch_mgr_wait_for_event(&ddi, poll)) {
			_dispatch_kevent_qos_s *ke = ddi.ddi_eventlist + ddi.ddi_maxevents;
			_dispatch_kevent_debug("received", ke);
			_dispatch_kevent_drain(ke);
		}
	}
}
#endif // DISPATCH_USE_MGR_THREAD

DISPATCH_NORETURN
void
_dispatch_mgr_thread(dispatch_queue_t dq DISPATCH_UNUSED,
		dispatch_invoke_flags_t flags DISPATCH_UNUSED)
{
#if DISPATCH_USE_KEVENT_WORKQUEUE
	if (_dispatch_kevent_workqueue_enabled) {
		DISPATCH_INTERNAL_CRASH(0, "Manager queue invoked with "
				"kevent workqueue enabled");
	}
#endif
#if DISPATCH_USE_MGR_THREAD
	_dispatch_mgr_init();
	// never returns, so burn bridges behind us & clear stack 2k ahead
	_dispatch_clear_stack(2048);
	_dispatch_mgr_invoke();
#endif
}

#if DISPATCH_USE_KEVENT_WORKQUEUE

#define DISPATCH_KEVENT_WORKER_IS_NOT_MANAGER ((pthread_priority_t)(~0ul))

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_kevent_worker_thread_init(dispatch_deferred_items_t ddi)
{
	uint64_t owned = DISPATCH_QUEUE_SERIAL_DRAIN_OWNED;

	ddi->ddi_magic = DISPATCH_DEFERRED_ITEMS_MAGIC;
	ddi->ddi_nevents = 0;
	ddi->ddi_maxevents = countof(ddi->ddi_eventlist);
	ddi->ddi_stashed_pp = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;

	pthread_priority_t pp = _dispatch_get_priority();
	if (!(pp & _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG)) {
		// If this thread does not have the event manager flag set, don't setup
		// as the dispatch manager and let the caller know to only process
		// the delivered events.
		//
		// Also add the NEEDS_UNBIND flag so that
		// _dispatch_priority_compute_update knows it has to unbind
		pp &= _PTHREAD_PRIORITY_OVERCOMMIT_FLAG | ~_PTHREAD_PRIORITY_FLAGS_MASK;
		pp |= _PTHREAD_PRIORITY_NEEDS_UNBIND_FLAG;
		_dispatch_thread_setspecific(dispatch_priority_key,
					(void *)(uintptr_t)pp);
		ddi->ddi_stashed_pp = 0;
		return DISPATCH_KEVENT_WORKER_IS_NOT_MANAGER;
	}

	if ((pp & _PTHREAD_PRIORITY_SCHED_PRI_FLAG) ||
			!(pp & ~_PTHREAD_PRIORITY_FLAGS_MASK)) {
		// When the phtread kext is delivering kevents to us, and pthread
		// root queues are in use, then the pthread priority TSD is set
		// to a sched pri with the _PTHREAD_PRIORITY_SCHED_PRI_FLAG bit set.
		//
		// Given that this isn't a valid QoS we need to fixup the TSD,
		// and the best option is to clear the qos/priority bits which tells
		// us to not do any QoS related calls on this thread.
		//
		// However, in that case the manager thread is opted out of QoS,
		// as far as pthread is concerned, and can't be turned into
		// something else, so we can't stash.
		pp &= (pthread_priority_t)_PTHREAD_PRIORITY_FLAGS_MASK;
	}
	// Managers always park without mutating to a regular worker thread, and
	// hence never need to unbind from userland, and when draining a manager,
	// the NEEDS_UNBIND flag would cause the mutation to happen.
	// So we need to strip this flag
	pp &= ~(pthread_priority_t)_PTHREAD_PRIORITY_NEEDS_UNBIND_FLAG;
	_dispatch_thread_setspecific(dispatch_priority_key, (void *)(uintptr_t)pp);

	// ensure kevents registered from this thread are registered at manager QoS
	pthread_priority_t old_dp = _dispatch_set_defaultpriority(
			(pthread_priority_t)_PTHREAD_PRIORITY_EVENT_MANAGER_FLAG, NULL);
	_dispatch_queue_set_current(&_dispatch_mgr_q);
	if (_dispatch_queue_drain_try_lock(&_dispatch_mgr_q,
			DISPATCH_INVOKE_STEALING, NULL) != owned) {
		DISPATCH_INTERNAL_CRASH(0, "Locking the manager should not fail");
	}
	static int event_thread_init;
	if (!event_thread_init) {
		event_thread_init = 1;
		_dispatch_event_init();
	}
	return old_dp;
}

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline bool
_dispatch_kevent_worker_thread_reset(pthread_priority_t old_dp)
{
	dispatch_queue_t dq = &_dispatch_mgr_q;
	uint64_t orig_dq_state;

	_dispatch_queue_drain_unlock(dq, DISPATCH_QUEUE_SERIAL_DRAIN_OWNED,
			&orig_dq_state);
	_dispatch_reset_defaultpriority(old_dp);
	_dispatch_queue_set_current(NULL);
	return _dq_state_is_dirty(orig_dq_state);
}

DISPATCH_NOINLINE
void
_dispatch_kevent_worker_thread(_dispatch_kevent_qos_s **events, int *nevents)
{
	_dispatch_introspection_thread_add();

	if (!events && !nevents) {
		// events for worker thread request have already been delivered earlier
		return;
	}

	_dispatch_kevent_qos_s *ke = *events;
	int n = *nevents;
	if (!dispatch_assume(n) || !dispatch_assume(*events)) return;

	dispatch_deferred_items_s ddi;
	pthread_priority_t old_dp = _dispatch_kevent_worker_thread_init(&ddi);

	_dispatch_deferred_items_set(&ddi);
	for (int i = 0; i < n; i++) {
		_dispatch_kevent_debug("received", ke);
		_dispatch_kevent_drain(ke++);
	}

	if (old_dp != DISPATCH_KEVENT_WORKER_IS_NOT_MANAGER) {
		_dispatch_mgr_queue_drain();
		bool poll = _dispatch_mgr_timers();
		if (_dispatch_kevent_worker_thread_reset(old_dp)) {
			poll = true;
		}
		if (poll) _dispatch_mgr_queue_poke(&_dispatch_mgr_q, 0);
	}
	_dispatch_deferred_items_set(NULL);

	if (ddi.ddi_stashed_pp & _PTHREAD_PRIORITY_PRIORITY_MASK) {
		*nevents = 0;
		if (ddi.ddi_nevents) {
			_dispatch_kq_update_all(ddi.ddi_eventlist, ddi.ddi_nevents);
		}
		ddi.ddi_stashed_pp &= _PTHREAD_PRIORITY_QOS_CLASS_MASK;
		return _dispatch_root_queue_drain_deferred_item(ddi.ddi_stashed_dq,
				ddi.ddi_stashed_dou, ddi.ddi_stashed_pp);
#ifndef WORKQ_KEVENT_EVENT_BUFFER_LEN
	} else if (ddi.ddi_nevents > *nevents) {
		*nevents = 0;
		_dispatch_kq_update_all(ddi.ddi_eventlist, ddi.ddi_nevents);
#endif
	} else {
		*nevents = ddi.ddi_nevents;
		dispatch_static_assert(__builtin_types_compatible_p(typeof(**events),
				typeof(*ddi.ddi_eventlist)));
		memcpy(*events, ddi.ddi_eventlist,
			 (size_t)ddi.ddi_nevents * sizeof(*ddi.ddi_eventlist));
	}
}
#endif // DISPATCH_USE_KEVENT_WORKQUEUE

#pragma mark -
#pragma mark dispatch_memorypressure

#if DISPATCH_USE_MEMORYPRESSURE_SOURCE
#define DISPATCH_MEMORYPRESSURE_SOURCE_TYPE DISPATCH_SOURCE_TYPE_MEMORYPRESSURE
#define DISPATCH_MEMORYPRESSURE_SOURCE_MASK ( \
		DISPATCH_MEMORYPRESSURE_NORMAL | \
		DISPATCH_MEMORYPRESSURE_WARN | \
		DISPATCH_MEMORYPRESSURE_CRITICAL | \
		DISPATCH_MEMORYPRESSURE_PROC_LIMIT_WARN | \
		DISPATCH_MEMORYPRESSURE_PROC_LIMIT_CRITICAL)
#define DISPATCH_MEMORYPRESSURE_MALLOC_MASK ( \
		DISPATCH_MEMORYPRESSURE_WARN | \
		DISPATCH_MEMORYPRESSURE_CRITICAL | \
		DISPATCH_MEMORYPRESSURE_PROC_LIMIT_WARN | \
		DISPATCH_MEMORYPRESSURE_PROC_LIMIT_CRITICAL)
#elif DISPATCH_USE_VM_PRESSURE_SOURCE
#define DISPATCH_MEMORYPRESSURE_SOURCE_TYPE DISPATCH_SOURCE_TYPE_VM
#define DISPATCH_MEMORYPRESSURE_SOURCE_MASK DISPATCH_VM_PRESSURE
#endif

#if DISPATCH_USE_MEMORYPRESSURE_SOURCE || DISPATCH_USE_VM_PRESSURE_SOURCE
static dispatch_source_t _dispatch_memorypressure_source;

static void
_dispatch_memorypressure_handler(void *context DISPATCH_UNUSED)
{
#if DISPATCH_USE_MEMORYPRESSURE_SOURCE
	unsigned long memorypressure;
	memorypressure = dispatch_source_get_data(_dispatch_memorypressure_source);

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
	if (memorypressure & DISPATCH_MEMORYPRESSURE_MALLOC_MASK) {
		malloc_memory_event_handler(memorypressure & DISPATCH_MEMORYPRESSURE_MALLOC_MASK);
	}
#elif DISPATCH_USE_VM_PRESSURE_SOURCE
	// we must have gotten DISPATCH_VM_PRESSURE
	malloc_zone_pressure_relief(0,0);
#endif
}

static void
_dispatch_memorypressure_init(void)
{
	_dispatch_memorypressure_source = dispatch_source_create(
			DISPATCH_MEMORYPRESSURE_SOURCE_TYPE, 0,
			DISPATCH_MEMORYPRESSURE_SOURCE_MASK,
			_dispatch_get_root_queue(_DISPATCH_QOS_CLASS_DEFAULT, true));
	dispatch_source_set_event_handler_f(_dispatch_memorypressure_source,
			_dispatch_memorypressure_handler);
	dispatch_activate(_dispatch_memorypressure_source);
}
#else
static inline void _dispatch_memorypressure_init(void) {}
#endif // DISPATCH_USE_MEMORYPRESSURE_SOURCE || DISPATCH_USE_VM_PRESSURE_SOURCE

#pragma mark -
#pragma mark dispatch_mach

#if HAVE_MACH

#if DISPATCH_DEBUG && DISPATCH_MACHPORT_DEBUG
#define _dispatch_debug_machport(name) \
		dispatch_debug_machport((name), __func__)
#else
#define _dispatch_debug_machport(name) ((void)(name))
#endif

// Flags for all notifications that are registered/unregistered when a
// send-possible notification is requested/delivered
#define _DISPATCH_MACH_SP_FLAGS (DISPATCH_MACH_SEND_POSSIBLE| \
		DISPATCH_MACH_SEND_DEAD|DISPATCH_MACH_SEND_DELETED)
#define _DISPATCH_MACH_RECV_FLAGS (DISPATCH_MACH_RECV_MESSAGE| \
		DISPATCH_MACH_RECV_MESSAGE_DIRECT| \
		DISPATCH_MACH_RECV_MESSAGE_DIRECT_ONCE)
#define _DISPATCH_MACH_RECV_DIRECT_FLAGS ( \
		DISPATCH_MACH_RECV_MESSAGE_DIRECT| \
		DISPATCH_MACH_RECV_MESSAGE_DIRECT_ONCE)

#define _DISPATCH_IS_POWER_OF_TWO(v) (!(v & (v - 1)) && v)
#define _DISPATCH_HASH(x, y) (_DISPATCH_IS_POWER_OF_TWO(y) ? \
		(MACH_PORT_INDEX(x) & ((y) - 1)) : (MACH_PORT_INDEX(x) % (y)))

#define _DISPATCH_MACHPORT_HASH_SIZE 32
#define _DISPATCH_MACHPORT_HASH(x) \
		_DISPATCH_HASH((x), _DISPATCH_MACHPORT_HASH_SIZE)

#ifndef MACH_RCV_VOUCHER
#define MACH_RCV_VOUCHER 0x00000800
#endif
#define DISPATCH_MACH_RCV_TRAILER MACH_RCV_TRAILER_CTX
#define DISPATCH_MACH_RCV_OPTIONS ( \
		MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_LARGE_IDENTITY | \
		MACH_RCV_TRAILER_ELEMENTS(DISPATCH_MACH_RCV_TRAILER) | \
		MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)) | \
		MACH_RCV_VOUCHER

#define DISPATCH_MACH_NOTIFICATION_ARMED(dk) ((dk)->dk_kevent.ext[0])

static void _dispatch_kevent_mach_msg_recv(_dispatch_kevent_qos_s *ke,
		mach_msg_header_t *hdr);
static void _dispatch_kevent_mach_msg_destroy(_dispatch_kevent_qos_s *ke,
		mach_msg_header_t *hdr);
static void _dispatch_source_merge_mach_msg(dispatch_source_t ds,
		dispatch_source_refs_t dr, dispatch_kevent_t dk,
		_dispatch_kevent_qos_s *ke, mach_msg_header_t *hdr,
		mach_msg_size_t siz);
static kern_return_t _dispatch_mach_notify_update(dispatch_kevent_t dk,
		uint32_t new_flags, uint32_t del_flags, uint32_t mask,
		mach_msg_id_t notify_msgid, mach_port_mscount_t notify_sync);
static void _dispatch_mach_notify_source_invoke(mach_msg_header_t *hdr);
static void _dispatch_mach_reply_kevent_unregister(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, unsigned int options);
static void _dispatch_mach_notification_kevent_unregister(dispatch_mach_t dm);
static void _dispatch_mach_msg_recv(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, _dispatch_kevent_qos_s *ke,
		mach_msg_header_t *hdr, mach_msg_size_t siz);
static void _dispatch_mach_merge_notification_kevent(dispatch_mach_t dm,
		const _dispatch_kevent_qos_s *ke);
static inline mach_msg_option_t _dispatch_mach_checkin_options(void);

static const size_t _dispatch_mach_recv_msg_size =
		DISPATCH_MACH_RECEIVE_MAX_INLINE_MESSAGE_SIZE;
static const size_t dispatch_mach_trailer_size =
		sizeof(dispatch_mach_trailer_t);
static mach_port_t _dispatch_mach_notify_port;
static dispatch_source_t _dispatch_mach_notify_source;

static inline void*
_dispatch_kevent_mach_msg_buf(_dispatch_kevent_qos_s *ke)
{
	return (void*)ke->ext[0];
}

static inline mach_msg_size_t
_dispatch_kevent_mach_msg_size(_dispatch_kevent_qos_s *ke)
{
	// buffer size in the successful receive case, but message size (like
	// msgh_size) in the MACH_RCV_TOO_LARGE case, i.e. add trailer size.
	return (mach_msg_size_t)ke->ext[1];
}

static void
_dispatch_source_type_mach_recv_direct_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED,
	unsigned long mask DISPATCH_UNUSED,
	dispatch_queue_t q DISPATCH_UNUSED)
{
	ds->ds_pending_data_mask = DISPATCH_MACH_RECV_MESSAGE_DIRECT;
#if DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK
	if (_dispatch_evfilt_machport_direct_enabled) return;
	ds->ds_dkev->dk_kevent.fflags = DISPATCH_MACH_RECV_MESSAGE_DIRECT;
	ds->ds_dkev->dk_kevent.flags &= ~(EV_UDATA_SPECIFIC|EV_VANISHED);
	ds->ds_is_direct_kevent = false;
#endif
}

static const
struct dispatch_source_type_s _dispatch_source_type_mach_recv_direct = {
	.ke = {
		.filter = EVFILT_MACHPORT,
		.flags = EV_VANISHED|EV_DISPATCH|EV_UDATA_SPECIFIC,
		.fflags = DISPATCH_MACH_RCV_OPTIONS,
	},
	.init = _dispatch_source_type_mach_recv_direct_init,
};

#if DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK
static mach_port_t _dispatch_mach_portset,  _dispatch_mach_recv_portset;
static _dispatch_kevent_qos_s _dispatch_mach_recv_kevent = {
	.filter = EVFILT_MACHPORT,
	.flags = EV_ADD|EV_ENABLE|EV_DISPATCH,
	.fflags = DISPATCH_MACH_RCV_OPTIONS,
};

static void
_dispatch_mach_recv_msg_buf_init(void)
{
	if (_dispatch_evfilt_machport_direct_enabled) return;
	mach_vm_size_t vm_size = mach_vm_round_page(
			_dispatch_mach_recv_msg_size + dispatch_mach_trailer_size);
	mach_vm_address_t vm_addr = vm_page_size;
	kern_return_t kr;

	while (slowpath(kr = mach_vm_allocate(mach_task_self(), &vm_addr, vm_size,
			VM_FLAGS_ANYWHERE))) {
		if (kr != KERN_NO_SPACE) {
			DISPATCH_CLIENT_CRASH(kr,
					"Could not allocate mach msg receive buffer");
		}
		_dispatch_temporary_resource_shortage();
		vm_addr = vm_page_size;
	}
	_dispatch_mach_recv_kevent.ext[0] = (uintptr_t)vm_addr;
	_dispatch_mach_recv_kevent.ext[1] = vm_size;
}
#endif

DISPATCH_NOINLINE
static void
_dispatch_source_merge_mach_msg_direct(dispatch_source_t ds,
		_dispatch_kevent_qos_s *ke, mach_msg_header_t *hdr)
{
	dispatch_continuation_t dc = _dispatch_source_get_event_handler(ds->ds_refs);
	dispatch_queue_t cq = _dispatch_queue_get_current();

	// see firehose_client_push_notify_async
	_dispatch_queue_set_current(ds->_as_dq);
	dc->dc_func(hdr);
	_dispatch_queue_set_current(cq);
	if (hdr != _dispatch_kevent_mach_msg_buf(ke)) {
		free(hdr);
	}
}

dispatch_source_t
_dispatch_source_create_mach_msg_direct_recv(mach_port_t recvp,
		const struct dispatch_continuation_s *dc)
{
	dispatch_source_t ds;
	ds = dispatch_source_create(&_dispatch_source_type_mach_recv_direct,
			recvp, 0, &_dispatch_mgr_q);
	os_atomic_store(&ds->ds_refs->ds_handler[DS_EVENT_HANDLER],
			(dispatch_continuation_t)dc, relaxed);
	return ds;
}

static void
_dispatch_mach_notify_port_init(void *context DISPATCH_UNUSED)
{
	kern_return_t kr;
#if HAVE_MACH_PORT_CONSTRUCT
	mach_port_options_t opts = { .flags = MPO_CONTEXT_AS_GUARD | MPO_STRICT };
#ifdef __LP64__
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
	if (slowpath(kr)) {
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

static mach_port_t
_dispatch_get_mach_notify_port(void)
{
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_mach_notify_port_init);
	return _dispatch_mach_notify_port;
}

#if DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK
static void
_dispatch_mach_recv_portset_init(void *context DISPATCH_UNUSED)
{
	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET,
			&_dispatch_mach_recv_portset);
	DISPATCH_VERIFY_MIG(kr);
	if (slowpath(kr)) {
		DISPATCH_CLIENT_CRASH(kr,
				"mach_port_allocate() failed: cannot create port set");
	}
	_dispatch_kevent_qos_s *ke = &_dispatch_mach_recv_kevent;
	dispatch_assert(_dispatch_kevent_mach_msg_buf(ke));
	dispatch_assert(dispatch_mach_trailer_size ==
			REQUESTED_TRAILER_SIZE_NATIVE(MACH_RCV_TRAILER_ELEMENTS(
			DISPATCH_MACH_RCV_TRAILER)));
	ke->ident = _dispatch_mach_recv_portset;
#if DISPATCH_USE_KEVENT_WORKQUEUE
	if (_dispatch_kevent_workqueue_enabled) {
		ke->qos = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;
	}
#endif
	_dispatch_kq_immediate_update(&_dispatch_mach_recv_kevent);
}

static mach_port_t
_dispatch_get_mach_recv_portset(void)
{
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_mach_recv_portset_init);
	return _dispatch_mach_recv_portset;
}

static void
_dispatch_mach_portset_init(void *context DISPATCH_UNUSED)
{
	_dispatch_kevent_qos_s kev = {
		.filter = EVFILT_MACHPORT,
		.flags = EV_ADD,
	};
#if DISPATCH_USE_KEVENT_WORKQUEUE
	if (_dispatch_kevent_workqueue_enabled) {
		kev.qos = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;
	}
#endif

	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET,
			&_dispatch_mach_portset);
	DISPATCH_VERIFY_MIG(kr);
	if (slowpath(kr)) {
		DISPATCH_CLIENT_CRASH(kr,
				"mach_port_allocate() failed: cannot create port set");
	}
	kev.ident = _dispatch_mach_portset;
	_dispatch_kq_immediate_update(&kev);
}

static mach_port_t
_dispatch_get_mach_portset(void)
{
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_mach_portset_init);
	return _dispatch_mach_portset;
}

static kern_return_t
_dispatch_mach_portset_update(dispatch_kevent_t dk, mach_port_t mps)
{
	mach_port_t mp = (mach_port_t)dk->dk_kevent.ident;
	kern_return_t kr;

	_dispatch_debug_machport(mp);
	kr = mach_port_move_member(mach_task_self(), mp, mps);
	if (slowpath(kr)) {
		DISPATCH_VERIFY_MIG(kr);
		switch (kr) {
		case KERN_INVALID_RIGHT:
			if (mps) {
				_dispatch_bug_mach_client("_dispatch_kevent_machport_enable: "
						"mach_port_move_member() failed ", kr);
				break;
			}
			//fall through
		case KERN_INVALID_NAME:
#if DISPATCH_DEBUG
			_dispatch_log("Corruption: Mach receive right 0x%x destroyed "
					"prematurely", mp);
#endif
			break;
		default:
			(void)dispatch_assume_zero(kr);
			break;
		}
	}
	return mps ? kr : 0;
}

static kern_return_t
_dispatch_kevent_machport_resume(dispatch_kevent_t dk, uint32_t new_flags,
		uint32_t del_flags)
{
	kern_return_t kr = 0;
	dispatch_assert_zero(new_flags & del_flags);
	if ((new_flags & _DISPATCH_MACH_RECV_FLAGS) ||
			(del_flags & _DISPATCH_MACH_RECV_FLAGS)) {
		mach_port_t mps;
		if (new_flags & _DISPATCH_MACH_RECV_DIRECT_FLAGS) {
			mps = _dispatch_get_mach_recv_portset();
		} else if ((new_flags & DISPATCH_MACH_RECV_MESSAGE) ||
				((del_flags & _DISPATCH_MACH_RECV_DIRECT_FLAGS) &&
				(dk->dk_kevent.fflags & DISPATCH_MACH_RECV_MESSAGE))) {
			mps = _dispatch_get_mach_portset();
		} else {
			mps = MACH_PORT_NULL;
		}
		kr = _dispatch_mach_portset_update(dk, mps);
	}
	return kr;
}
#endif // DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK

static kern_return_t
_dispatch_kevent_mach_notify_resume(dispatch_kevent_t dk, uint32_t new_flags,
		uint32_t del_flags)
{
	kern_return_t kr = 0;
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
		kr = _dispatch_mach_notify_update(dk, new_flags, del_flags,
				_DISPATCH_MACH_SP_FLAGS, MACH_NOTIFY_SEND_POSSIBLE,
				MACH_NOTIFY_SEND_POSSIBLE == MACH_NOTIFY_DEAD_NAME ? 1 : 0);
	}
	return kr;
}

#if DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK
DISPATCH_NOINLINE
static void
_dispatch_kevent_machport_drain(_dispatch_kevent_qos_s *ke)
{
	mach_port_t name = (mach_port_name_t)ke->data;
	dispatch_kevent_t dk;

	_dispatch_debug_machport(name);
	dk = _dispatch_kevent_find(name, EVFILT_MACHPORT);
	if (!dispatch_assume(dk)) {
		return;
	}
	_dispatch_mach_portset_update(dk, MACH_PORT_NULL); // emulate EV_DISPATCH

	_dispatch_kevent_qos_s kev = {
		.ident = name,
		.filter = EVFILT_MACHPORT,
		.flags = EV_ADD|EV_ENABLE|EV_DISPATCH,
		.fflags = DISPATCH_MACH_RECV_MESSAGE,
		.udata = (uintptr_t)dk,
	};
	_dispatch_kevent_debug("synthetic", &kev);
	_dispatch_kevent_merge(&kev);
}
#endif

DISPATCH_NOINLINE
static void
_dispatch_kevent_mach_msg_drain(_dispatch_kevent_qos_s *ke)
{
	mach_msg_header_t *hdr = _dispatch_kevent_mach_msg_buf(ke);
	mach_msg_size_t siz;
	mach_msg_return_t kr = (mach_msg_return_t)ke->fflags;

	if (!fastpath(hdr)) {
		DISPATCH_INTERNAL_CRASH(kr, "EVFILT_MACHPORT with no message");
	}
	if (fastpath(!kr)) {
		_dispatch_kevent_mach_msg_recv(ke, hdr);
		goto out;
	} else if (kr != MACH_RCV_TOO_LARGE) {
		goto out;
	} else if (!ke->data) {
		DISPATCH_INTERNAL_CRASH(0, "MACH_RCV_LARGE_IDENTITY with no identity");
	}
	if (slowpath(ke->ext[1] > (UINT_MAX - dispatch_mach_trailer_size))) {
		DISPATCH_INTERNAL_CRASH(ke->ext[1],
				"EVFILT_MACHPORT with overlarge message");
	}
	siz = _dispatch_kevent_mach_msg_size(ke) + dispatch_mach_trailer_size;
	hdr = malloc(siz);
	if (!dispatch_assume(hdr)) {
		// Kernel will discard message too large to fit
		hdr = NULL;
		siz = 0;
	}
	mach_port_t name = (mach_port_name_t)ke->data;
	const mach_msg_option_t options = ((DISPATCH_MACH_RCV_OPTIONS |
			MACH_RCV_TIMEOUT) & ~MACH_RCV_LARGE);
	kr = mach_msg(hdr, options, 0, siz, name, MACH_MSG_TIMEOUT_NONE,
			MACH_PORT_NULL);
	if (fastpath(!kr)) {
		_dispatch_kevent_mach_msg_recv(ke, hdr);
		goto out;
	} else if (kr == MACH_RCV_TOO_LARGE) {
		_dispatch_log("BUG in libdispatch client: "
				"_dispatch_kevent_mach_msg_drain: dropped message too "
				"large to fit in memory: id = 0x%x, size = %u",
				hdr->msgh_id, _dispatch_kevent_mach_msg_size(ke));
		kr = MACH_MSG_SUCCESS;
	}
	if (hdr != _dispatch_kevent_mach_msg_buf(ke)) {
		free(hdr);
	}
out:
	if (slowpath(kr)) {
		_dispatch_bug_mach_client("_dispatch_kevent_mach_msg_drain: "
				"message reception failed", kr);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_mach_kevent_merge(_dispatch_kevent_qos_s *ke)
{
	if (unlikely(!(ke->flags & EV_UDATA_SPECIFIC))) {
#if DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK
		if (ke->ident == _dispatch_mach_recv_portset) {
			_dispatch_kevent_mach_msg_drain(ke);
			return _dispatch_kq_deferred_update(&_dispatch_mach_recv_kevent);
		} else if (ke->ident == _dispatch_mach_portset) {
			return _dispatch_kevent_machport_drain(ke);
		}
#endif
		return _dispatch_kevent_error(ke);
	}

	dispatch_kevent_t dk = (dispatch_kevent_t)ke->udata;
	dispatch_source_refs_t dr = TAILQ_FIRST(&dk->dk_sources);
	bool is_reply = (dk->dk_kevent.flags & EV_ONESHOT);
	dispatch_source_t ds = _dispatch_source_from_refs(dr);

	if (_dispatch_kevent_mach_msg_size(ke)) {
		_dispatch_kevent_mach_msg_drain(ke);
		if (is_reply) {
			// _dispatch_kevent_mach_msg_drain() should have deleted this event
			dispatch_assert(ke->flags & EV_DELETE);
			return;
		}

		if (!(ds->dq_atomic_flags & DSF_CANCELED)) {
			// re-arm the mach channel
			ke->fflags = DISPATCH_MACH_RCV_OPTIONS;
			ke->data = 0;
			ke->ext[0] = 0;
			ke->ext[1] = 0;
			return _dispatch_kq_deferred_update(ke);
		}
	} else if (is_reply) {
		DISPATCH_INTERNAL_CRASH(ke->flags, "Unexpected EVFILT_MACHPORT event");
	}
	if (unlikely((ke->flags & EV_VANISHED) &&
			(dx_type(ds) == DISPATCH_MACH_CHANNEL_TYPE))) {
		DISPATCH_CLIENT_CRASH(ke->flags,
				"Unexpected EV_VANISHED (do not destroy random mach ports)");
	}
	return _dispatch_kevent_merge(ke);
}

static void
_dispatch_kevent_mach_msg_recv(_dispatch_kevent_qos_s *ke,
		mach_msg_header_t *hdr)
{
	dispatch_source_refs_t dri;
	dispatch_kevent_t dk;
	mach_port_t name = hdr->msgh_local_port;
	mach_msg_size_t siz = hdr->msgh_size + dispatch_mach_trailer_size;

	if (!dispatch_assume(hdr->msgh_size <= UINT_MAX -
			dispatch_mach_trailer_size)) {
		_dispatch_bug_client("_dispatch_kevent_mach_msg_recv: "
				"received overlarge message");
		return _dispatch_kevent_mach_msg_destroy(ke, hdr);
	}
	if (!dispatch_assume(name)) {
		_dispatch_bug_client("_dispatch_kevent_mach_msg_recv: "
				"received message with MACH_PORT_NULL port");
		return _dispatch_kevent_mach_msg_destroy(ke, hdr);
	}
	_dispatch_debug_machport(name);
	if (ke->flags & EV_UDATA_SPECIFIC) {
		dk = (void*)ke->udata;
	} else {
		dk = _dispatch_kevent_find(name, EVFILT_MACHPORT);
	}
	if (!dispatch_assume(dk)) {
		_dispatch_bug_client("_dispatch_kevent_mach_msg_recv: "
				"received message with unknown kevent");
		return _dispatch_kevent_mach_msg_destroy(ke, hdr);
	}
	TAILQ_FOREACH(dri, &dk->dk_sources, dr_list) {
		dispatch_source_t dsi = _dispatch_source_from_refs(dri);
		if (dsi->ds_pending_data_mask & _DISPATCH_MACH_RECV_DIRECT_FLAGS) {
			return _dispatch_source_merge_mach_msg(dsi, dri, dk, ke, hdr, siz);
		}
	}
	_dispatch_bug_client("_dispatch_kevent_mach_msg_recv: "
			"received message with no listeners");
	return _dispatch_kevent_mach_msg_destroy(ke, hdr);
}

static void
_dispatch_kevent_mach_msg_destroy(_dispatch_kevent_qos_s *ke,
		mach_msg_header_t *hdr)
{
	if (hdr) {
		mach_msg_destroy(hdr);
		if (hdr != _dispatch_kevent_mach_msg_buf(ke)) {
			free(hdr);
		}
	}
}

static void
_dispatch_source_merge_mach_msg(dispatch_source_t ds, dispatch_source_refs_t dr,
		dispatch_kevent_t dk, _dispatch_kevent_qos_s *ke,
		mach_msg_header_t *hdr, mach_msg_size_t siz)
{
	if (dx_type(ds) == DISPATCH_SOURCE_KEVENT_TYPE) {
		return _dispatch_source_merge_mach_msg_direct(ds, ke, hdr);
	}
	dispatch_mach_reply_refs_t dmr = NULL;
	if (dk->dk_kevent.flags & EV_ONESHOT) {
		dmr = (dispatch_mach_reply_refs_t)dr;
	}
	return _dispatch_mach_msg_recv((dispatch_mach_t)ds, dmr, ke, hdr, siz);
}

DISPATCH_NOINLINE
static void
_dispatch_mach_notify_merge(mach_port_t name, uint32_t flag, bool final)
{
	dispatch_source_refs_t dri, dr_next;
	dispatch_kevent_t dk;
	bool unreg;

	dk = _dispatch_kevent_find(name, DISPATCH_EVFILT_MACH_NOTIFICATION);
	if (!dk) {
		return;
	}

	// Update notification registration state.
	dk->dk_kevent.data &= ~_DISPATCH_MACH_SP_FLAGS;
	_dispatch_kevent_qos_s kev = {
		.ident = name,
		.filter = DISPATCH_EVFILT_MACH_NOTIFICATION,
		.flags = EV_ADD|EV_ENABLE,
		.fflags = flag,
		.udata = (uintptr_t)dk,
	};
	if (final) {
		// This can never happen again
		unreg = true;
	} else {
		// Re-register for notification before delivery
		unreg = _dispatch_kevent_resume(dk, flag, 0);
	}
	DISPATCH_MACH_NOTIFICATION_ARMED(dk) = 0;
	TAILQ_FOREACH_SAFE(dri, &dk->dk_sources, dr_list, dr_next) {
		dispatch_source_t dsi = _dispatch_source_from_refs(dri);
		if (dx_type(dsi) == DISPATCH_MACH_CHANNEL_TYPE) {
			dispatch_mach_t dm = (dispatch_mach_t)dsi;
			_dispatch_mach_merge_notification_kevent(dm, &kev);
			if (unreg && dm->dm_dkev) {
				_dispatch_mach_notification_kevent_unregister(dm);
			}
		} else {
			_dispatch_source_merge_kevent(dsi, &kev);
			if (unreg) {
				_dispatch_source_kevent_unregister(dsi);
			}
		}
		if (!dr_next || DISPATCH_MACH_NOTIFICATION_ARMED(dk)) {
			// current merge is last in list (dk might have been freed)
			// or it re-armed the notification
			return;
		}
	}
}

static kern_return_t
_dispatch_mach_notify_update(dispatch_kevent_t dk, uint32_t new_flags,
		uint32_t del_flags, uint32_t mask, mach_msg_id_t notify_msgid,
		mach_port_mscount_t notify_sync)
{
	mach_port_t previous, port = (mach_port_t)dk->dk_kevent.ident;
	typeof(dk->dk_kevent.data) prev = dk->dk_kevent.data;
	kern_return_t kr, krr = 0;

	// Update notification registration state.
	dk->dk_kevent.data |= (new_flags | dk->dk_kevent.fflags) & mask;
	dk->dk_kevent.data &= ~(del_flags & mask);

	_dispatch_debug_machport(port);
	if ((dk->dk_kevent.data & mask) && !(prev & mask)) {
		_dispatch_debug("machport[0x%08x]: registering for send-possible "
				"notification", port);
		previous = MACH_PORT_NULL;
		krr = mach_port_request_notification(mach_task_self(), port,
				notify_msgid, notify_sync, _dispatch_get_mach_notify_port(),
				MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
		DISPATCH_VERIFY_MIG(krr);

		switch(krr) {
		case KERN_INVALID_NAME:
		case KERN_INVALID_RIGHT:
			// Suppress errors & clear registration state
			dk->dk_kevent.data &= ~mask;
			break;
		default:
			// Else, we don't expect any errors from mach. Log any errors
			if (dispatch_assume_zero(krr)) {
				// log the error & clear registration state
				dk->dk_kevent.data &= ~mask;
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
	} else if (!(dk->dk_kevent.data & mask) && (prev & mask)) {
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
	if (slowpath(previous)) {
		// the kernel has not consumed the send-once right yet
		(void)dispatch_assume_zero(
				_dispatch_send_consume_send_once_right(previous));
	}
	return krr;
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
	// Fallback when missing support for newer _SET variant, fires strictly more.
	if (kr == KERN_INVALID_ARGUMENT &&
		notify_type != HOST_NOTIFY_CALENDAR_CHANGE){
		notify_type = HOST_NOTIFY_CALENDAR_CHANGE;
		goto retry;
	}
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
}

static void
_dispatch_mach_host_calendar_change_register(void)
{
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_mach_host_notify_update);
}

static void
_dispatch_mach_notify_source_invoke(mach_msg_header_t *hdr)
{
	mig_reply_error_t reply;
	dispatch_assert(sizeof(mig_reply_error_t) == sizeof(union
		__ReplyUnion___dispatch_libdispatch_internal_protocol_subsystem));
	dispatch_assert(sizeof(mig_reply_error_t) < _dispatch_mach_recv_msg_size);
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

#pragma mark -
#pragma mark dispatch_mach_t

#define DISPATCH_MACH_RETURN_IMMEDIATE_SEND_RESULT 0x1
#define DISPATCH_MACH_REGISTER_FOR_REPLY 0x2
#define DISPATCH_MACH_WAIT_FOR_REPLY 0x4
#define DISPATCH_MACH_OWNED_REPLY_PORT 0x8
#define DISPATCH_MACH_OPTIONS_MASK 0xffff

#define DM_SEND_STATUS_SUCCESS 0x1
#define DM_SEND_STATUS_RETURNING_IMMEDIATE_SEND_RESULT 0x2

DISPATCH_ENUM(dispatch_mach_send_invoke_flags, uint32_t,
	DM_SEND_INVOKE_NONE            = 0x0,
	DM_SEND_INVOKE_FLUSH           = 0x1,
	DM_SEND_INVOKE_NEEDS_BARRIER   = 0x2,
	DM_SEND_INVOKE_CANCEL          = 0x4,
	DM_SEND_INVOKE_CAN_RUN_BARRIER = 0x8,
	DM_SEND_INVOKE_IMMEDIATE_SEND  = 0x10,
);
#define DM_SEND_INVOKE_IMMEDIATE_SEND_MASK \
		((dispatch_mach_send_invoke_flags_t)DM_SEND_INVOKE_IMMEDIATE_SEND)

static inline pthread_priority_t _dispatch_mach_priority_propagate(
		mach_msg_option_t options);
static mach_port_t _dispatch_mach_msg_get_remote_port(dispatch_object_t dou);
static mach_port_t _dispatch_mach_msg_get_reply_port(dispatch_object_t dou);
static void _dispatch_mach_msg_disconnected(dispatch_mach_t dm,
		mach_port_t local_port, mach_port_t remote_port);
static inline void _dispatch_mach_msg_reply_received(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, mach_port_t local_port);
static dispatch_mach_msg_t _dispatch_mach_msg_create_reply_disconnected(
		dispatch_object_t dou, dispatch_mach_reply_refs_t dmr);
static bool _dispatch_mach_reconnect_invoke(dispatch_mach_t dm,
		dispatch_object_t dou);
static inline mach_msg_header_t* _dispatch_mach_msg_get_msg(
		dispatch_mach_msg_t dmsg);
static void _dispatch_mach_send_push(dispatch_mach_t dm, dispatch_object_t dou,
		pthread_priority_t pp);

static dispatch_mach_t
_dispatch_mach_create(const char *label, dispatch_queue_t q, void *context,
		dispatch_mach_handler_function_t handler, bool handler_is_block)
{
	dispatch_mach_t dm;
	dispatch_mach_refs_t dr;

	dm = _dispatch_alloc(DISPATCH_VTABLE(mach),
			sizeof(struct dispatch_mach_s));
	_dispatch_queue_init(dm->_as_dq, DQF_NONE, 1, true);

	dm->dq_label = label;
	dm->do_ref_cnt++; // the reference _dispatch_mach_cancel_invoke holds

	dr = _dispatch_calloc(1ul, sizeof(struct dispatch_mach_refs_s));
	dr->dr_source_wref = _dispatch_ptr2wref(dm);
	dr->dm_handler_func = handler;
	dr->dm_handler_ctxt = context;
	dm->ds_refs = dr;
	dm->dm_handler_is_block = handler_is_block;

	dm->dm_refs = _dispatch_calloc(1ul,
			sizeof(struct dispatch_mach_send_refs_s));
	dm->dm_refs->dr_source_wref = _dispatch_ptr2wref(dm);
	dm->dm_refs->dm_disconnect_cnt = DISPATCH_MACH_NEVER_CONNECTED;
	TAILQ_INIT(&dm->dm_refs->dm_replies);

	if (slowpath(!q)) {
		q = _dispatch_get_root_queue(_DISPATCH_QOS_CLASS_DEFAULT, true);
	} else {
		_dispatch_retain(q);
	}
	dm->do_targetq = q;
	_dispatch_object_debug(dm, "%s", __func__);
	return dm;
}

dispatch_mach_t
dispatch_mach_create(const char *label, dispatch_queue_t q,
		dispatch_mach_handler_t handler)
{
	dispatch_block_t bb = _dispatch_Block_copy((void*)handler);
	return _dispatch_mach_create(label, q, bb,
			(dispatch_mach_handler_function_t)_dispatch_Block_invoke(bb), true);
}

dispatch_mach_t
dispatch_mach_create_f(const char *label, dispatch_queue_t q, void *context,
		dispatch_mach_handler_function_t handler)
{
	return _dispatch_mach_create(label, q, context, handler, false);
}

void
_dispatch_mach_dispose(dispatch_mach_t dm)
{
	_dispatch_object_debug(dm, "%s", __func__);
	dispatch_mach_refs_t dr = dm->ds_refs;
	if (dm->dm_handler_is_block && dr->dm_handler_ctxt) {
		Block_release(dr->dm_handler_ctxt);
	}
	free(dr);
	free(dm->dm_refs);
	_dispatch_queue_destroy(dm->_as_dq);
}

void
dispatch_mach_connect(dispatch_mach_t dm, mach_port_t receive,
		mach_port_t send, dispatch_mach_msg_t checkin)
{
	dispatch_mach_send_refs_t dr = dm->dm_refs;
	dispatch_kevent_t dk;
	uint32_t disconnect_cnt;
	dispatch_source_type_t type = &_dispatch_source_type_mach_recv_direct;

	dm->ds_is_direct_kevent = (bool)_dispatch_evfilt_machport_direct_enabled;
	if (MACH_PORT_VALID(receive)) {
		dk = _dispatch_calloc(1ul, sizeof(struct dispatch_kevent_s));
		dk->dk_kevent = type->ke;
		dk->dk_kevent.ident = receive;
		dk->dk_kevent.flags |= EV_ADD|EV_ENABLE|EV_VANISHED;
		dk->dk_kevent.udata = (uintptr_t)dk;
		TAILQ_INIT(&dk->dk_sources);
		dm->ds_dkev = dk;
		dm->ds_pending_data_mask = DISPATCH_MACH_RECV_MESSAGE_DIRECT;
		dm->ds_needs_rearm = dm->ds_is_direct_kevent;
		if (!dm->ds_is_direct_kevent) {
			dk->dk_kevent.fflags = DISPATCH_MACH_RECV_MESSAGE_DIRECT;
			dk->dk_kevent.flags &= ~(EV_UDATA_SPECIFIC|EV_VANISHED);
		}
		_dispatch_retain(dm); // the reference the manager queue holds
	}
	dr->dm_send = send;
	if (MACH_PORT_VALID(send)) {
		if (checkin) {
			dispatch_retain(checkin);
			checkin->dmsg_options = _dispatch_mach_checkin_options();
			dr->dm_checkin_port = _dispatch_mach_msg_get_remote_port(checkin);
		}
		dr->dm_checkin = checkin;
	}
	// monitor message reply ports
	dm->ds_pending_data_mask |= DISPATCH_MACH_RECV_MESSAGE_DIRECT_ONCE;
	dispatch_assert(DISPATCH_MACH_NEVER_CONNECTED - 1 ==
			DISPATCH_MACH_NEVER_INSTALLED);
	disconnect_cnt = os_atomic_dec2o(dr, dm_disconnect_cnt, release);
	if (unlikely(disconnect_cnt != DISPATCH_MACH_NEVER_INSTALLED)) {
		DISPATCH_CLIENT_CRASH(disconnect_cnt, "Channel already connected");
	}
	_dispatch_object_debug(dm, "%s", __func__);
	return dispatch_activate(dm);
}

// assumes low bit of mach port names is always set
#define DISPATCH_MACH_REPLY_PORT_UNOWNED 0x1u

static inline void
_dispatch_mach_reply_mark_reply_port_owned(dispatch_mach_reply_refs_t dmr)
{
	dmr->dmr_reply &= ~DISPATCH_MACH_REPLY_PORT_UNOWNED;
}

static inline bool
_dispatch_mach_reply_is_reply_port_owned(dispatch_mach_reply_refs_t dmr)
{
	mach_port_t reply_port = dmr->dmr_reply;
	return reply_port ? !(reply_port & DISPATCH_MACH_REPLY_PORT_UNOWNED) :false;
}

static inline mach_port_t
_dispatch_mach_reply_get_reply_port(dispatch_mach_reply_refs_t dmr)
{
	mach_port_t reply_port = dmr->dmr_reply;
	return reply_port ? (reply_port | DISPATCH_MACH_REPLY_PORT_UNOWNED) : 0;
}

static inline bool
_dispatch_mach_reply_tryremove(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr)
{
	bool removed;
	_dispatch_unfair_lock_lock(&dm->dm_refs->dm_replies_lock);
	if ((removed = _TAILQ_IS_ENQUEUED(dmr, dmr_list))) {
		TAILQ_REMOVE(&dm->dm_refs->dm_replies, dmr, dmr_list);
		_TAILQ_MARK_NOT_ENQUEUED(dmr, dmr_list);
	}
	_dispatch_unfair_lock_unlock(&dm->dm_refs->dm_replies_lock);
	return removed;
}

DISPATCH_NOINLINE
static void
_dispatch_mach_reply_waiter_unregister(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, unsigned int options)
{
	dispatch_mach_msg_t dmsgr = NULL;
	bool disconnected = (options & DKEV_UNREGISTER_DISCONNECTED);
	if (options & DKEV_UNREGISTER_REPLY_REMOVE) {
		_dispatch_unfair_lock_lock(&dm->dm_refs->dm_replies_lock);
		if (unlikely(!_TAILQ_IS_ENQUEUED(dmr, dmr_list))) {
			DISPATCH_INTERNAL_CRASH(0, "Could not find reply registration");
		}
		TAILQ_REMOVE(&dm->dm_refs->dm_replies, dmr, dmr_list);
		_TAILQ_MARK_NOT_ENQUEUED(dmr, dmr_list);
		_dispatch_unfair_lock_unlock(&dm->dm_refs->dm_replies_lock);
	}
	if (disconnected) {
		dmsgr = _dispatch_mach_msg_create_reply_disconnected(NULL, dmr);
	} else if (dmr->dmr_voucher) {
		_voucher_release(dmr->dmr_voucher);
		dmr->dmr_voucher = NULL;
	}
	_dispatch_debug("machport[0x%08x]: unregistering for sync reply%s, ctxt %p",
			_dispatch_mach_reply_get_reply_port(dmr),
			disconnected ? " (disconnected)" : "", dmr->dmr_ctxt);
	if (dmsgr) {
		return _dispatch_queue_push(dm->_as_dq, dmsgr, dmsgr->dmsg_priority);
	}
	dispatch_assert(!(options & DKEV_UNREGISTER_WAKEUP));
}

DISPATCH_NOINLINE
static void
_dispatch_mach_reply_kevent_unregister(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, unsigned int options)
{
	dispatch_mach_msg_t dmsgr = NULL;
	bool replies_empty = false;
	bool disconnected = (options & DKEV_UNREGISTER_DISCONNECTED);
	if (options & DKEV_UNREGISTER_REPLY_REMOVE) {
		_dispatch_unfair_lock_lock(&dm->dm_refs->dm_replies_lock);
		if (unlikely(!_TAILQ_IS_ENQUEUED(dmr, dmr_list))) {
			DISPATCH_INTERNAL_CRASH(0, "Could not find reply registration");
		}
		TAILQ_REMOVE(&dm->dm_refs->dm_replies, dmr, dmr_list);
		_TAILQ_MARK_NOT_ENQUEUED(dmr, dmr_list);
		replies_empty = TAILQ_EMPTY(&dm->dm_refs->dm_replies);
		_dispatch_unfair_lock_unlock(&dm->dm_refs->dm_replies_lock);
	}
	if (disconnected) {
		dmsgr = _dispatch_mach_msg_create_reply_disconnected(NULL, dmr);
	} else if (dmr->dmr_voucher) {
		_voucher_release(dmr->dmr_voucher);
		dmr->dmr_voucher = NULL;
	}
	uint32_t flags = DISPATCH_MACH_RECV_MESSAGE_DIRECT_ONCE;
	dispatch_kevent_t dk = dmr->dmr_dkev;
	_dispatch_debug("machport[0x%08x]: unregistering for reply%s, ctxt %p",
			(mach_port_t)dk->dk_kevent.ident,
			disconnected ? " (disconnected)" : "", dmr->dmr_ctxt);
	if (!dm->ds_is_direct_kevent) {
		dmr->dmr_dkev = NULL;
		TAILQ_REMOVE(&dk->dk_sources, (dispatch_source_refs_t)dmr, dr_list);
		_dispatch_kevent_unregister(dk, flags, 0);
	} else {
		long r = _dispatch_kevent_unregister(dk, flags, options);
		if (r == EINPROGRESS) {
			_dispatch_debug("machport[0x%08x]: deferred delete kevent[%p]",
					(mach_port_t)dk->dk_kevent.ident, dk);
			dispatch_assert(options == DKEV_UNREGISTER_DISCONNECTED);
			// dmr must be put back so that the event delivery finds it, the
			// replies lock is held by the caller.
			TAILQ_INSERT_HEAD(&dm->dm_refs->dm_replies, dmr, dmr_list);
			if (dmsgr) {
				dmr->dmr_voucher = dmsgr->dmsg_voucher;
				dmsgr->dmsg_voucher = NULL;
				dispatch_release(dmsgr);
			}
			return; // deferred unregistration
		}
		dispatch_assume_zero(r);
		dmr->dmr_dkev = NULL;
		_TAILQ_TRASH_ENTRY(dmr, dr_list);
	}
	free(dmr);
	if (dmsgr) {
		return _dispatch_queue_push(dm->_as_dq, dmsgr, dmsgr->dmsg_priority);
	}
	if ((options & DKEV_UNREGISTER_WAKEUP) && replies_empty &&
			(dm->dm_refs->dm_disconnect_cnt ||
			(dm->dq_atomic_flags & DSF_CANCELED))) {
		dx_wakeup(dm, 0, DISPATCH_WAKEUP_FLUSH);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_mach_reply_waiter_register(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, mach_port_t reply_port,
		dispatch_mach_msg_t dmsg, mach_msg_option_t msg_opts)
{
	dmr->dr_source_wref = _dispatch_ptr2wref(dm);
	dmr->dmr_dkev = NULL;
	dmr->dmr_reply = reply_port;
	if (msg_opts & DISPATCH_MACH_OWNED_REPLY_PORT) {
		_dispatch_mach_reply_mark_reply_port_owned(dmr);
	} else {
		if (dmsg->dmsg_voucher) {
			dmr->dmr_voucher = _voucher_retain(dmsg->dmsg_voucher);
		}
		dmr->dmr_priority = (dispatch_priority_t)dmsg->dmsg_priority;
		// make reply context visible to leaks rdar://11777199
		dmr->dmr_ctxt = dmsg->do_ctxt;
	}

	_dispatch_debug("machport[0x%08x]: registering for sync reply, ctxt %p",
			reply_port, dmsg->do_ctxt);
	_dispatch_unfair_lock_lock(&dm->dm_refs->dm_replies_lock);
	if (unlikely(_TAILQ_IS_ENQUEUED(dmr, dmr_list))) {
		DISPATCH_INTERNAL_CRASH(dmr->dmr_list.tqe_prev, "Reply already registered");
	}
	TAILQ_INSERT_TAIL(&dm->dm_refs->dm_replies, dmr, dmr_list);
	_dispatch_unfair_lock_unlock(&dm->dm_refs->dm_replies_lock);
}

DISPATCH_NOINLINE
static void
_dispatch_mach_reply_kevent_register(dispatch_mach_t dm, mach_port_t reply_port,
		dispatch_mach_msg_t dmsg)
{
	dispatch_kevent_t dk;
	dispatch_mach_reply_refs_t dmr;
	dispatch_source_type_t type = &_dispatch_source_type_mach_recv_direct;
	pthread_priority_t mp, pp;

	dk = _dispatch_calloc(1ul, sizeof(struct dispatch_kevent_s));
	dk->dk_kevent = type->ke;
	dk->dk_kevent.ident = reply_port;
	dk->dk_kevent.flags |= EV_ADD|EV_ENABLE|EV_ONESHOT;
	dk->dk_kevent.udata = (uintptr_t)dk;
	TAILQ_INIT(&dk->dk_sources);
	if (!dm->ds_is_direct_kevent) {
		dk->dk_kevent.fflags = DISPATCH_MACH_RECV_MESSAGE_DIRECT_ONCE;
		dk->dk_kevent.flags &= ~(EV_UDATA_SPECIFIC|EV_VANISHED);
	}

	dmr = _dispatch_calloc(1ul, sizeof(struct dispatch_mach_reply_refs_s));
	dmr->dr_source_wref = _dispatch_ptr2wref(dm);
	dmr->dmr_dkev = dk;
	dmr->dmr_reply = reply_port;
	if (dmsg->dmsg_voucher) {
		dmr->dmr_voucher = _voucher_retain(dmsg->dmsg_voucher);
	}
	dmr->dmr_priority = (dispatch_priority_t)dmsg->dmsg_priority;
	// make reply context visible to leaks rdar://11777199
	dmr->dmr_ctxt = dmsg->do_ctxt;

	pp = dm->dq_priority & ~_PTHREAD_PRIORITY_FLAGS_MASK;
	if (pp && dm->ds_is_direct_kevent) {
		mp = dmsg->dmsg_priority & ~_PTHREAD_PRIORITY_FLAGS_MASK;
		if (pp < mp) pp = mp;
		pp |= dm->dq_priority & _PTHREAD_PRIORITY_OVERCOMMIT_FLAG;
	} else {
		pp = _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;
	}

	_dispatch_debug("machport[0x%08x]: registering for reply, ctxt %p",
			reply_port, dmsg->do_ctxt);
	uint32_t flags;
	bool do_resume = _dispatch_kevent_register(&dmr->dmr_dkev, pp, &flags);
	TAILQ_INSERT_TAIL(&dmr->dmr_dkev->dk_sources, (dispatch_source_refs_t)dmr,
			dr_list);
	_dispatch_unfair_lock_lock(&dm->dm_refs->dm_replies_lock);
	if (unlikely(_TAILQ_IS_ENQUEUED(dmr, dmr_list))) {
		DISPATCH_INTERNAL_CRASH(dmr->dmr_list.tqe_prev, "Reply already registered");
	}
	TAILQ_INSERT_TAIL(&dm->dm_refs->dm_replies, dmr, dmr_list);
	_dispatch_unfair_lock_unlock(&dm->dm_refs->dm_replies_lock);
	if (do_resume && _dispatch_kevent_resume(dmr->dmr_dkev, flags, 0)) {
		return _dispatch_mach_reply_kevent_unregister(dm, dmr,
				DKEV_UNREGISTER_DISCONNECTED|DKEV_UNREGISTER_REPLY_REMOVE);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_mach_notification_kevent_unregister(dispatch_mach_t dm)
{
	DISPATCH_ASSERT_ON_MANAGER_QUEUE();
	dispatch_kevent_t dk = dm->dm_dkev;
	dm->dm_dkev = NULL;
	TAILQ_REMOVE(&dk->dk_sources, (dispatch_source_refs_t)dm->dm_refs,
			dr_list);
	dm->ds_pending_data_mask &= ~(unsigned long)
			(DISPATCH_MACH_SEND_POSSIBLE|DISPATCH_MACH_SEND_DEAD);
	_dispatch_kevent_unregister(dk,
			DISPATCH_MACH_SEND_POSSIBLE|DISPATCH_MACH_SEND_DEAD, 0);
}

DISPATCH_NOINLINE
static void
_dispatch_mach_notification_kevent_register(dispatch_mach_t dm,mach_port_t send)
{
	DISPATCH_ASSERT_ON_MANAGER_QUEUE();
	dispatch_kevent_t dk;

	dk = _dispatch_calloc(1ul, sizeof(struct dispatch_kevent_s));
	dk->dk_kevent = _dispatch_source_type_mach_send.ke;
	dk->dk_kevent.ident = send;
	dk->dk_kevent.flags |= EV_ADD|EV_ENABLE;
	dk->dk_kevent.fflags = DISPATCH_MACH_SEND_POSSIBLE|DISPATCH_MACH_SEND_DEAD;
	dk->dk_kevent.udata = (uintptr_t)dk;
	TAILQ_INIT(&dk->dk_sources);

	dm->ds_pending_data_mask |= dk->dk_kevent.fflags;

	uint32_t flags;
	bool do_resume = _dispatch_kevent_register(&dk,
			_PTHREAD_PRIORITY_EVENT_MANAGER_FLAG, &flags);
	TAILQ_INSERT_TAIL(&dk->dk_sources,
			(dispatch_source_refs_t)dm->dm_refs, dr_list);
	dm->dm_dkev = dk;
	if (do_resume && _dispatch_kevent_resume(dm->dm_dkev, flags, 0)) {
		_dispatch_mach_notification_kevent_unregister(dm);
	}
}

static mach_port_t
_dispatch_get_thread_reply_port(void)
{
	mach_port_t reply_port, mrp = _dispatch_get_thread_mig_reply_port();
	if (mrp) {
		reply_port = mrp;
		_dispatch_debug("machport[0x%08x]: borrowed thread sync reply port",
				reply_port);
	} else {
		reply_port = mach_reply_port();
		_dispatch_set_thread_mig_reply_port(reply_port);
		_dispatch_debug("machport[0x%08x]: allocated thread sync reply port",
				reply_port);
	}
	_dispatch_debug_machport(reply_port);
	return reply_port;
}

static void
_dispatch_clear_thread_reply_port(mach_port_t reply_port)
{
	mach_port_t mrp = _dispatch_get_thread_mig_reply_port();
	if (reply_port != mrp) {
		if (mrp) {
			_dispatch_debug("machport[0x%08x]: did not clear thread sync reply "
					"port (found 0x%08x)", reply_port, mrp);
		}
		return;
	}
	_dispatch_set_thread_mig_reply_port(MACH_PORT_NULL);
	_dispatch_debug_machport(reply_port);
	_dispatch_debug("machport[0x%08x]: cleared thread sync reply port",
			reply_port);
}

static void
_dispatch_set_thread_reply_port(mach_port_t reply_port)
{
	_dispatch_debug_machport(reply_port);
	mach_port_t mrp = _dispatch_get_thread_mig_reply_port();
	if (mrp) {
		kern_return_t kr = mach_port_mod_refs(mach_task_self(), reply_port,
				MACH_PORT_RIGHT_RECEIVE, -1);
		DISPATCH_VERIFY_MIG(kr);
		dispatch_assume_zero(kr);
		_dispatch_debug("machport[0x%08x]: deallocated sync reply port "
				"(found 0x%08x)", reply_port, mrp);
	} else {
		_dispatch_set_thread_mig_reply_port(reply_port);
		_dispatch_debug("machport[0x%08x]: restored thread sync reply port",
				reply_port);
	}
}

static inline mach_port_t
_dispatch_mach_msg_get_remote_port(dispatch_object_t dou)
{
	mach_msg_header_t *hdr = _dispatch_mach_msg_get_msg(dou._dmsg);
	mach_port_t remote = hdr->msgh_remote_port;
	return remote;
}

static inline mach_port_t
_dispatch_mach_msg_get_reply_port(dispatch_object_t dou)
{
	mach_msg_header_t *hdr = _dispatch_mach_msg_get_msg(dou._dmsg);
	mach_port_t local = hdr->msgh_local_port;
	if (!MACH_PORT_VALID(local) || MACH_MSGH_BITS_LOCAL(hdr->msgh_bits) !=
			MACH_MSG_TYPE_MAKE_SEND_ONCE) return MACH_PORT_NULL;
	return local;
}

static inline void
_dispatch_mach_msg_set_reason(dispatch_mach_msg_t dmsg, mach_error_t err,
		unsigned long reason)
{
	dispatch_assert_zero(reason & ~(unsigned long)code_emask);
	dmsg->dmsg_error = ((err || !reason) ? err :
			 err_local|err_sub(0x3e0)|(mach_error_t)reason);
}

static inline unsigned long
_dispatch_mach_msg_get_reason(dispatch_mach_msg_t dmsg, mach_error_t *err_ptr)
{
	mach_error_t err = dmsg->dmsg_error;

	dmsg->dmsg_error = 0;
	if ((err & system_emask) == err_local && err_get_sub(err) == 0x3e0) {
		*err_ptr = 0;
		return err_get_code(err);
	}
	*err_ptr = err;
	return err ? DISPATCH_MACH_MESSAGE_SEND_FAILED : DISPATCH_MACH_MESSAGE_SENT;
}

static void
_dispatch_mach_msg_recv(dispatch_mach_t dm, dispatch_mach_reply_refs_t dmr,
		_dispatch_kevent_qos_s *ke, mach_msg_header_t *hdr, mach_msg_size_t siz)
{
	_dispatch_debug_machport(hdr->msgh_remote_port);
	_dispatch_debug("machport[0x%08x]: received msg id 0x%x, reply on 0x%08x",
			hdr->msgh_local_port, hdr->msgh_id, hdr->msgh_remote_port);
	bool canceled = (dm->dq_atomic_flags & DSF_CANCELED);
	if (!dmr && canceled) {
		// message received after cancellation, _dispatch_mach_kevent_merge is
		// responsible for mach channel source state (e.g. deferred deletion)
		return _dispatch_kevent_mach_msg_destroy(ke, hdr);
	}
	dispatch_mach_msg_t dmsg;
	voucher_t voucher;
	pthread_priority_t priority;
	void *ctxt = NULL;
	if (dmr) {
		_voucher_mach_msg_clear(hdr, false); // deallocate reply message voucher
		voucher = dmr->dmr_voucher;
		dmr->dmr_voucher = NULL; // transfer reference
		priority = dmr->dmr_priority;
		ctxt = dmr->dmr_ctxt;
		unsigned int options = DKEV_DISPOSE_IMMEDIATE_DELETE;
		options |= DKEV_UNREGISTER_REPLY_REMOVE;
		options |= DKEV_UNREGISTER_WAKEUP;
		if (canceled) options |= DKEV_UNREGISTER_DISCONNECTED;
		_dispatch_mach_reply_kevent_unregister(dm, dmr, options);
		ke->flags |= EV_DELETE; // remember that unregister deleted the event
		if (canceled) return;
	} else {
		voucher = voucher_create_with_mach_msg(hdr);
		priority = _voucher_get_priority(voucher);
	}
	dispatch_mach_msg_destructor_t destructor;
	destructor = (hdr == _dispatch_kevent_mach_msg_buf(ke)) ?
			DISPATCH_MACH_MSG_DESTRUCTOR_DEFAULT :
			DISPATCH_MACH_MSG_DESTRUCTOR_FREE;
	dmsg = dispatch_mach_msg_create(hdr, siz, destructor, NULL);
	if (hdr == _dispatch_kevent_mach_msg_buf(ke)) {
		_dispatch_ktrace2(DISPATCH_MACH_MSG_hdr_move, (uint64_t)hdr, (uint64_t)dmsg->dmsg_buf);
	}
	dmsg->dmsg_voucher = voucher;
	dmsg->dmsg_priority = priority;
	dmsg->do_ctxt = ctxt;
	_dispatch_mach_msg_set_reason(dmsg, 0, DISPATCH_MACH_MESSAGE_RECEIVED);
	_dispatch_voucher_debug("mach-msg[%p] create", voucher, dmsg);
	_dispatch_voucher_ktrace_dmsg_push(dmsg);
	return _dispatch_queue_push(dm->_as_dq, dmsg, dmsg->dmsg_priority);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_mach_msg_t
_dispatch_mach_msg_reply_recv(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, mach_port_t reply_port)
{
	if (slowpath(!MACH_PORT_VALID(reply_port))) {
		DISPATCH_CLIENT_CRASH(reply_port, "Invalid reply port");
	}
	void *ctxt = dmr->dmr_ctxt;
	mach_msg_header_t *hdr, *hdr2 = NULL;
	void *hdr_copyout_addr;
	mach_msg_size_t siz, msgsiz = 0;
	mach_msg_return_t kr;
	mach_msg_option_t options;
	siz = mach_vm_round_page(_dispatch_mach_recv_msg_size +
			dispatch_mach_trailer_size);
	hdr = alloca(siz);
	for (mach_vm_address_t p = mach_vm_trunc_page(hdr + vm_page_size);
			p < (mach_vm_address_t)hdr + siz; p += vm_page_size) {
		*(char*)p = 0; // ensure alloca buffer doesn't overlap with stack guard
	}
	options = DISPATCH_MACH_RCV_OPTIONS & (~MACH_RCV_VOUCHER);
retry:
	_dispatch_debug_machport(reply_port);
	_dispatch_debug("machport[0x%08x]: MACH_RCV_MSG %s", reply_port,
			(options & MACH_RCV_TIMEOUT) ? "poll" : "wait");
	kr = mach_msg(hdr, options, 0, siz, reply_port, MACH_MSG_TIMEOUT_NONE,
			MACH_PORT_NULL);
	hdr_copyout_addr = hdr;
	_dispatch_debug_machport(reply_port);
	_dispatch_debug("machport[0x%08x]: MACH_RCV_MSG (size %u, opts 0x%x) "
			"returned: %s - 0x%x", reply_port, siz, options,
			mach_error_string(kr), kr);
	switch (kr) {
	case MACH_RCV_TOO_LARGE:
		if (!fastpath(hdr->msgh_size <= UINT_MAX -
				dispatch_mach_trailer_size)) {
			DISPATCH_CLIENT_CRASH(hdr->msgh_size, "Overlarge message");
		}
		if (options & MACH_RCV_LARGE) {
			msgsiz = hdr->msgh_size + dispatch_mach_trailer_size;
			hdr2 = malloc(msgsiz);
			if (dispatch_assume(hdr2)) {
				hdr = hdr2;
				siz = msgsiz;
			}
			options |= MACH_RCV_TIMEOUT;
			options &= ~MACH_RCV_LARGE;
			goto retry;
		}
		_dispatch_log("BUG in libdispatch client: "
				"dispatch_mach_send_and_wait_for_reply: dropped message too "
				"large to fit in memory: id = 0x%x, size = %u", hdr->msgh_id,
				hdr->msgh_size);
		break;
	case MACH_RCV_INVALID_NAME: // rdar://problem/21963848
	case MACH_RCV_PORT_CHANGED: // rdar://problem/21885327
	case MACH_RCV_PORT_DIED:
		// channel was disconnected/canceled and reply port destroyed
		_dispatch_debug("machport[0x%08x]: sync reply port destroyed, ctxt %p: "
				"%s - 0x%x", reply_port, ctxt, mach_error_string(kr), kr);
		goto out;
	case MACH_MSG_SUCCESS:
		if (hdr->msgh_remote_port) {
			_dispatch_debug_machport(hdr->msgh_remote_port);
		}
		_dispatch_debug("machport[0x%08x]: received msg id 0x%x, size = %u, "
				"reply on 0x%08x", hdr->msgh_local_port, hdr->msgh_id,
				hdr->msgh_size, hdr->msgh_remote_port);
		siz = hdr->msgh_size + dispatch_mach_trailer_size;
		if (hdr2 && siz < msgsiz) {
			void *shrink = realloc(hdr2, msgsiz);
			if (shrink) hdr = hdr2 = shrink;
		}
		break;
	default:
		dispatch_assume_zero(kr);
		break;
	}
	_dispatch_mach_msg_reply_received(dm, dmr, hdr->msgh_local_port);
	hdr->msgh_local_port = MACH_PORT_NULL;
	if (slowpath((dm->dq_atomic_flags & DSF_CANCELED) || kr)) {
		if (!kr) mach_msg_destroy(hdr);
		goto out;
	}
	dispatch_mach_msg_t dmsg;
	dispatch_mach_msg_destructor_t destructor = (!hdr2) ?
			DISPATCH_MACH_MSG_DESTRUCTOR_DEFAULT :
			DISPATCH_MACH_MSG_DESTRUCTOR_FREE;
	dmsg = dispatch_mach_msg_create(hdr, siz, destructor, NULL);
	if (!hdr2 || hdr != hdr_copyout_addr) {
		_dispatch_ktrace2(DISPATCH_MACH_MSG_hdr_move, (uint64_t)hdr_copyout_addr, (uint64_t)_dispatch_mach_msg_get_msg(dmsg));
	}
	dmsg->do_ctxt = ctxt;
	return dmsg;
out:
	free(hdr2);
	return NULL;
}

static inline void
_dispatch_mach_msg_reply_received(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, mach_port_t local_port)
{
	bool removed = _dispatch_mach_reply_tryremove(dm, dmr);
	if (!MACH_PORT_VALID(local_port) || !removed) {
		// port moved/destroyed during receive, or reply waiter was never
		// registered or already removed (disconnected)
		return;
	}
	mach_port_t reply_port = _dispatch_mach_reply_get_reply_port(dmr);
	_dispatch_debug("machport[0x%08x]: unregistered for sync reply, ctxt %p",
			reply_port, dmr->dmr_ctxt);
	if (_dispatch_mach_reply_is_reply_port_owned(dmr)) {
		_dispatch_set_thread_reply_port(reply_port);
		if (local_port != reply_port) {
			DISPATCH_CLIENT_CRASH(local_port,
					"Reply received on unexpected port");
		}
		return;
	}
	mach_msg_header_t *hdr;
	dispatch_mach_msg_t dmsg;
	dmsg = dispatch_mach_msg_create(NULL, sizeof(mach_msg_header_t),
			DISPATCH_MACH_MSG_DESTRUCTOR_DEFAULT, &hdr);
	hdr->msgh_local_port = local_port;
	dmsg->dmsg_voucher = dmr->dmr_voucher;
	dmr->dmr_voucher = NULL;  // transfer reference
	dmsg->dmsg_priority = dmr->dmr_priority;
	dmsg->do_ctxt = dmr->dmr_ctxt;
	_dispatch_mach_msg_set_reason(dmsg, 0, DISPATCH_MACH_REPLY_RECEIVED);
	return _dispatch_queue_push(dm->_as_dq, dmsg, dmsg->dmsg_priority);
}

static inline void
_dispatch_mach_msg_disconnected(dispatch_mach_t dm, mach_port_t local_port,
		mach_port_t remote_port)
{
	mach_msg_header_t *hdr;
	dispatch_mach_msg_t dmsg;
	dmsg = dispatch_mach_msg_create(NULL, sizeof(mach_msg_header_t),
			DISPATCH_MACH_MSG_DESTRUCTOR_DEFAULT, &hdr);
	if (local_port) hdr->msgh_local_port = local_port;
	if (remote_port) hdr->msgh_remote_port = remote_port;
	_dispatch_mach_msg_set_reason(dmsg, 0, DISPATCH_MACH_DISCONNECTED);
	_dispatch_debug("machport[0x%08x]: %s right disconnected", local_port ?
			local_port : remote_port, local_port ? "receive" : "send");
	return _dispatch_queue_push(dm->_as_dq, dmsg, dmsg->dmsg_priority);
}

static inline dispatch_mach_msg_t
_dispatch_mach_msg_create_reply_disconnected(dispatch_object_t dou,
		dispatch_mach_reply_refs_t dmr)
{
	dispatch_mach_msg_t dmsg = dou._dmsg, dmsgr;
	mach_port_t reply_port = dmsg ? dmsg->dmsg_reply :
			_dispatch_mach_reply_get_reply_port(dmr);
	voucher_t v;

	if (!reply_port) {
		if (!dmsg) {
			v = dmr->dmr_voucher;
			dmr->dmr_voucher = NULL; // transfer reference
			if (v) _voucher_release(v);
		}
		return NULL;
	}

	if (dmsg) {
		v = dmsg->dmsg_voucher;
		if (v) _voucher_retain(v);
	} else {
		v = dmr->dmr_voucher;
		dmr->dmr_voucher = NULL; // transfer reference
	}

	if ((dmsg && (dmsg->dmsg_options & DISPATCH_MACH_WAIT_FOR_REPLY) &&
			(dmsg->dmsg_options & DISPATCH_MACH_OWNED_REPLY_PORT)) ||
			(dmr && !dmr->dmr_dkev &&
			_dispatch_mach_reply_is_reply_port_owned(dmr))) {
		if (v) _voucher_release(v);
		// deallocate owned reply port to break _dispatch_mach_msg_reply_recv
		// out of waiting in mach_msg(MACH_RCV_MSG)
		kern_return_t kr = mach_port_mod_refs(mach_task_self(), reply_port,
				MACH_PORT_RIGHT_RECEIVE, -1);
		DISPATCH_VERIFY_MIG(kr);
		dispatch_assume_zero(kr);
		return NULL;
	}

	mach_msg_header_t *hdr;
	dmsgr = dispatch_mach_msg_create(NULL, sizeof(mach_msg_header_t),
			DISPATCH_MACH_MSG_DESTRUCTOR_DEFAULT, &hdr);
	dmsgr->dmsg_voucher = v;
	hdr->msgh_local_port = reply_port;
	if (dmsg) {
		dmsgr->dmsg_priority = dmsg->dmsg_priority;
		dmsgr->do_ctxt = dmsg->do_ctxt;
	} else {
		dmsgr->dmsg_priority = dmr->dmr_priority;
		dmsgr->do_ctxt = dmr->dmr_ctxt;
	}
	_dispatch_mach_msg_set_reason(dmsgr, 0, DISPATCH_MACH_DISCONNECTED);
	_dispatch_debug("machport[0x%08x]: reply disconnected, ctxt %p",
			hdr->msgh_local_port, dmsgr->do_ctxt);
	return dmsgr;
}

DISPATCH_NOINLINE
static void
_dispatch_mach_msg_not_sent(dispatch_mach_t dm, dispatch_object_t dou)
{
	dispatch_mach_msg_t dmsg = dou._dmsg, dmsgr;
	mach_msg_header_t *msg = _dispatch_mach_msg_get_msg(dmsg);
	mach_msg_option_t msg_opts = dmsg->dmsg_options;
	_dispatch_debug("machport[0x%08x]: not sent msg id 0x%x, ctxt %p, "
			"msg_opts 0x%x, kvoucher 0x%08x, reply on 0x%08x",
			msg->msgh_remote_port, msg->msgh_id, dmsg->do_ctxt,
			msg_opts, msg->msgh_voucher_port, dmsg->dmsg_reply);
	unsigned long reason = (msg_opts & DISPATCH_MACH_REGISTER_FOR_REPLY) ?
			0 : DISPATCH_MACH_MESSAGE_NOT_SENT;
	dmsgr = _dispatch_mach_msg_create_reply_disconnected(dmsg, NULL);
	_dispatch_mach_msg_set_reason(dmsg, 0, reason);
	_dispatch_queue_push(dm->_as_dq, dmsg, dmsg->dmsg_priority);
	if (dmsgr) _dispatch_queue_push(dm->_as_dq, dmsgr, dmsgr->dmsg_priority);
}

DISPATCH_NOINLINE
static uint32_t
_dispatch_mach_msg_send(dispatch_mach_t dm, dispatch_object_t dou,
		dispatch_mach_reply_refs_t dmr, pthread_priority_t pp,
		dispatch_mach_send_invoke_flags_t send_flags)
{
	dispatch_mach_send_refs_t dr = dm->dm_refs;
	dispatch_mach_msg_t dmsg = dou._dmsg, dmsgr = NULL;
	voucher_t voucher = dmsg->dmsg_voucher;
	mach_voucher_t ipc_kvoucher = MACH_VOUCHER_NULL;
	uint32_t send_status = 0;
	bool clear_voucher = false, kvoucher_move_send = false;
	mach_msg_header_t *msg = _dispatch_mach_msg_get_msg(dmsg);
	bool is_reply = (MACH_MSGH_BITS_REMOTE(msg->msgh_bits) ==
			MACH_MSG_TYPE_MOVE_SEND_ONCE);
	mach_port_t reply_port = dmsg->dmsg_reply;
	if (!is_reply) {
		dr->dm_needs_mgr = 0;
		if (unlikely(dr->dm_checkin && dmsg != dr->dm_checkin)) {
			// send initial checkin message
			if (dm->dm_dkev && slowpath(_dispatch_queue_get_current() !=
					&_dispatch_mgr_q)) {
				// send kevent must be uninstalled on the manager queue
				dr->dm_needs_mgr = 1;
				goto out;
			}
			if (unlikely(!_dispatch_mach_msg_send(dm,
					dr->dm_checkin, NULL, pp, DM_SEND_INVOKE_NONE))) {
				goto out;
			}
			dr->dm_checkin = NULL;
		}
	}
	mach_msg_return_t kr = 0;
	mach_msg_option_t opts = 0, msg_opts = dmsg->dmsg_options;
	if (!(msg_opts & DISPATCH_MACH_REGISTER_FOR_REPLY)) {
		mach_msg_priority_t msg_priority = MACH_MSG_PRIORITY_UNSPECIFIED;
		opts = MACH_SEND_MSG | (msg_opts & ~DISPATCH_MACH_OPTIONS_MASK);
		if (!is_reply) {
			if (dmsg != dr->dm_checkin) {
				msg->msgh_remote_port = dr->dm_send;
			}
			if (_dispatch_queue_get_current() == &_dispatch_mgr_q) {
				if (slowpath(!dm->dm_dkev)) {
					_dispatch_mach_notification_kevent_register(dm,
							msg->msgh_remote_port);
				}
				if (fastpath(dm->dm_dkev)) {
					if (DISPATCH_MACH_NOTIFICATION_ARMED(dm->dm_dkev)) {
						goto out;
					}
					opts |= MACH_SEND_NOTIFY;
				}
			}
			opts |= MACH_SEND_TIMEOUT;
			if (dmsg->dmsg_priority != _voucher_get_priority(voucher)) {
				ipc_kvoucher = _voucher_create_mach_voucher_with_priority(
						voucher, dmsg->dmsg_priority);
			}
			_dispatch_voucher_debug("mach-msg[%p] msg_set", voucher, dmsg);
			if (ipc_kvoucher) {
				kvoucher_move_send = true;
				clear_voucher = _voucher_mach_msg_set_mach_voucher(msg,
						ipc_kvoucher, kvoucher_move_send);
			} else {
				clear_voucher = _voucher_mach_msg_set(msg, voucher);
			}
			if (pp && _dispatch_evfilt_machport_direct_enabled) {
				opts |= MACH_SEND_OVERRIDE;
				msg_priority = (mach_msg_priority_t)pp;
			}
		}
		_dispatch_debug_machport(msg->msgh_remote_port);
		if (reply_port) _dispatch_debug_machport(reply_port);
		if (msg_opts & DISPATCH_MACH_WAIT_FOR_REPLY) {
			if (msg_opts & DISPATCH_MACH_OWNED_REPLY_PORT) {
				_dispatch_clear_thread_reply_port(reply_port);
			}
			_dispatch_mach_reply_waiter_register(dm, dmr, reply_port, dmsg,
					msg_opts);
		}
		kr = mach_msg(msg, opts, msg->msgh_size, 0, MACH_PORT_NULL, 0,
				msg_priority);
		_dispatch_debug("machport[0x%08x]: sent msg id 0x%x, ctxt %p, "
				"opts 0x%x, msg_opts 0x%x, kvoucher 0x%08x, reply on 0x%08x: "
				"%s - 0x%x", msg->msgh_remote_port, msg->msgh_id, dmsg->do_ctxt,
				opts, msg_opts, msg->msgh_voucher_port, reply_port,
				mach_error_string(kr), kr);
		if (unlikely(kr && (msg_opts & DISPATCH_MACH_WAIT_FOR_REPLY))) {
			_dispatch_mach_reply_waiter_unregister(dm, dmr,
					DKEV_UNREGISTER_REPLY_REMOVE);
		}
		if (clear_voucher) {
			if (kr == MACH_SEND_INVALID_VOUCHER && msg->msgh_voucher_port) {
				DISPATCH_CLIENT_CRASH(kr, "Voucher port corruption");
			}
			mach_voucher_t kv;
			kv = _voucher_mach_msg_clear(msg, kvoucher_move_send);
			if (kvoucher_move_send) ipc_kvoucher = kv;
		}
	}
	if (kr == MACH_SEND_TIMED_OUT && (opts & MACH_SEND_TIMEOUT)) {
		if (opts & MACH_SEND_NOTIFY) {
			_dispatch_debug("machport[0x%08x]: send-possible notification "
					"armed", (mach_port_t)dm->dm_dkev->dk_kevent.ident);
			DISPATCH_MACH_NOTIFICATION_ARMED(dm->dm_dkev) = 1;
		} else {
			// send kevent must be installed on the manager queue
			dr->dm_needs_mgr = 1;
		}
		if (ipc_kvoucher) {
			_dispatch_kvoucher_debug("reuse on re-send", ipc_kvoucher);
			voucher_t ipc_voucher;
			ipc_voucher = _voucher_create_with_priority_and_mach_voucher(
					voucher, dmsg->dmsg_priority, ipc_kvoucher);
			_dispatch_voucher_debug("mach-msg[%p] replace voucher[%p]",
					ipc_voucher, dmsg, voucher);
			if (dmsg->dmsg_voucher) _voucher_release(dmsg->dmsg_voucher);
			dmsg->dmsg_voucher = ipc_voucher;
		}
		goto out;
	} else if (ipc_kvoucher && (kr || !kvoucher_move_send)) {
		_voucher_dealloc_mach_voucher(ipc_kvoucher);
	}
	if (!(msg_opts & DISPATCH_MACH_WAIT_FOR_REPLY) && !kr && reply_port &&
			!(dm->ds_dkev && dm->ds_dkev->dk_kevent.ident == reply_port)) {
		if (!dm->ds_is_direct_kevent &&
				_dispatch_queue_get_current() != &_dispatch_mgr_q) {
			// reply receive kevent must be installed on the manager queue
			dr->dm_needs_mgr = 1;
			dmsg->dmsg_options = msg_opts | DISPATCH_MACH_REGISTER_FOR_REPLY;
			goto out;
		}
		_dispatch_mach_reply_kevent_register(dm, reply_port, dmsg);
	}
	if (unlikely(!is_reply && dmsg == dr->dm_checkin && dm->dm_dkev)) {
		_dispatch_mach_notification_kevent_unregister(dm);
	}
	if (slowpath(kr)) {
		// Send failed, so reply was never registered <rdar://problem/14309159>
		dmsgr = _dispatch_mach_msg_create_reply_disconnected(dmsg, NULL);
	}
	_dispatch_mach_msg_set_reason(dmsg, kr, 0);
	if ((send_flags & DM_SEND_INVOKE_IMMEDIATE_SEND) &&
			(msg_opts & DISPATCH_MACH_RETURN_IMMEDIATE_SEND_RESULT)) {
		// Return sent message synchronously <rdar://problem/25947334>
		send_status |= DM_SEND_STATUS_RETURNING_IMMEDIATE_SEND_RESULT;
	} else {
		_dispatch_queue_push(dm->_as_dq, dmsg, dmsg->dmsg_priority);
	}
	if (dmsgr) _dispatch_queue_push(dm->_as_dq, dmsgr, dmsgr->dmsg_priority);
	send_status |= DM_SEND_STATUS_SUCCESS;
out:
	return send_status;
}

#pragma mark -
#pragma mark dispatch_mach_send_refs_t

static void _dispatch_mach_cancel(dispatch_mach_t dm);
static void _dispatch_mach_send_barrier_drain_push(dispatch_mach_t dm,
		pthread_priority_t pp);

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dm_state_get_override(uint64_t dm_state)
{
	dm_state &= DISPATCH_MACH_STATE_OVERRIDE_MASK;
	return (pthread_priority_t)(dm_state >> 32);
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dm_state_override_from_priority(pthread_priority_t pp)
{
	uint64_t pp_state = pp & _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	return pp_state << 32;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dm_state_needs_override(uint64_t dm_state, uint64_t pp_state)
{
	return (pp_state > (dm_state & DISPATCH_MACH_STATE_OVERRIDE_MASK));
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dm_state_merge_override(uint64_t dm_state, uint64_t pp_state)
{
	if (_dm_state_needs_override(dm_state, pp_state)) {
		dm_state &= ~DISPATCH_MACH_STATE_OVERRIDE_MASK;
		dm_state |= pp_state;
		dm_state |= DISPATCH_MACH_STATE_DIRTY;
		dm_state |= DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
	}
	return dm_state;
}

#define _dispatch_mach_send_push_update_tail(dr, tail) \
		os_mpsc_push_update_tail(dr, dm, tail, do_next)
#define _dispatch_mach_send_push_update_head(dr, head) \
		os_mpsc_push_update_head(dr, dm, head)
#define _dispatch_mach_send_get_head(dr) \
		os_mpsc_get_head(dr, dm)
#define _dispatch_mach_send_unpop_head(dr, dc, dc_next) \
		os_mpsc_undo_pop_head(dr, dm, dc, dc_next, do_next)
#define _dispatch_mach_send_pop_head(dr, head) \
		os_mpsc_pop_head(dr, dm, head, do_next)

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_mach_send_push_inline(dispatch_mach_send_refs_t dr,
		dispatch_object_t dou)
{
	if (_dispatch_mach_send_push_update_tail(dr, dou._do)) {
		_dispatch_mach_send_push_update_head(dr, dou._do);
		return true;
	}
	return false;
}

DISPATCH_NOINLINE
static bool
_dispatch_mach_send_drain(dispatch_mach_t dm, dispatch_invoke_flags_t flags,
		dispatch_mach_send_invoke_flags_t send_flags)
{
	dispatch_mach_send_refs_t dr = dm->dm_refs;
	dispatch_mach_reply_refs_t dmr;
	dispatch_mach_msg_t dmsg;
	struct dispatch_object_s *dc = NULL, *next_dc = NULL;
	pthread_priority_t pp = _dm_state_get_override(dr->dm_state);
	uint64_t old_state, new_state;
	uint32_t send_status;
	bool needs_mgr, disconnecting, returning_send_result = false;

again:
	needs_mgr = false; disconnecting = false;
	while (dr->dm_tail) {
		dc = _dispatch_mach_send_get_head(dr);
		do {
			dispatch_mach_send_invoke_flags_t sf = send_flags;
			// Only request immediate send result for the first message
			send_flags &= ~DM_SEND_INVOKE_IMMEDIATE_SEND_MASK;
			next_dc = _dispatch_mach_send_pop_head(dr, dc);
			if (_dispatch_object_has_type(dc,
					DISPATCH_CONTINUATION_TYPE(MACH_SEND_BARRIER))) {
				if (!(send_flags & DM_SEND_INVOKE_CAN_RUN_BARRIER)) {
					goto partial_drain;
				}
				_dispatch_continuation_pop(dc, dm->_as_dq, flags);
				continue;
			}
			if (_dispatch_object_is_slow_item(dc)) {
				dmsg = ((dispatch_continuation_t)dc)->dc_data;
				dmr = ((dispatch_continuation_t)dc)->dc_other;
			} else if (_dispatch_object_has_vtable(dc)) {
				dmsg = (dispatch_mach_msg_t)dc;
				dmr = NULL;
			} else {
				if ((dm->dm_dkev || !dm->ds_is_direct_kevent) &&
						(_dispatch_queue_get_current() != &_dispatch_mgr_q)) {
					// send kevent must be uninstalled on the manager queue
					needs_mgr = true;
					goto partial_drain;
				}
				if (unlikely(!_dispatch_mach_reconnect_invoke(dm, dc))) {
					disconnecting = true;
					goto partial_drain;
				}
				continue;
			}
			_dispatch_voucher_ktrace_dmsg_pop(dmsg);
			if (unlikely(dr->dm_disconnect_cnt ||
					(dm->dq_atomic_flags & DSF_CANCELED))) {
				_dispatch_mach_msg_not_sent(dm, dmsg);
				continue;
			}
			send_status = _dispatch_mach_msg_send(dm, dmsg, dmr, pp, sf);
			if (unlikely(!send_status)) {
				goto partial_drain;
			}
			if (send_status & DM_SEND_STATUS_RETURNING_IMMEDIATE_SEND_RESULT) {
				returning_send_result = true;
			}
		} while ((dc = next_dc));
	}

	os_atomic_rmw_loop2o(dr, dm_state, old_state, new_state, release, {
		if (old_state & DISPATCH_MACH_STATE_DIRTY) {
			new_state = old_state;
			new_state &= ~DISPATCH_MACH_STATE_DIRTY;
			new_state &= ~DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
			new_state &= ~DISPATCH_MACH_STATE_PENDING_BARRIER;
		} else {
			// unlock
			new_state = 0;
		}
	});
	goto out;

partial_drain:
	// if this is not a complete drain, we must undo some things
	_dispatch_mach_send_unpop_head(dr, dc, next_dc);

	if (_dispatch_object_has_type(dc,
			DISPATCH_CONTINUATION_TYPE(MACH_SEND_BARRIER))) {
		os_atomic_rmw_loop2o(dr, dm_state, old_state, new_state, release, {
			new_state = old_state;
			new_state |= DISPATCH_MACH_STATE_DIRTY;
			new_state |= DISPATCH_MACH_STATE_PENDING_BARRIER;
			new_state &= ~DISPATCH_MACH_STATE_UNLOCK_MASK;
		});
	} else {
		os_atomic_rmw_loop2o(dr, dm_state, old_state, new_state, release, {
			new_state = old_state;
			if (old_state & (DISPATCH_MACH_STATE_DIRTY |
					DISPATCH_MACH_STATE_RECEIVED_OVERRIDE)) {
				new_state &= ~DISPATCH_MACH_STATE_DIRTY;
				new_state &= ~DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
				new_state &= ~DISPATCH_MACH_STATE_PENDING_BARRIER;
			} else {
				new_state |= DISPATCH_MACH_STATE_DIRTY;
				new_state &= ~DISPATCH_MACH_STATE_UNLOCK_MASK;
			}
		});
	}

out:
	if (old_state & DISPATCH_MACH_STATE_RECEIVED_OVERRIDE) {
		// Ensure that the root queue sees that this thread was overridden.
		_dispatch_set_defaultpriority_override();
	}

	if (unlikely(new_state & DISPATCH_MACH_STATE_UNLOCK_MASK)) {
		os_atomic_thread_fence(acquire);
		pp = _dm_state_get_override(new_state);
		goto again;
	}

	if (new_state & DISPATCH_MACH_STATE_PENDING_BARRIER) {
		pp = _dm_state_get_override(new_state);
		_dispatch_mach_send_barrier_drain_push(dm, pp);
	} else {
		if (needs_mgr) {
			pp = _dm_state_get_override(new_state);
		} else {
			pp = 0;
		}
		if (!disconnecting) dx_wakeup(dm, pp, DISPATCH_WAKEUP_FLUSH);
	}
	return returning_send_result;
}

DISPATCH_NOINLINE
static void
_dispatch_mach_send_invoke(dispatch_mach_t dm,
		dispatch_invoke_flags_t flags,
		dispatch_mach_send_invoke_flags_t send_flags)
{
	dispatch_lock_owner tid_self = _dispatch_tid_self();
	uint64_t old_state, new_state;
	pthread_priority_t pp_floor;

	uint64_t canlock_mask = DISPATCH_MACH_STATE_UNLOCK_MASK;
	uint64_t canlock_state = 0;

	if (send_flags & DM_SEND_INVOKE_NEEDS_BARRIER) {
		canlock_mask |= DISPATCH_MACH_STATE_PENDING_BARRIER;
		canlock_state = DISPATCH_MACH_STATE_PENDING_BARRIER;
	} else if (!(send_flags & DM_SEND_INVOKE_CAN_RUN_BARRIER)) {
		canlock_mask |= DISPATCH_MACH_STATE_PENDING_BARRIER;
	}

	if (flags & DISPATCH_INVOKE_MANAGER_DRAIN) {
		pp_floor = 0;
	} else {
		// _dispatch_queue_class_invoke will have applied the queue override
		// (if any) before we get here. Else use the default base priority
		// as an estimation of the priority we already asked for.
		pp_floor = dm->_as_dq->dq_override;
		if (!pp_floor) {
			pp_floor = _dispatch_get_defaultpriority();
			pp_floor &= _PTHREAD_PRIORITY_QOS_CLASS_MASK;
		}
	}

retry:
	os_atomic_rmw_loop2o(dm->dm_refs, dm_state, old_state, new_state, acquire, {
		new_state = old_state;
		if (unlikely((old_state & canlock_mask) != canlock_state)) {
			if (!(send_flags & DM_SEND_INVOKE_FLUSH)) {
				os_atomic_rmw_loop_give_up(break);
			}
			new_state |= DISPATCH_MACH_STATE_DIRTY;
		} else {
			if (likely(pp_floor)) {
				pthread_priority_t pp = _dm_state_get_override(old_state);
				if (unlikely(pp > pp_floor)) {
					os_atomic_rmw_loop_give_up({
						_dispatch_wqthread_override_start(tid_self, pp);
						// Ensure that the root queue sees
						// that this thread was overridden.
						_dispatch_set_defaultpriority_override();
						pp_floor = pp;
						goto retry;
					});
				}
			}
			new_state |= tid_self;
			new_state &= ~DISPATCH_MACH_STATE_DIRTY;
			new_state &= ~DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
			new_state &= ~DISPATCH_MACH_STATE_PENDING_BARRIER;
		}
	});

	if (unlikely((old_state & canlock_mask) != canlock_state)) {
		return;
	}
	if (send_flags & DM_SEND_INVOKE_CANCEL) {
		_dispatch_mach_cancel(dm);
	}
	_dispatch_mach_send_drain(dm, flags, send_flags);
}

DISPATCH_NOINLINE
void
_dispatch_mach_send_barrier_drain_invoke(dispatch_continuation_t dc,
		dispatch_invoke_flags_t flags)
{
	dispatch_mach_t dm = (dispatch_mach_t)_dispatch_queue_get_current();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT;
	dispatch_thread_frame_s dtf;

	DISPATCH_COMPILER_CAN_ASSUME(dc->dc_priority == DISPATCH_NO_PRIORITY);
	DISPATCH_COMPILER_CAN_ASSUME(dc->dc_voucher == DISPATCH_NO_VOUCHER);
	// hide the mach channel (see _dispatch_mach_barrier_invoke comment)
	_dispatch_thread_frame_stash(&dtf);
	_dispatch_continuation_pop_forwarded(dc, DISPATCH_NO_VOUCHER, dc_flags,{
		_dispatch_mach_send_invoke(dm, flags,
				DM_SEND_INVOKE_NEEDS_BARRIER | DM_SEND_INVOKE_CAN_RUN_BARRIER);
	});
	_dispatch_thread_frame_unstash(&dtf);
}

DISPATCH_NOINLINE
static void
_dispatch_mach_send_barrier_drain_push(dispatch_mach_t dm,
		pthread_priority_t pp)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();

	dc->do_vtable = DC_VTABLE(MACH_SEND_BARRRIER_DRAIN);
	dc->dc_func = NULL;
	dc->dc_ctxt = NULL;
	dc->dc_voucher = DISPATCH_NO_VOUCHER;
	dc->dc_priority = DISPATCH_NO_PRIORITY;
	return _dispatch_queue_push(dm->_as_dq, dc, pp);
}

DISPATCH_NOINLINE
static void
_dispatch_mach_send_push(dispatch_mach_t dm, dispatch_continuation_t dc,
		pthread_priority_t pp)
{
	dispatch_mach_send_refs_t dr = dm->dm_refs;
	uint64_t pp_state, old_state, new_state, state_flags = 0;
	dispatch_lock_owner owner;
	bool wakeup;

	// <rdar://problem/25896179> when pushing a send barrier that destroys
	// the last reference to this channel, and the send queue is already
	// draining on another thread, the send barrier may run as soon as
	// _dispatch_mach_send_push_inline() returns.
	_dispatch_retain(dm);
	pp_state = _dm_state_override_from_priority(pp);

	wakeup = _dispatch_mach_send_push_inline(dr, dc);
	if (wakeup) {
		state_flags = DISPATCH_MACH_STATE_DIRTY;
		if (dc->do_vtable == DC_VTABLE(MACH_SEND_BARRIER)) {
			state_flags |= DISPATCH_MACH_STATE_PENDING_BARRIER;
		}
	}

	if (state_flags) {
		os_atomic_rmw_loop2o(dr, dm_state, old_state, new_state, release, {
			new_state = _dm_state_merge_override(old_state, pp_state);
			new_state |= state_flags;
		});
	} else {
		os_atomic_rmw_loop2o(dr, dm_state, old_state, new_state, relaxed, {
			new_state = _dm_state_merge_override(old_state, pp_state);
			if (old_state == new_state) {
				os_atomic_rmw_loop_give_up(break);
			}
		});
	}

	pp = _dm_state_get_override(new_state);
	owner = _dispatch_lock_owner((dispatch_lock)old_state);
	if (owner) {
		if (_dm_state_needs_override(old_state, pp_state)) {
			_dispatch_wqthread_override_start_check_owner(owner, pp,
					&dr->dm_state_lock.dul_lock);
		}
		return _dispatch_release_tailcall(dm);
	}

	dispatch_wakeup_flags_t wflags = 0;
	if (state_flags & DISPATCH_MACH_STATE_PENDING_BARRIER) {
		_dispatch_mach_send_barrier_drain_push(dm, pp);
	} else if (wakeup || dr->dm_disconnect_cnt ||
			(dm->dq_atomic_flags & DSF_CANCELED)) {
		wflags = DISPATCH_WAKEUP_FLUSH | DISPATCH_WAKEUP_CONSUME;
	} else if (old_state & DISPATCH_MACH_STATE_PENDING_BARRIER) {
		wflags = DISPATCH_WAKEUP_OVERRIDING | DISPATCH_WAKEUP_CONSUME;
	}
	if (wflags) {
		return dx_wakeup(dm, pp, wflags);
	}
	return _dispatch_release_tailcall(dm);
}

DISPATCH_NOINLINE
static bool
_dispatch_mach_send_push_and_trydrain(dispatch_mach_t dm,
		dispatch_object_t dou, pthread_priority_t pp,
		dispatch_mach_send_invoke_flags_t send_flags)
{
	dispatch_mach_send_refs_t dr = dm->dm_refs;
	dispatch_lock_owner tid_self = _dispatch_tid_self();
	uint64_t pp_state, old_state, new_state, canlock_mask, state_flags = 0;
	dispatch_lock_owner owner;

	pp_state = _dm_state_override_from_priority(pp);
	bool wakeup = _dispatch_mach_send_push_inline(dr, dou);
	if (wakeup) {
		state_flags = DISPATCH_MACH_STATE_DIRTY;
	}

	if (unlikely(dr->dm_disconnect_cnt ||
			(dm->dq_atomic_flags & DSF_CANCELED))) {
		os_atomic_rmw_loop2o(dr, dm_state, old_state, new_state, release, {
			new_state = _dm_state_merge_override(old_state, pp_state);
			new_state |= state_flags;
		});
		dx_wakeup(dm, pp, DISPATCH_WAKEUP_FLUSH);
		return false;
	}

	canlock_mask = DISPATCH_MACH_STATE_UNLOCK_MASK |
			DISPATCH_MACH_STATE_PENDING_BARRIER;
	if (state_flags) {
		os_atomic_rmw_loop2o(dr, dm_state, old_state, new_state, seq_cst, {
			new_state = _dm_state_merge_override(old_state, pp_state);
			new_state |= state_flags;
			if (likely((old_state & canlock_mask) == 0)) {
				new_state |= tid_self;
				new_state &= ~DISPATCH_MACH_STATE_DIRTY;
				new_state &= ~DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
				new_state &= ~DISPATCH_MACH_STATE_PENDING_BARRIER;
			}
		});
	} else {
		os_atomic_rmw_loop2o(dr, dm_state, old_state, new_state, acquire, {
			new_state = _dm_state_merge_override(old_state, pp_state);
			if (new_state == old_state) {
				os_atomic_rmw_loop_give_up(return false);
			}
			if (likely((old_state & canlock_mask) == 0)) {
				new_state |= tid_self;
				new_state &= ~DISPATCH_MACH_STATE_DIRTY;
				new_state &= ~DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
				new_state &= ~DISPATCH_MACH_STATE_PENDING_BARRIER;
			}
		});
	}

	owner = _dispatch_lock_owner((dispatch_lock)old_state);
	if (owner) {
		if (_dm_state_needs_override(old_state, pp_state)) {
			_dispatch_wqthread_override_start_check_owner(owner, pp,
					&dr->dm_state_lock.dul_lock);
		}
		return false;
	}

	if (old_state & DISPATCH_MACH_STATE_PENDING_BARRIER) {
		dx_wakeup(dm, pp, DISPATCH_WAKEUP_OVERRIDING);
		return false;
	}

	// Ensure our message is still at the head of the queue and has not already
	// been dequeued by another thread that raced us to the send queue lock.
	// A plain load of the head and comparison against our object pointer is
	// sufficient.
	if (unlikely(!(wakeup && dou._do == dr->dm_head))) {
		// Don't request immediate send result for messages we don't own
		send_flags &= ~DM_SEND_INVOKE_IMMEDIATE_SEND_MASK;
	}
	return _dispatch_mach_send_drain(dm, DISPATCH_INVOKE_NONE, send_flags);
}

static void
_dispatch_mach_merge_notification_kevent(dispatch_mach_t dm,
		const _dispatch_kevent_qos_s *ke)
{
	if (!(ke->fflags & dm->ds_pending_data_mask)) {
		return;
	}
	_dispatch_mach_send_invoke(dm, DISPATCH_INVOKE_MANAGER_DRAIN,
			DM_SEND_INVOKE_FLUSH);
}

#pragma mark -
#pragma mark dispatch_mach_t

static inline mach_msg_option_t
_dispatch_mach_checkin_options(void)
{
	mach_msg_option_t options = 0;
#if DISPATCH_USE_CHECKIN_NOIMPORTANCE
	options = MACH_SEND_NOIMPORTANCE; // <rdar://problem/16996737>
#endif
	return options;
}


static inline mach_msg_option_t
_dispatch_mach_send_options(void)
{
	mach_msg_option_t options = 0;
	return options;
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_mach_priority_propagate(mach_msg_option_t options)
{
#if DISPATCH_USE_NOIMPORTANCE_QOS
	if (options & MACH_SEND_NOIMPORTANCE) return 0;
#else
	(void)options;
#endif
	return _dispatch_priority_propagate();
}

DISPATCH_NOINLINE
static bool
_dispatch_mach_send_msg(dispatch_mach_t dm, dispatch_mach_msg_t dmsg,
		dispatch_continuation_t dc_wait, mach_msg_option_t options)
{
	dispatch_mach_send_refs_t dr = dm->dm_refs;
	if (slowpath(dmsg->do_next != DISPATCH_OBJECT_LISTLESS)) {
		DISPATCH_CLIENT_CRASH(dmsg->do_next, "Message already enqueued");
	}
	dispatch_retain(dmsg);
	pthread_priority_t priority = _dispatch_mach_priority_propagate(options);
	options |= _dispatch_mach_send_options();
	dmsg->dmsg_options = options;
	mach_msg_header_t *msg = _dispatch_mach_msg_get_msg(dmsg);
	dmsg->dmsg_reply = _dispatch_mach_msg_get_reply_port(dmsg);
	bool is_reply = (MACH_MSGH_BITS_REMOTE(msg->msgh_bits) ==
			MACH_MSG_TYPE_MOVE_SEND_ONCE);
	dmsg->dmsg_priority = priority;
	dmsg->dmsg_voucher = _voucher_copy();
	_dispatch_voucher_debug("mach-msg[%p] set", dmsg->dmsg_voucher, dmsg);

	uint32_t send_status;
	bool returning_send_result = false;
	dispatch_mach_send_invoke_flags_t send_flags = DM_SEND_INVOKE_NONE;
	if (options & DISPATCH_MACH_RETURN_IMMEDIATE_SEND_RESULT) {
		send_flags = DM_SEND_INVOKE_IMMEDIATE_SEND;
	}
	if (is_reply && !dmsg->dmsg_reply && !dr->dm_disconnect_cnt &&
			!(dm->dq_atomic_flags & DSF_CANCELED)) {
		// replies are sent to a send-once right and don't need the send queue
		dispatch_assert(!dc_wait);
		send_status = _dispatch_mach_msg_send(dm, dmsg, NULL, 0, send_flags);
		dispatch_assert(send_status);
		returning_send_result = !!(send_status &
				DM_SEND_STATUS_RETURNING_IMMEDIATE_SEND_RESULT);
	} else {
		_dispatch_voucher_ktrace_dmsg_push(dmsg);
		priority &= _PTHREAD_PRIORITY_QOS_CLASS_MASK;
		dispatch_object_t dou = { ._dmsg = dmsg };
		if (dc_wait) dou._dc = dc_wait;
		returning_send_result = _dispatch_mach_send_push_and_trydrain(dm, dou,
				priority, send_flags);
	}
	if (returning_send_result) {
		_dispatch_voucher_debug("mach-msg[%p] clear", dmsg->dmsg_voucher, dmsg);
		if (dmsg->dmsg_voucher) _voucher_release(dmsg->dmsg_voucher);
		dmsg->dmsg_voucher = NULL;
		dmsg->do_next = DISPATCH_OBJECT_LISTLESS;
		dispatch_release(dmsg);
	}
	return returning_send_result;
}

DISPATCH_NOINLINE
void
dispatch_mach_send(dispatch_mach_t dm, dispatch_mach_msg_t dmsg,
		mach_msg_option_t options)
{
	dispatch_assert_zero(options & DISPATCH_MACH_OPTIONS_MASK);
	options &= ~DISPATCH_MACH_OPTIONS_MASK;
	bool returned_send_result = _dispatch_mach_send_msg(dm, dmsg, NULL,options);
	dispatch_assert(!returned_send_result);
}

DISPATCH_NOINLINE
void
dispatch_mach_send_with_result(dispatch_mach_t dm, dispatch_mach_msg_t dmsg,
		mach_msg_option_t options, dispatch_mach_send_flags_t send_flags,
		dispatch_mach_reason_t *send_result, mach_error_t *send_error)
{
	if (unlikely(send_flags != DISPATCH_MACH_SEND_DEFAULT)) {
		DISPATCH_CLIENT_CRASH(send_flags, "Invalid send flags");
	}
	dispatch_assert_zero(options & DISPATCH_MACH_OPTIONS_MASK);
	options &= ~DISPATCH_MACH_OPTIONS_MASK;
	options |= DISPATCH_MACH_RETURN_IMMEDIATE_SEND_RESULT;
	bool returned_send_result = _dispatch_mach_send_msg(dm, dmsg, NULL,options);
	unsigned long reason = DISPATCH_MACH_NEEDS_DEFERRED_SEND;
	mach_error_t err = 0;
	if (returned_send_result) {
		reason = _dispatch_mach_msg_get_reason(dmsg, &err);
	}
	*send_result = reason;
	*send_error = err;
}

static inline
dispatch_mach_msg_t
_dispatch_mach_send_and_wait_for_reply(dispatch_mach_t dm,
		dispatch_mach_msg_t dmsg, mach_msg_option_t options,
		bool *returned_send_result)
{
	mach_port_t reply_port = _dispatch_mach_msg_get_reply_port(dmsg);
	if (!reply_port) {
		// use per-thread mach reply port <rdar://24597802>
		reply_port = _dispatch_get_thread_reply_port();
		mach_msg_header_t *hdr = _dispatch_mach_msg_get_msg(dmsg);
		dispatch_assert(MACH_MSGH_BITS_LOCAL(hdr->msgh_bits) ==
				MACH_MSG_TYPE_MAKE_SEND_ONCE);
		hdr->msgh_local_port = reply_port;
		options |= DISPATCH_MACH_OWNED_REPLY_PORT;
	}

	dispatch_mach_reply_refs_t dmr;
#if DISPATCH_DEBUG
	dmr = _dispatch_calloc(1, sizeof(*dmr));
#else
	struct dispatch_mach_reply_refs_s dmr_buf = { };
	dmr = &dmr_buf;
#endif
	struct dispatch_continuation_s dc_wait = {
		.dc_flags = DISPATCH_OBJ_SYNC_SLOW_BIT,
		.dc_data = dmsg,
		.dc_other = dmr,
		.dc_priority = DISPATCH_NO_PRIORITY,
		.dc_voucher = DISPATCH_NO_VOUCHER,
	};
	dmr->dmr_ctxt = dmsg->do_ctxt;
	*returned_send_result = _dispatch_mach_send_msg(dm, dmsg, &dc_wait,options);
	if (options & DISPATCH_MACH_OWNED_REPLY_PORT) {
		_dispatch_clear_thread_reply_port(reply_port);
	}
	dmsg = _dispatch_mach_msg_reply_recv(dm, dmr, reply_port);
#if DISPATCH_DEBUG
	free(dmr);
#endif
	return dmsg;
}

DISPATCH_NOINLINE
dispatch_mach_msg_t
dispatch_mach_send_and_wait_for_reply(dispatch_mach_t dm,
		dispatch_mach_msg_t dmsg, mach_msg_option_t options)
{
	bool returned_send_result;
	dispatch_mach_msg_t reply;
	dispatch_assert_zero(options & DISPATCH_MACH_OPTIONS_MASK);
	options &= ~DISPATCH_MACH_OPTIONS_MASK;
	options |= DISPATCH_MACH_WAIT_FOR_REPLY;
	reply = _dispatch_mach_send_and_wait_for_reply(dm, dmsg, options,
			&returned_send_result);
	dispatch_assert(!returned_send_result);
	return reply;
}

DISPATCH_NOINLINE
dispatch_mach_msg_t
dispatch_mach_send_with_result_and_wait_for_reply(dispatch_mach_t dm,
		dispatch_mach_msg_t dmsg, mach_msg_option_t options,
		dispatch_mach_send_flags_t send_flags,
		dispatch_mach_reason_t *send_result, mach_error_t *send_error)
{
	if (unlikely(send_flags != DISPATCH_MACH_SEND_DEFAULT)) {
		DISPATCH_CLIENT_CRASH(send_flags, "Invalid send flags");
	}
	bool returned_send_result;
	dispatch_mach_msg_t reply;
	dispatch_assert_zero(options & DISPATCH_MACH_OPTIONS_MASK);
	options &= ~DISPATCH_MACH_OPTIONS_MASK;
	options |= DISPATCH_MACH_WAIT_FOR_REPLY;
	options |= DISPATCH_MACH_RETURN_IMMEDIATE_SEND_RESULT;
	reply = _dispatch_mach_send_and_wait_for_reply(dm, dmsg, options,
			&returned_send_result);
	unsigned long reason = DISPATCH_MACH_NEEDS_DEFERRED_SEND;
	mach_error_t err = 0;
	if (returned_send_result) {
		reason = _dispatch_mach_msg_get_reason(dmsg, &err);
	}
	*send_result = reason;
	*send_error = err;
	return reply;
}

DISPATCH_NOINLINE
static bool
_dispatch_mach_disconnect(dispatch_mach_t dm)
{
	dispatch_mach_send_refs_t dr = dm->dm_refs;
	bool disconnected;
	if (dm->dm_dkev) {
		_dispatch_mach_notification_kevent_unregister(dm);
	}
	if (MACH_PORT_VALID(dr->dm_send)) {
		_dispatch_mach_msg_disconnected(dm, MACH_PORT_NULL, dr->dm_send);
	}
	dr->dm_send = MACH_PORT_NULL;
	if (dr->dm_checkin) {
		_dispatch_mach_msg_not_sent(dm, dr->dm_checkin);
		dr->dm_checkin = NULL;
	}
	_dispatch_unfair_lock_lock(&dm->dm_refs->dm_replies_lock);
	dispatch_mach_reply_refs_t dmr, tmp;
	TAILQ_FOREACH_SAFE(dmr, &dm->dm_refs->dm_replies, dmr_list, tmp) {
		TAILQ_REMOVE(&dm->dm_refs->dm_replies, dmr, dmr_list);
		_TAILQ_MARK_NOT_ENQUEUED(dmr, dmr_list);
		if (dmr->dmr_dkev) {
			_dispatch_mach_reply_kevent_unregister(dm, dmr,
					DKEV_UNREGISTER_DISCONNECTED);
		} else {
			_dispatch_mach_reply_waiter_unregister(dm, dmr,
					DKEV_UNREGISTER_DISCONNECTED);
		}
	}
	disconnected = TAILQ_EMPTY(&dm->dm_refs->dm_replies);
	_dispatch_unfair_lock_unlock(&dm->dm_refs->dm_replies_lock);
	return disconnected;
}

static void
_dispatch_mach_cancel(dispatch_mach_t dm)
{
	_dispatch_object_debug(dm, "%s", __func__);
	if (!_dispatch_mach_disconnect(dm)) return;
	if (dm->ds_dkev) {
		mach_port_t local_port = (mach_port_t)dm->ds_dkev->dk_kevent.ident;
		_dispatch_source_kevent_unregister(dm->_as_ds);
		if ((dm->dq_atomic_flags & DSF_STATE_MASK) == DSF_DELETED) {
			_dispatch_mach_msg_disconnected(dm, local_port, MACH_PORT_NULL);
		}
	} else {
		_dispatch_queue_atomic_flags_set_and_clear(dm->_as_dq, DSF_DELETED,
				DSF_ARMED | DSF_DEFERRED_DELETE);
	}
}

DISPATCH_NOINLINE
static bool
_dispatch_mach_reconnect_invoke(dispatch_mach_t dm, dispatch_object_t dou)
{
	if (!_dispatch_mach_disconnect(dm)) return false;
	dispatch_mach_send_refs_t dr = dm->dm_refs;
	dr->dm_checkin = dou._dc->dc_data;
	dr->dm_send = (mach_port_t)dou._dc->dc_other;
	_dispatch_continuation_free(dou._dc);
	(void)os_atomic_dec2o(dr, dm_disconnect_cnt, relaxed);
	_dispatch_object_debug(dm, "%s", __func__);
	_dispatch_release(dm); // <rdar://problem/26266265>
	return true;
}

DISPATCH_NOINLINE
void
dispatch_mach_reconnect(dispatch_mach_t dm, mach_port_t send,
		dispatch_mach_msg_t checkin)
{
	dispatch_mach_send_refs_t dr = dm->dm_refs;
	(void)os_atomic_inc2o(dr, dm_disconnect_cnt, relaxed);
	if (MACH_PORT_VALID(send) && checkin) {
		dispatch_retain(checkin);
		checkin->dmsg_options = _dispatch_mach_checkin_options();
		dr->dm_checkin_port = _dispatch_mach_msg_get_remote_port(checkin);
	} else {
		checkin = NULL;
		dr->dm_checkin_port = MACH_PORT_NULL;
	}
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	dc->dc_flags = DISPATCH_OBJ_CONSUME_BIT;
	// actually called manually in _dispatch_mach_send_drain
	dc->dc_func = (void*)_dispatch_mach_reconnect_invoke;
	dc->dc_ctxt = dc;
	dc->dc_data = checkin;
	dc->dc_other = (void*)(uintptr_t)send;
	dc->dc_voucher = DISPATCH_NO_VOUCHER;
	dc->dc_priority = DISPATCH_NO_PRIORITY;
	_dispatch_retain(dm); // <rdar://problem/26266265>
	return _dispatch_mach_send_push(dm, dc, 0);
}

DISPATCH_NOINLINE
mach_port_t
dispatch_mach_get_checkin_port(dispatch_mach_t dm)
{
	dispatch_mach_send_refs_t dr = dm->dm_refs;
	if (slowpath(dm->dq_atomic_flags & DSF_CANCELED)) {
		return MACH_PORT_DEAD;
	}
	return dr->dm_checkin_port;
}

DISPATCH_NOINLINE
static void
_dispatch_mach_connect_invoke(dispatch_mach_t dm)
{
	dispatch_mach_refs_t dr = dm->ds_refs;
	_dispatch_client_callout4(dr->dm_handler_ctxt,
			DISPATCH_MACH_CONNECTED, NULL, 0, dr->dm_handler_func);
	dm->dm_connect_handler_called = 1;
}

DISPATCH_NOINLINE
void
_dispatch_mach_msg_invoke(dispatch_mach_msg_t dmsg,
		dispatch_invoke_flags_t flags)
{
	dispatch_thread_frame_s dtf;
	dispatch_mach_refs_t dr;
	dispatch_mach_t dm;
	mach_error_t err;
	unsigned long reason = _dispatch_mach_msg_get_reason(dmsg, &err);
	_dispatch_thread_set_self_t adopt_flags = DISPATCH_PRIORITY_ENFORCE|
			DISPATCH_VOUCHER_CONSUME|DISPATCH_VOUCHER_REPLACE;

	// hide mach channel
	dm = (dispatch_mach_t)_dispatch_thread_frame_stash(&dtf);
	dr = dm->ds_refs;
	dmsg->do_next = DISPATCH_OBJECT_LISTLESS;
	_dispatch_voucher_ktrace_dmsg_pop(dmsg);
	_dispatch_voucher_debug("mach-msg[%p] adopt", dmsg->dmsg_voucher, dmsg);
	(void)_dispatch_adopt_priority_and_set_voucher(dmsg->dmsg_priority,
			dmsg->dmsg_voucher, adopt_flags);
	dmsg->dmsg_voucher = NULL;
	dispatch_invoke_with_autoreleasepool(flags, {
		if (slowpath(!dm->dm_connect_handler_called)) {
			_dispatch_mach_connect_invoke(dm);
		}
		_dispatch_client_callout4(dr->dm_handler_ctxt, reason, dmsg, err,
				dr->dm_handler_func);
	});
	_dispatch_thread_frame_unstash(&dtf);
	_dispatch_introspection_queue_item_complete(dmsg);
	dispatch_release(dmsg);
}

DISPATCH_NOINLINE
void
_dispatch_mach_barrier_invoke(dispatch_continuation_t dc,
		dispatch_invoke_flags_t flags)
{
	dispatch_thread_frame_s dtf;
	dispatch_mach_t dm = dc->dc_other;
	dispatch_mach_refs_t dr;
	uintptr_t dc_flags = (uintptr_t)dc->dc_data;
	unsigned long type = dc_type(dc);

	// hide mach channel from clients
	if (type == DISPATCH_CONTINUATION_TYPE(MACH_RECV_BARRIER)) {
		// on the send queue, the mach channel isn't the current queue
		// its target queue is the current one already
		_dispatch_thread_frame_stash(&dtf);
	}
	dr = dm->ds_refs;
	DISPATCH_COMPILER_CAN_ASSUME(dc_flags & DISPATCH_OBJ_CONSUME_BIT);
	_dispatch_continuation_pop_forwarded(dc, dm->dq_override_voucher, dc_flags,{
		dispatch_invoke_with_autoreleasepool(flags, {
			if (slowpath(!dm->dm_connect_handler_called)) {
				_dispatch_mach_connect_invoke(dm);
			}
			_dispatch_client_callout(dc->dc_ctxt, dc->dc_func);
			_dispatch_client_callout4(dr->dm_handler_ctxt,
					DISPATCH_MACH_BARRIER_COMPLETED, NULL, 0,
					dr->dm_handler_func);
		});
	});
	if (type == DISPATCH_CONTINUATION_TYPE(MACH_RECV_BARRIER)) {
		_dispatch_thread_frame_unstash(&dtf);
	}
}

DISPATCH_NOINLINE
void
dispatch_mach_send_barrier_f(dispatch_mach_t dm, void *context,
		dispatch_function_t func)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT;
	pthread_priority_t pp;

	_dispatch_continuation_init_f(dc, dm, context, func, 0, 0, dc_flags);
	dc->dc_data = (void *)dc->dc_flags;
	dc->dc_other = dm;
	dc->do_vtable = DC_VTABLE(MACH_SEND_BARRIER);
	_dispatch_trace_continuation_push(dm->_as_dq, dc);
	pp = _dispatch_continuation_get_override_priority(dm->_as_dq, dc);
	return _dispatch_mach_send_push(dm, dc, pp);
}

DISPATCH_NOINLINE
void
dispatch_mach_send_barrier(dispatch_mach_t dm, dispatch_block_t barrier)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT;
	pthread_priority_t pp;

	_dispatch_continuation_init(dc, dm, barrier, 0, 0, dc_flags);
	dc->dc_data = (void *)dc->dc_flags;
	dc->dc_other = dm;
	dc->do_vtable = DC_VTABLE(MACH_SEND_BARRIER);
	_dispatch_trace_continuation_push(dm->_as_dq, dc);
	pp = _dispatch_continuation_get_override_priority(dm->_as_dq, dc);
	return _dispatch_mach_send_push(dm, dc, pp);
}

DISPATCH_NOINLINE
void
dispatch_mach_receive_barrier_f(dispatch_mach_t dm, void *context,
		dispatch_function_t func)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT;

	_dispatch_continuation_init_f(dc, dm, context, func, 0, 0, dc_flags);
	dc->dc_data = (void *)dc->dc_flags;
	dc->dc_other = dm;
	dc->do_vtable = DC_VTABLE(MACH_RECV_BARRIER);
	return _dispatch_continuation_async(dm->_as_dq, dc);
}

DISPATCH_NOINLINE
void
dispatch_mach_receive_barrier(dispatch_mach_t dm, dispatch_block_t barrier)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT;

	_dispatch_continuation_init(dc, dm, barrier, 0, 0, dc_flags);
	dc->dc_data = (void *)dc->dc_flags;
	dc->dc_other = dm;
	dc->do_vtable = DC_VTABLE(MACH_RECV_BARRIER);
	return _dispatch_continuation_async(dm->_as_dq, dc);
}

DISPATCH_NOINLINE
static void
_dispatch_mach_cancel_invoke(dispatch_mach_t dm, dispatch_invoke_flags_t flags)
{
	dispatch_mach_refs_t dr = dm->ds_refs;

	dispatch_invoke_with_autoreleasepool(flags, {
		if (slowpath(!dm->dm_connect_handler_called)) {
			_dispatch_mach_connect_invoke(dm);
		}
		_dispatch_client_callout4(dr->dm_handler_ctxt,
				DISPATCH_MACH_CANCELED, NULL, 0, dr->dm_handler_func);
	});
	dm->dm_cancel_handler_called = 1;
	_dispatch_release(dm); // the retain is done at creation time
}

DISPATCH_NOINLINE
void
dispatch_mach_cancel(dispatch_mach_t dm)
{
	dispatch_source_cancel(dm->_as_ds);
}

static void
_dispatch_mach_install(dispatch_mach_t dm, pthread_priority_t pp)
{
	uint32_t disconnect_cnt;

	if (dm->ds_dkev) {
		_dispatch_source_kevent_register(dm->_as_ds, pp);
	}
	if (dm->ds_is_direct_kevent) {
		pp &= (~_PTHREAD_PRIORITY_FLAGS_MASK |
				_PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG |
				_PTHREAD_PRIORITY_OVERCOMMIT_FLAG);
		// _dispatch_mach_reply_kevent_register assumes this has been done
		// which is unlike regular sources or queues, the DEFAULTQUEUE flag
		// is used so that the priority of that channel doesn't act as a floor
		// QoS for incoming messages (26761457)
		dm->dq_priority = (dispatch_priority_t)pp;
	}
	dm->ds_is_installed = true;
	if (unlikely(!os_atomic_cmpxchgv2o(dm->dm_refs, dm_disconnect_cnt,
			DISPATCH_MACH_NEVER_INSTALLED, 0, &disconnect_cnt, release))) {
		DISPATCH_INTERNAL_CRASH(disconnect_cnt, "Channel already installed");
	}
}

void
_dispatch_mach_finalize_activation(dispatch_mach_t dm)
{
	if (dm->ds_is_direct_kevent && !dm->ds_is_installed) {
		dispatch_source_t ds = dm->_as_ds;
		pthread_priority_t pp = _dispatch_source_compute_kevent_priority(ds);
		if (pp) _dispatch_mach_install(dm, pp);
	}

	// call "super"
	_dispatch_queue_finalize_activation(dm->_as_dq);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_t
_dispatch_mach_invoke2(dispatch_object_t dou, dispatch_invoke_flags_t flags,
		uint64_t *owned, struct dispatch_object_s **dc_ptr DISPATCH_UNUSED)
{
	dispatch_mach_t dm = dou._dm;
	dispatch_queue_t retq = NULL;
	dispatch_queue_t dq = _dispatch_queue_get_current();

	// This function performs all mach channel actions. Each action is
	// responsible for verifying that it takes place on the appropriate queue.
	// If the current queue is not the correct queue for this action, the
	// correct queue will be returned and the invoke will be re-driven on that
	// queue.

	// The order of tests here in invoke and in wakeup should be consistent.

	dispatch_mach_send_refs_t dr = dm->dm_refs;
	dispatch_queue_t dkq = &_dispatch_mgr_q;

	if (dm->ds_is_direct_kevent) {
		dkq = dm->do_targetq;
	}

	if (slowpath(!dm->ds_is_installed)) {
		// The channel needs to be installed on the kevent queue.
		if (dq != dkq) {
			return dkq;
		}
		_dispatch_mach_install(dm, _dispatch_get_defaultpriority());
	}

	if (_dispatch_queue_class_probe(dm)) {
		if (dq == dm->do_targetq) {
			retq = _dispatch_queue_serial_drain(dm->_as_dq, flags, owned, NULL);
		} else {
			retq = dm->do_targetq;
		}
	}

	dispatch_queue_flags_t dqf = _dispatch_queue_atomic_flags(dm->_as_dq);

	if (dr->dm_tail) {
		bool requires_mgr = dr->dm_needs_mgr || (dr->dm_disconnect_cnt &&
				(dm->dm_dkev || !dm->ds_is_direct_kevent));
		if (!(dm->dm_dkev && DISPATCH_MACH_NOTIFICATION_ARMED(dm->dm_dkev)) ||
				(dqf & DSF_CANCELED) || dr->dm_disconnect_cnt) {
			// The channel has pending messages to send.
			if (unlikely(requires_mgr && dq != &_dispatch_mgr_q)) {
				return retq ? retq : &_dispatch_mgr_q;
			}
			dispatch_mach_send_invoke_flags_t send_flags = DM_SEND_INVOKE_NONE;
			if (dq != &_dispatch_mgr_q) {
				send_flags |= DM_SEND_INVOKE_CAN_RUN_BARRIER;
			}
			_dispatch_mach_send_invoke(dm, flags, send_flags);
		}
	} else if (dqf & DSF_CANCELED) {
		// The channel has been cancelled and needs to be uninstalled from the
		// manager queue. After uninstallation, the cancellation handler needs
		// to be delivered to the target queue.
		if ((dqf & DSF_STATE_MASK) == (DSF_ARMED | DSF_DEFERRED_DELETE)) {
			// waiting for the delivery of a deferred delete event
			return retq;
		}
		if ((dqf & DSF_STATE_MASK) != DSF_DELETED) {
			if (dq != &_dispatch_mgr_q) {
				return retq ? retq : &_dispatch_mgr_q;
			}
			_dispatch_mach_send_invoke(dm, flags, DM_SEND_INVOKE_CANCEL);
			dqf = _dispatch_queue_atomic_flags(dm->_as_dq);
			if (unlikely((dqf & DSF_STATE_MASK) != DSF_DELETED)) {
				// waiting for the delivery of a deferred delete event
				// or deletion didn't happen because send_invoke couldn't
				// acquire the send lock
				return retq;
			}
		}
		if (!dm->dm_cancel_handler_called) {
			if (dq != dm->do_targetq) {
				return retq ? retq : dm->do_targetq;
			}
			_dispatch_mach_cancel_invoke(dm, flags);
		}
	}

	return retq;
}

DISPATCH_NOINLINE
void
_dispatch_mach_invoke(dispatch_mach_t dm, dispatch_invoke_flags_t flags)
{
	_dispatch_queue_class_invoke(dm, flags, _dispatch_mach_invoke2);
}

void
_dispatch_mach_wakeup(dispatch_mach_t dm, pthread_priority_t pp,
		dispatch_wakeup_flags_t flags)
{
	// This function determines whether the mach channel needs to be invoked.
	// The order of tests here in probe and in invoke should be consistent.

	dispatch_mach_send_refs_t dr = dm->dm_refs;
	dispatch_queue_wakeup_target_t dkq = DISPATCH_QUEUE_WAKEUP_MGR;
	dispatch_queue_wakeup_target_t tq = DISPATCH_QUEUE_WAKEUP_NONE;
	dispatch_queue_flags_t dqf = _dispatch_queue_atomic_flags(dm->_as_dq);

	if (dm->ds_is_direct_kevent) {
		dkq = DISPATCH_QUEUE_WAKEUP_TARGET;
	}

	if (!dm->ds_is_installed) {
		// The channel needs to be installed on the kevent queue.
		tq = dkq;
		goto done;
	}

	if (_dispatch_queue_class_probe(dm)) {
		tq = DISPATCH_QUEUE_WAKEUP_TARGET;
		goto done;
	}

	if (_dispatch_lock_is_locked(dr->dm_state_lock.dul_lock)) {
		// Sending and uninstallation below require the send lock, the channel
		// will be woken up when the lock is dropped <rdar://15132939&15203957>
		_dispatch_queue_reinstate_override_priority(dm, (dispatch_priority_t)pp);
		goto done;
	}

	if (dr->dm_tail) {
		bool requires_mgr = dr->dm_needs_mgr || (dr->dm_disconnect_cnt &&
				(dm->dm_dkev || !dm->ds_is_direct_kevent));
		if (!(dm->dm_dkev && DISPATCH_MACH_NOTIFICATION_ARMED(dm->dm_dkev)) ||
				(dqf & DSF_CANCELED) || dr->dm_disconnect_cnt) {
			if (unlikely(requires_mgr)) {
				tq = DISPATCH_QUEUE_WAKEUP_MGR;
			} else {
				tq = DISPATCH_QUEUE_WAKEUP_TARGET;
			}
		} else {
			// can happen when we can't send because the port is full
			// but we should not lose the override
			_dispatch_queue_reinstate_override_priority(dm,
					(dispatch_priority_t)pp);
		}
	} else if (dqf & DSF_CANCELED) {
		if ((dqf & DSF_STATE_MASK) == (DSF_ARMED | DSF_DEFERRED_DELETE)) {
			// waiting for the delivery of a deferred delete event
		} else if ((dqf & DSF_STATE_MASK) != DSF_DELETED) {
			// The channel needs to be uninstalled from the manager queue
			tq = DISPATCH_QUEUE_WAKEUP_MGR;
		} else if (!dm->dm_cancel_handler_called) {
			// the cancellation handler needs to be delivered to the target
			// queue.
			tq = DISPATCH_QUEUE_WAKEUP_TARGET;
		}
	}

done:
	if (tq) {
		return _dispatch_queue_class_wakeup(dm->_as_dq, pp, flags, tq);
	} else if (pp) {
		return _dispatch_queue_class_override_drainer(dm->_as_dq, pp, flags);
	} else if (flags & DISPATCH_WAKEUP_CONSUME) {
		return _dispatch_release_tailcall(dm);
	}
}

#pragma mark -
#pragma mark dispatch_mach_msg_t

dispatch_mach_msg_t
dispatch_mach_msg_create(mach_msg_header_t *msg, size_t size,
		dispatch_mach_msg_destructor_t destructor, mach_msg_header_t **msg_ptr)
{
	if (slowpath(size < sizeof(mach_msg_header_t)) ||
			slowpath(destructor && !msg)) {
		DISPATCH_CLIENT_CRASH(size, "Empty message");
	}
	dispatch_mach_msg_t dmsg = _dispatch_alloc(DISPATCH_VTABLE(mach_msg),
			sizeof(struct dispatch_mach_msg_s) +
			(destructor ? 0 : size - sizeof(dmsg->dmsg_msg)));
	if (destructor) {
		dmsg->dmsg_msg = msg;
	} else if (msg) {
		memcpy(dmsg->dmsg_buf, msg, size);
	}
	dmsg->do_next = DISPATCH_OBJECT_LISTLESS;
	dmsg->do_targetq = _dispatch_get_root_queue(_DISPATCH_QOS_CLASS_DEFAULT,
			false);
	dmsg->dmsg_destructor = destructor;
	dmsg->dmsg_size = size;
	if (msg_ptr) {
		*msg_ptr = _dispatch_mach_msg_get_msg(dmsg);
	}
	return dmsg;
}

void
_dispatch_mach_msg_dispose(dispatch_mach_msg_t dmsg)
{
	if (dmsg->dmsg_voucher) {
		_voucher_release(dmsg->dmsg_voucher);
		dmsg->dmsg_voucher = NULL;
	}
	switch (dmsg->dmsg_destructor) {
	case DISPATCH_MACH_MSG_DESTRUCTOR_DEFAULT:
		break;
	case DISPATCH_MACH_MSG_DESTRUCTOR_FREE:
		free(dmsg->dmsg_msg);
		break;
	case DISPATCH_MACH_MSG_DESTRUCTOR_VM_DEALLOCATE: {
		mach_vm_size_t vm_size = dmsg->dmsg_size;
		mach_vm_address_t vm_addr = (uintptr_t)dmsg->dmsg_msg;
		(void)dispatch_assume_zero(mach_vm_deallocate(mach_task_self(),
				vm_addr, vm_size));
		break;
	}}
}

static inline mach_msg_header_t*
_dispatch_mach_msg_get_msg(dispatch_mach_msg_t dmsg)
{
	return dmsg->dmsg_destructor ? dmsg->dmsg_msg :
			(mach_msg_header_t*)dmsg->dmsg_buf;
}

mach_msg_header_t*
dispatch_mach_msg_get_msg(dispatch_mach_msg_t dmsg, size_t *size_ptr)
{
	if (size_ptr) {
		*size_ptr = dmsg->dmsg_size;
	}
	return _dispatch_mach_msg_get_msg(dmsg);
}

size_t
_dispatch_mach_msg_debug(dispatch_mach_msg_t dmsg, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			dx_kind(dmsg), dmsg);
	offset += dsnprintf(&buf[offset], bufsiz - offset, "xrefcnt = 0x%x, "
			"refcnt = 0x%x, ", dmsg->do_xref_cnt + 1, dmsg->do_ref_cnt + 1);
	offset += dsnprintf(&buf[offset], bufsiz - offset, "opts/err = 0x%x, "
			"msgh[%p] = { ", dmsg->dmsg_options, dmsg->dmsg_buf);
	mach_msg_header_t *hdr = _dispatch_mach_msg_get_msg(dmsg);
	if (hdr->msgh_id) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "id 0x%x, ",
				hdr->msgh_id);
	}
	if (hdr->msgh_size) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "size %u, ",
				hdr->msgh_size);
	}
	if (hdr->msgh_bits) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "bits <l %u, r %u",
				MACH_MSGH_BITS_LOCAL(hdr->msgh_bits),
				MACH_MSGH_BITS_REMOTE(hdr->msgh_bits));
		if (MACH_MSGH_BITS_OTHER(hdr->msgh_bits)) {
			offset += dsnprintf(&buf[offset], bufsiz - offset, ", o 0x%x",
					MACH_MSGH_BITS_OTHER(hdr->msgh_bits));
		}
		offset += dsnprintf(&buf[offset], bufsiz - offset, ">, ");
	}
	if (hdr->msgh_local_port && hdr->msgh_remote_port) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "local 0x%x, "
				"remote 0x%x", hdr->msgh_local_port, hdr->msgh_remote_port);
	} else if (hdr->msgh_local_port) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "local 0x%x",
				hdr->msgh_local_port);
	} else if (hdr->msgh_remote_port) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "remote 0x%x",
				hdr->msgh_remote_port);
	} else {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "no ports");
	}
	offset += dsnprintf(&buf[offset], bufsiz - offset, " } }");
	return offset;
}

#pragma mark -
#pragma mark dispatch_mig_server

mach_msg_return_t
dispatch_mig_server(dispatch_source_t ds, size_t maxmsgsz,
		dispatch_mig_callback_t callback)
{
	mach_msg_options_t options = MACH_RCV_MSG | MACH_RCV_TIMEOUT
		| MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_CTX)
		| MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) | MACH_RCV_VOUCHER;
	mach_msg_options_t tmp_options;
	mig_reply_error_t *bufTemp, *bufRequest, *bufReply;
	mach_msg_return_t kr = 0;
	uint64_t assertion_token = 0;
	unsigned int cnt = 1000; // do not stall out serial queues
	boolean_t demux_success;
	bool received = false;
	size_t rcv_size = maxmsgsz + MAX_TRAILER_SIZE;

	bufRequest = alloca(rcv_size);
	bufRequest->RetCode = 0;
	for (mach_vm_address_t p = mach_vm_trunc_page(bufRequest + vm_page_size);
			p < (mach_vm_address_t)bufRequest + rcv_size; p += vm_page_size) {
		*(char*)p = 0; // ensure alloca buffer doesn't overlap with stack guard
	}

	bufReply = alloca(rcv_size);
	bufReply->Head.msgh_size = 0;
	for (mach_vm_address_t p = mach_vm_trunc_page(bufReply + vm_page_size);
			p < (mach_vm_address_t)bufReply + rcv_size; p += vm_page_size) {
		*(char*)p = 0; // ensure alloca buffer doesn't overlap with stack guard
	}

#if DISPATCH_DEBUG
	options |= MACH_RCV_LARGE; // rdar://problem/8422992
#endif
	tmp_options = options;
	// XXX FIXME -- change this to not starve out the target queue
	for (;;) {
		if (DISPATCH_QUEUE_IS_SUSPENDED(ds) || (--cnt == 0)) {
			options &= ~MACH_RCV_MSG;
			tmp_options &= ~MACH_RCV_MSG;

			if (!(tmp_options & MACH_SEND_MSG)) {
				goto out;
			}
		}
		kr = mach_msg(&bufReply->Head, tmp_options, bufReply->Head.msgh_size,
				(mach_msg_size_t)rcv_size, (mach_port_t)ds->ds_ident_hack, 0,0);

		tmp_options = options;

		if (slowpath(kr)) {
			switch (kr) {
			case MACH_SEND_INVALID_DEST:
			case MACH_SEND_TIMED_OUT:
				if (bufReply->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) {
					mach_msg_destroy(&bufReply->Head);
				}
				break;
			case MACH_RCV_TIMED_OUT:
				// Don't return an error if a message was sent this time or
				// a message was successfully received previously
				// rdar://problems/7363620&7791738
				if(bufReply->Head.msgh_remote_port || received) {
					kr = MACH_MSG_SUCCESS;
				}
				break;
			case MACH_RCV_INVALID_NAME:
				break;
#if DISPATCH_DEBUG
			case MACH_RCV_TOO_LARGE:
				// receive messages that are too large and log their id and size
				// rdar://problem/8422992
				tmp_options &= ~MACH_RCV_LARGE;
				size_t large_size = bufReply->Head.msgh_size + MAX_TRAILER_SIZE;
				void *large_buf = malloc(large_size);
				if (large_buf) {
					rcv_size = large_size;
					bufReply = large_buf;
				}
				if (!mach_msg(&bufReply->Head, tmp_options, 0,
						(mach_msg_size_t)rcv_size,
						(mach_port_t)ds->ds_ident_hack, 0, 0)) {
					_dispatch_log("BUG in libdispatch client: "
							"dispatch_mig_server received message larger than "
							"requested size %zd: id = 0x%x, size = %d",
							maxmsgsz, bufReply->Head.msgh_id,
							bufReply->Head.msgh_size);
				}
				if (large_buf) {
					free(large_buf);
				}
				// fall through
#endif
			default:
				_dispatch_bug_mach_client(
						"dispatch_mig_server: mach_msg() failed", kr);
				break;
			}
			goto out;
		}

		if (!(tmp_options & MACH_RCV_MSG)) {
			goto out;
		}

		if (assertion_token) {
#if DISPATCH_USE_IMPORTANCE_ASSERTION
			int r = proc_importance_assertion_complete(assertion_token);
			(void)dispatch_assume_zero(r);
#endif
			assertion_token = 0;
		}
		received = true;

		bufTemp = bufRequest;
		bufRequest = bufReply;
		bufReply = bufTemp;

#if DISPATCH_USE_IMPORTANCE_ASSERTION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		int r = proc_importance_assertion_begin_with_msg(&bufRequest->Head,
				NULL, &assertion_token);
		if (r && slowpath(r != EIO)) {
			(void)dispatch_assume_zero(r);
		}
#pragma clang diagnostic pop
#endif
		_voucher_replace(voucher_create_with_mach_msg(&bufRequest->Head));
		demux_success = callback(&bufRequest->Head, &bufReply->Head);

		if (!demux_success) {
			// destroy the request - but not the reply port
			bufRequest->Head.msgh_remote_port = 0;
			mach_msg_destroy(&bufRequest->Head);
		} else if (!(bufReply->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
			// if MACH_MSGH_BITS_COMPLEX is _not_ set, then bufReply->RetCode
			// is present
			if (slowpath(bufReply->RetCode)) {
				if (bufReply->RetCode == MIG_NO_REPLY) {
					continue;
				}

				// destroy the request - but not the reply port
				bufRequest->Head.msgh_remote_port = 0;
				mach_msg_destroy(&bufRequest->Head);
			}
		}

		if (bufReply->Head.msgh_remote_port) {
			tmp_options |= MACH_SEND_MSG;
			if (MACH_MSGH_BITS_REMOTE(bufReply->Head.msgh_bits) !=
					MACH_MSG_TYPE_MOVE_SEND_ONCE) {
				tmp_options |= MACH_SEND_TIMEOUT;
			}
		}
	}

out:
	if (assertion_token) {
#if DISPATCH_USE_IMPORTANCE_ASSERTION
		int r = proc_importance_assertion_complete(assertion_token);
		(void)dispatch_assume_zero(r);
#endif
	}

	return kr;
}

#endif /* HAVE_MACH */

#pragma mark -
#pragma mark dispatch_source_debug

DISPATCH_NOINLINE
static const char *
_evfiltstr(short filt)
{
	switch (filt) {
#define _evfilt2(f) case (f): return #f
	_evfilt2(EVFILT_READ);
	_evfilt2(EVFILT_WRITE);
	_evfilt2(EVFILT_AIO);
	_evfilt2(EVFILT_VNODE);
	_evfilt2(EVFILT_PROC);
	_evfilt2(EVFILT_SIGNAL);
	_evfilt2(EVFILT_TIMER);
#if HAVE_MACH
	_evfilt2(EVFILT_MACHPORT);
	_evfilt2(DISPATCH_EVFILT_MACH_NOTIFICATION);
#endif
	_evfilt2(EVFILT_FS);
	_evfilt2(EVFILT_USER);
#ifdef EVFILT_VM
	_evfilt2(EVFILT_VM);
#endif
#ifdef EVFILT_SOCK
	_evfilt2(EVFILT_SOCK);
#endif
#ifdef EVFILT_MEMORYSTATUS
	_evfilt2(EVFILT_MEMORYSTATUS);
#endif

	_evfilt2(DISPATCH_EVFILT_TIMER);
	_evfilt2(DISPATCH_EVFILT_CUSTOM_ADD);
	_evfilt2(DISPATCH_EVFILT_CUSTOM_OR);
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
#endif

static size_t
_dispatch_source_debug_attr(dispatch_source_t ds, char* buf, size_t bufsiz)
{
	dispatch_queue_t target = ds->do_targetq;
	return dsnprintf(buf, bufsiz, "target = %s[%p], ident = 0x%lx, "
			"mask = 0x%lx, pending_data = 0x%lx, registered = %d, "
			"armed = %d, deleted = %d%s, canceled = %d, ",
			target && target->dq_label ? target->dq_label : "", target,
			ds->ds_ident_hack, ds->ds_pending_data_mask, ds->ds_pending_data,
			ds->ds_is_installed, (bool)(ds->dq_atomic_flags & DSF_ARMED),
			(bool)(ds->dq_atomic_flags & DSF_DELETED),
			(ds->dq_atomic_flags & DSF_DEFERRED_DELETE) ? " (pending)" : "",
			(bool)(ds->dq_atomic_flags & DSF_CANCELED));
}

static size_t
_dispatch_timer_debug_attr(dispatch_source_t ds, char* buf, size_t bufsiz)
{
	dispatch_source_refs_t dr = ds->ds_refs;
	return dsnprintf(buf, bufsiz, "timer = { target = 0x%llx, deadline = 0x%llx"
			", last_fire = 0x%llx, interval = 0x%llx, flags = 0x%lx }, ",
			(unsigned long long)ds_timer(dr).target,
			(unsigned long long)ds_timer(dr).deadline,
			(unsigned long long)ds_timer(dr).last_fire,
			(unsigned long long)ds_timer(dr).interval, ds_timer(dr).flags);
}

size_t
_dispatch_source_debug(dispatch_source_t ds, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			dx_kind(ds), ds);
	offset += _dispatch_object_debug_attr(ds, &buf[offset], bufsiz - offset);
	offset += _dispatch_source_debug_attr(ds, &buf[offset], bufsiz - offset);
	if (ds->ds_is_timer) {
		offset += _dispatch_timer_debug_attr(ds, &buf[offset], bufsiz - offset);
	}
	const char *filter;
	if (!ds->ds_dkev) {
		filter = "????";
	} else if (ds->ds_is_custom_source) {
		filter = _evfiltstr((int16_t)(uintptr_t)ds->ds_dkev);
	} else {
		filter = _evfiltstr(ds->ds_dkev->dk_kevent.filter);
	}
	offset += dsnprintf(&buf[offset], bufsiz - offset, "kevent = %p%s, "
			"filter = %s }", ds->ds_dkev,  ds->ds_is_direct_kevent ? " (direct)"
			: "", filter);
	return offset;
}

#if HAVE_MACH
static size_t
_dispatch_mach_debug_attr(dispatch_mach_t dm, char* buf, size_t bufsiz)
{
	dispatch_queue_t target = dm->do_targetq;
	return dsnprintf(buf, bufsiz, "target = %s[%p], receive = 0x%x, "
			"send = 0x%x, send-possible = 0x%x%s, checkin = 0x%x%s, "
			"send state = %016llx, disconnected = %d, canceled = %d ",
			target && target->dq_label ? target->dq_label : "", target,
			dm->ds_dkev ?(mach_port_t)dm->ds_dkev->dk_kevent.ident:0,
			dm->dm_refs->dm_send,
			dm->dm_dkev ?(mach_port_t)dm->dm_dkev->dk_kevent.ident:0,
			dm->dm_dkev && DISPATCH_MACH_NOTIFICATION_ARMED(dm->dm_dkev) ?
			" (armed)" : "", dm->dm_refs->dm_checkin_port,
			dm->dm_refs->dm_checkin ? " (pending)" : "",
			dm->dm_refs->dm_state, dm->dm_refs->dm_disconnect_cnt,
			(bool)(dm->dq_atomic_flags & DSF_CANCELED));
}

size_t
_dispatch_mach_debug(dispatch_mach_t dm, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			dm->dq_label && !dm->dm_cancel_handler_called ? dm->dq_label :
			dx_kind(dm), dm);
	offset += _dispatch_object_debug_attr(dm, &buf[offset], bufsiz - offset);
	offset += _dispatch_mach_debug_attr(dm, &buf[offset], bufsiz - offset);
	offset += dsnprintf(&buf[offset], bufsiz - offset, "}");
	return offset;
}
#endif // HAVE_MACH

#if DISPATCH_DEBUG
DISPATCH_NOINLINE
static void
dispatch_kevent_debug(const char *verb, const _dispatch_kevent_qos_s *kev,
		int i, int n, const char *function, unsigned int line)
{
	char flagstr[256];
	char i_n[31];

	if (n > 1) {
		snprintf(i_n, sizeof(i_n), "%d/%d ", i + 1, n);
	} else {
		i_n[0] = '\0';
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
			"flags = %s (0x%x), fflags = 0x%x, data = 0x%llx, udata = 0x%llx, "
			"ext[0] = 0x%llx, ext[1] = 0x%llx }: %s #%u", verb, kev, i_n,
			kev->ident, _evfiltstr(kev->filter), _evflagstr(kev->flags, flagstr,
			sizeof(flagstr)), kev->flags, kev->fflags, kev->data, kev->udata,
#ifndef IGNORE_KEVENT64_EXT
			kev->ext[0], kev->ext[1],
#else
			0ull, 0ull,
#endif
			function, line);
#endif
}

static void
_dispatch_kevent_debugger2(void *context)
{
	struct sockaddr sa;
	socklen_t sa_len = sizeof(sa);
	int c, fd = (int)(long)context;
	unsigned int i;
	dispatch_kevent_t dk;
	dispatch_source_t ds;
	dispatch_source_refs_t dr;
	FILE *debug_stream;

	c = accept(fd, &sa, &sa_len);
	if (c == -1) {
		if (errno != EAGAIN) {
			(void)dispatch_assume_zero(errno);
		}
		return;
	}
#if 0
	int r = fcntl(c, F_SETFL, 0); // disable non-blocking IO
	if (r == -1) {
		(void)dispatch_assume_zero(errno);
	}
#endif
	debug_stream = fdopen(c, "a");
	if (!dispatch_assume(debug_stream)) {
		close(c);
		return;
	}

	fprintf(debug_stream, "HTTP/1.0 200 OK\r\n");
	fprintf(debug_stream, "Content-type: text/html\r\n");
	fprintf(debug_stream, "Pragma: nocache\r\n");
	fprintf(debug_stream, "\r\n");
	fprintf(debug_stream, "<html>\n");
	fprintf(debug_stream, "<head><title>PID %u</title></head>\n", getpid());
	fprintf(debug_stream, "<body>\n<ul>\n");

	for (i = 0; i < DSL_HASH_SIZE; i++) {
		if (TAILQ_EMPTY(&_dispatch_sources[i])) {
			continue;
		}
		TAILQ_FOREACH(dk, &_dispatch_sources[i], dk_list) {
			fprintf(debug_stream, "\t<br><li>DK %p ident %lu filter %s flags "
					"0x%hx fflags 0x%x data 0x%lx udata %p\n",
					dk, (unsigned long)dk->dk_kevent.ident,
					_evfiltstr(dk->dk_kevent.filter), dk->dk_kevent.flags,
					dk->dk_kevent.fflags, (unsigned long)dk->dk_kevent.data,
					(void*)dk->dk_kevent.udata);
			fprintf(debug_stream, "\t\t<ul>\n");
			TAILQ_FOREACH(dr, &dk->dk_sources, dr_list) {
				ds = _dispatch_source_from_refs(dr);
				fprintf(debug_stream, "\t\t\t<li>DS %p refcnt 0x%x state "
						"0x%llx data 0x%lx mask 0x%lx flags 0x%x</li>\n",
						ds, ds->do_ref_cnt + 1, ds->dq_state,
						ds->ds_pending_data, ds->ds_pending_data_mask,
						ds->dq_atomic_flags);
				if (_dq_state_is_enqueued(ds->dq_state)) {
					dispatch_queue_t dq = ds->do_targetq;
					fprintf(debug_stream, "\t\t<br>DQ: %p refcnt 0x%x state "
							"0x%llx label: %s\n", dq, dq->do_ref_cnt + 1,
							dq->dq_state, dq->dq_label ?: "");
				}
			}
			fprintf(debug_stream, "\t\t</ul>\n");
			fprintf(debug_stream, "\t</li>\n");
		}
	}
	fprintf(debug_stream, "</ul>\n</body>\n</html>\n");
	fflush(debug_stream);
	fclose(debug_stream);
}

static void
_dispatch_kevent_debugger2_cancel(void *context)
{
	int ret, fd = (int)(long)context;

	ret = close(fd);
	if (ret != -1) {
		(void)dispatch_assume_zero(errno);
	}
}

static void
_dispatch_kevent_debugger(void *context DISPATCH_UNUSED)
{
	union {
		struct sockaddr_in sa_in;
		struct sockaddr sa;
	} sa_u = {
		.sa_in = {
			.sin_family = AF_INET,
			.sin_addr = { htonl(INADDR_LOOPBACK), },
		},
	};
	dispatch_source_t ds;
	const char *valstr;
	int val, r, fd, sock_opt = 1;
	socklen_t slen = sizeof(sa_u);

#ifndef __linux__
	if (issetugid()) {
		return;
	}
#endif
	valstr = getenv("LIBDISPATCH_DEBUGGER");
	if (!valstr) {
		return;
	}
	val = atoi(valstr);
	if (val == 2) {
		sa_u.sa_in.sin_addr.s_addr = 0;
	}
	fd = socket(PF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		(void)dispatch_assume_zero(errno);
		return;
	}
	r = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&sock_opt,
			(socklen_t) sizeof sock_opt);
	if (r == -1) {
		(void)dispatch_assume_zero(errno);
		goto out_bad;
	}
#if 0
	r = fcntl(fd, F_SETFL, O_NONBLOCK);
	if (r == -1) {
		(void)dispatch_assume_zero(errno);
		goto out_bad;
	}
#endif
	r = bind(fd, &sa_u.sa, sizeof(sa_u));
	if (r == -1) {
		(void)dispatch_assume_zero(errno);
		goto out_bad;
	}
	r = listen(fd, SOMAXCONN);
	if (r == -1) {
		(void)dispatch_assume_zero(errno);
		goto out_bad;
	}
	r = getsockname(fd, &sa_u.sa, &slen);
	if (r == -1) {
		(void)dispatch_assume_zero(errno);
		goto out_bad;
	}

	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0,
			&_dispatch_mgr_q);
	if (dispatch_assume(ds)) {
		_dispatch_log("LIBDISPATCH: debug port: %hu",
				(in_port_t)ntohs(sa_u.sa_in.sin_port));

		/* ownership of fd transfers to ds */
		dispatch_set_context(ds, (void *)(long)fd);
		dispatch_source_set_event_handler_f(ds, _dispatch_kevent_debugger2);
		dispatch_source_set_cancel_handler_f(ds,
				_dispatch_kevent_debugger2_cancel);
		dispatch_resume(ds);

		return;
	}
out_bad:
	close(fd);
}

#if HAVE_MACH

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

#endif // HAVE_MACH

#endif // DISPATCH_DEBUG
