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

static void _dispatch_source_handler_free(dispatch_source_t ds, long kind);
static void _dispatch_source_set_interval(dispatch_source_t ds, uint64_t interval);

#define DISPATCH_TIMERS_UNREGISTER 0x1
#define DISPATCH_TIMERS_RETAIN_2 0x2
static void _dispatch_timers_update(dispatch_unote_t du, uint32_t flags);
static void _dispatch_timers_unregister(dispatch_timer_source_refs_t dt);

static void _dispatch_source_timer_configure(dispatch_source_t ds);
static inline unsigned long _dispatch_source_timer_data(
		dispatch_source_t ds, dispatch_unote_t du);

#pragma mark -
#pragma mark dispatch_source_t

dispatch_source_t
dispatch_source_create(dispatch_source_type_t dst, uintptr_t handle,
		unsigned long mask, dispatch_queue_t dq)
{
	dispatch_source_refs_t dr;
	dispatch_source_t ds;

	dr = dux_create(dst, handle, mask)._dr;
	if (unlikely(!dr)) {
		return DISPATCH_BAD_INPUT;
	}

	ds = _dispatch_object_alloc(DISPATCH_VTABLE(source),
			sizeof(struct dispatch_source_s));
	// Initialize as a queue first, then override some settings below.
	_dispatch_queue_init(ds->_as_dq, DQF_LEGACY, 1,
			DISPATCH_QUEUE_INACTIVE | DISPATCH_QUEUE_ROLE_INNER);
	ds->dq_label = "source";
	ds->do_ref_cnt++; // the reference the manager queue holds
	ds->ds_refs = dr;
	dr->du_owner_wref = _dispatch_ptr2wref(ds);

	if (slowpath(!dq)) {
		dq = _dispatch_get_root_queue(DISPATCH_QOS_DEFAULT, true);
	} else {
		_dispatch_retain((dispatch_queue_t _Nonnull)dq);
	}
	ds->do_targetq = dq;
	if (dr->du_is_timer && (dr->du_fflags & DISPATCH_TIMER_INTERVAL)) {
		_dispatch_source_set_interval(ds, handle);
	}
	_dispatch_object_debug(ds, "%s", __func__);
	return ds;
}

void
_dispatch_source_dispose(dispatch_source_t ds, bool *allow_free)
{
	_dispatch_object_debug(ds, "%s", __func__);
	_dispatch_source_handler_free(ds, DS_REGISTN_HANDLER);
	_dispatch_source_handler_free(ds, DS_EVENT_HANDLER);
	_dispatch_source_handler_free(ds, DS_CANCEL_HANDLER);
	_dispatch_unote_dispose(ds->ds_refs);
	ds->ds_refs = NULL;
	_dispatch_queue_destroy(ds->_as_dq, allow_free);
}

void
_dispatch_source_xref_dispose(dispatch_source_t ds)
{
	dispatch_queue_flags_t dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	if (unlikely(!(dqf & (DQF_LEGACY|DSF_CANCELED)))) {
		DISPATCH_CLIENT_CRASH(ds, "Release of a source that has not been "
				"cancelled, but has a mandatory cancel handler");
	}
	dx_wakeup(ds, 0, DISPATCH_WAKEUP_MAKE_DIRTY);
}

long
dispatch_source_testcancel(dispatch_source_t ds)
{
	return (bool)(ds->dq_atomic_flags & DSF_CANCELED);
}

unsigned long
dispatch_source_get_mask(dispatch_source_t ds)
{
	dispatch_source_refs_t dr = ds->ds_refs;
	if (ds->dq_atomic_flags & DSF_CANCELED) {
		return 0;
	}
#if DISPATCH_USE_MEMORYSTATUS
	if (dr->du_vmpressure_override) {
		return NOTE_VM_PRESSURE;
	}
#if TARGET_IPHONE_SIMULATOR
	if (dr->du_memorypressure_override) {
		return NOTE_MEMORYSTATUS_PRESSURE_WARN;
	}
#endif
#endif // DISPATCH_USE_MEMORYSTATUS
	return dr->du_fflags;
}

uintptr_t
dispatch_source_get_handle(dispatch_source_t ds)
{
	dispatch_source_refs_t dr = ds->ds_refs;
#if TARGET_IPHONE_SIMULATOR
	if (dr->du_memorypressure_override) {
		return 0;
	}
#endif
	return dr->du_ident;
}

unsigned long
dispatch_source_get_data(dispatch_source_t ds)
{
#if DISPATCH_USE_MEMORYSTATUS
	dispatch_source_refs_t dr = ds->ds_refs;
	if (dr->du_vmpressure_override) {
		return NOTE_VM_PRESSURE;
	}
#if TARGET_IPHONE_SIMULATOR
	if (dr->du_memorypressure_override) {
		return NOTE_MEMORYSTATUS_PRESSURE_WARN;
	}
#endif
#endif // DISPATCH_USE_MEMORYSTATUS
	uint64_t value = os_atomic_load2o(ds, ds_data, relaxed);
	return (unsigned long)(
		ds->ds_refs->du_data_action == DISPATCH_UNOTE_ACTION_DATA_OR_STATUS_SET
		? DISPATCH_SOURCE_GET_DATA(value) : value);
}

size_t
dispatch_source_get_extended_data(dispatch_source_t ds,
		dispatch_source_extended_data_t edata, size_t size)
{
	size_t target_size = MIN(size,
		sizeof(struct dispatch_source_extended_data_s));
	if (size > 0) {
		unsigned long data, status = 0;
		if (ds->ds_refs->du_data_action
				== DISPATCH_UNOTE_ACTION_DATA_OR_STATUS_SET) {
			uint64_t combined = os_atomic_load(&ds->ds_data, relaxed);
			data = DISPATCH_SOURCE_GET_DATA(combined);
			status = DISPATCH_SOURCE_GET_STATUS(combined);
		} else {
			data = dispatch_source_get_data(ds);
		}
		if (size >= offsetof(struct dispatch_source_extended_data_s, data)
				+ sizeof(edata->data)) {
			edata->data = data;
		}
		if (size >= offsetof(struct dispatch_source_extended_data_s, status)
				+ sizeof(edata->status)) {
			edata->status = status;
		}
		if (size > sizeof(struct dispatch_source_extended_data_s)) {
			memset(
				(char *)edata + sizeof(struct dispatch_source_extended_data_s),
				0, size - sizeof(struct dispatch_source_extended_data_s));
		}
	}
	return target_size;
}

DISPATCH_NOINLINE
void
_dispatch_source_merge_data(dispatch_source_t ds, pthread_priority_t pp,
		unsigned long val)
{
	dispatch_queue_flags_t dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	int filter = ds->ds_refs->du_filter;

	if (unlikely(dqf & (DSF_CANCELED | DSF_DELETED))) {
		return;
	}

	switch (filter) {
	case DISPATCH_EVFILT_CUSTOM_ADD:
		os_atomic_add2o(ds, ds_pending_data, val, relaxed);
		break;
	case DISPATCH_EVFILT_CUSTOM_OR:
		os_atomic_or2o(ds, ds_pending_data, val, relaxed);
		break;
	case DISPATCH_EVFILT_CUSTOM_REPLACE:
		os_atomic_store2o(ds, ds_pending_data, val, relaxed);
		break;
	default:
		DISPATCH_CLIENT_CRASH(filter, "Invalid source type");
	}

	dx_wakeup(ds, _dispatch_qos_from_pp(pp), DISPATCH_WAKEUP_MAKE_DIRTY);
}

void
dispatch_source_merge_data(dispatch_source_t ds, unsigned long val)
{
	_dispatch_source_merge_data(ds, 0, val);
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
	if (unlikely(!_dispatch_queue_is_legacy(ds->_as_dq))) {
		DISPATCH_CLIENT_CRASH(kind, "Cannot change a handler of this source "
				"after it has been activated");
	}
	_dispatch_ktrace1(DISPATCH_PERF_post_activate_mutation, ds);
	if (kind == DS_REGISTN_HANDLER) {
		_dispatch_bug_deprecated("Setting registration handler after "
				"the source has been activated");
	}
	dc->dc_data = (void *)kind;
	_dispatch_barrier_trysync_or_async_f(ds->_as_dq, dc,
			_dispatch_source_set_handler_slow, 0);
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

#ifdef __BLOCKS__
DISPATCH_NOINLINE
static void
_dispatch_source_set_cancel_handler(dispatch_source_t ds,
		dispatch_block_t handler)
{
	dispatch_continuation_t dc;
	dc = _dispatch_source_handler_alloc(ds, handler, DS_CANCEL_HANDLER, true);
	_dispatch_source_set_handler(ds, DS_CANCEL_HANDLER, dc);
}

void
dispatch_source_set_cancel_handler(dispatch_source_t ds,
		dispatch_block_t handler)
{
	if (unlikely(!_dispatch_queue_is_legacy(ds->_as_dq))) {
		DISPATCH_CLIENT_CRASH(0, "Cannot set a non mandatory handler on "
				"this source");
	}
	return _dispatch_source_set_cancel_handler(ds, handler);
}

void
dispatch_source_set_mandatory_cancel_handler(dispatch_source_t ds,
		dispatch_block_t handler)
{
	_dispatch_queue_atomic_flags_clear(ds->_as_dq, DQF_LEGACY);
	return _dispatch_source_set_cancel_handler(ds, handler);
}
#endif /* __BLOCKS__ */

DISPATCH_NOINLINE
static void
_dispatch_source_set_cancel_handler_f(dispatch_source_t ds,
		dispatch_function_t handler)
{
	dispatch_continuation_t dc;
	dc = _dispatch_source_handler_alloc(ds, handler, DS_CANCEL_HANDLER, false);
	_dispatch_source_set_handler(ds, DS_CANCEL_HANDLER, dc);
}

void
dispatch_source_set_cancel_handler_f(dispatch_source_t ds,
		dispatch_function_t handler)
{
	if (unlikely(!_dispatch_queue_is_legacy(ds->_as_dq))) {
		DISPATCH_CLIENT_CRASH(0, "Cannot set a non mandatory handler on "
				"this source");
	}
	return _dispatch_source_set_cancel_handler_f(ds, handler);
}

void
dispatch_source_set_mandatory_cancel_handler_f(dispatch_source_t ds,
		dispatch_function_t handler)
{
	_dispatch_queue_atomic_flags_clear(ds->_as_dq, DQF_LEGACY);
	return _dispatch_source_set_cancel_handler_f(ds, handler);
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
	_dispatch_continuation_pop(dc, NULL, flags, cq);
}

static void
_dispatch_source_cancel_callout(dispatch_source_t ds, dispatch_queue_t cq,
		dispatch_invoke_flags_t flags)
{
	dispatch_continuation_t dc;

	dc = _dispatch_source_handler_take(ds, DS_CANCEL_HANDLER);
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
	_dispatch_continuation_pop(dc, NULL, flags, cq);
}

static void
_dispatch_source_latch_and_call(dispatch_source_t ds, dispatch_queue_t cq,
		dispatch_invoke_flags_t flags)
{
	dispatch_source_refs_t dr = ds->ds_refs;
	dispatch_continuation_t dc = _dispatch_source_get_handler(dr, DS_EVENT_HANDLER);
	uint64_t prev;

	if (dr->du_is_timer && !(dr->du_fflags & DISPATCH_TIMER_AFTER)) {
		prev = _dispatch_source_timer_data(ds, dr);
	} else {
		prev = os_atomic_xchg2o(ds, ds_pending_data, 0, relaxed);
	}
	if (dr->du_data_action == DISPATCH_UNOTE_ACTION_DATA_SET) {
		ds->ds_data = ~prev;
	} else {
		ds->ds_data = prev;
	}
	if (!dispatch_assume(prev != 0) || !dc) {
		return;
	}
	_dispatch_continuation_pop(dc, NULL, flags, cq);
	if (dr->du_is_timer && (dr->du_fflags & DISPATCH_TIMER_AFTER)) {
		_dispatch_source_handler_free(ds, DS_EVENT_HANDLER);
		dispatch_release(ds); // dispatch_after sources are one-shot
	}
}

DISPATCH_NOINLINE
static void
_dispatch_source_refs_finalize_unregistration(dispatch_source_t ds)
{
	dispatch_queue_flags_t dqf;
	dispatch_source_refs_t dr = ds->ds_refs;

	dqf = _dispatch_queue_atomic_flags_set_and_clear_orig(ds->_as_dq,
			DSF_DELETED, DSF_ARMED | DSF_DEFERRED_DELETE | DSF_CANCEL_WAITER);
	if (dqf & DSF_CANCEL_WAITER) {
		_dispatch_wake_by_address(&ds->dq_atomic_flags);
	}
	_dispatch_debug("kevent-source[%p]: disarmed kevent[%p]", ds, dr);
	_dispatch_release_tailcall(ds); // the retain is done at creation time
}

void
_dispatch_source_refs_unregister(dispatch_source_t ds, uint32_t options)
{
	_dispatch_object_debug(ds, "%s", __func__);
	dispatch_queue_flags_t dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	dispatch_source_refs_t dr = ds->ds_refs;

	if (dr->du_is_timer) {
		// Because of the optimization to unregister fired oneshot timers
		// from the target queue, we can't trust _dispatch_unote_registered()
		// to tell the truth, it may not have happened yet
		if (dqf & DSF_ARMED) {
			_dispatch_timers_unregister(ds->ds_timer_refs);
			_dispatch_release_2(ds);
		}
		dr->du_ident = DISPATCH_TIMER_IDENT_CANCELED;
	} else {
		if (_dispatch_unote_needs_rearm(dr) && !(dqf & DSF_ARMED)) {
			options |= DU_UNREGISTER_IMMEDIATE_DELETE;
		}
		if (!_dispatch_unote_unregister(dr, options)) {
			_dispatch_debug("kevent-source[%p]: deferred delete kevent[%p]",
					ds, dr);
			_dispatch_queue_atomic_flags_set(ds->_as_dq, DSF_DEFERRED_DELETE);
			return; // deferred unregistration
		}
	}

	ds->ds_is_installed = true;
	_dispatch_source_refs_finalize_unregistration(ds);
}

DISPATCH_ALWAYS_INLINE
static inline bool
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

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_source_refs_resume(dispatch_source_t ds)
{
	dispatch_source_refs_t dr = ds->ds_refs;
	if (dr->du_is_timer) {
		_dispatch_timers_update(dr, 0);
		return true;
	}
	if (unlikely(!_dispatch_source_tryarm(ds))) {
		return false;
	}
	_dispatch_unote_resume(dr);
	_dispatch_debug("kevent-source[%p]: rearmed kevent[%p]", ds, dr);
	return true;
}

void
_dispatch_source_refs_register(dispatch_source_t ds, dispatch_wlh_t wlh,
		dispatch_priority_t pri)
{
	dispatch_source_refs_t dr = ds->ds_refs;
	dispatch_priority_t kbp;

	dispatch_assert(!ds->ds_is_installed);

	if (dr->du_is_timer) {
		dispatch_queue_t dq = ds->_as_dq;
		kbp = _dispatch_queue_compute_priority_and_wlh(dq, NULL);
		// aggressively coalesce background/maintenance QoS timers
		// <rdar://problem/12200216&27342536>
		if (_dispatch_qos_is_background(_dispatch_priority_qos(kbp))) {
			if (dr->du_fflags & DISPATCH_TIMER_STRICT) {
				_dispatch_ktrace1(DISPATCH_PERF_strict_bg_timer, ds);
			} else {
				dr->du_fflags |= DISPATCH_TIMER_BACKGROUND;
				dr->du_ident = _dispatch_source_timer_idx(dr);
			}
		}
		_dispatch_timers_update(dr, 0);
		return;
	}

	if (unlikely(!_dispatch_source_tryarm(ds) ||
			!_dispatch_unote_register(dr, wlh, pri))) {
		// Do the parts of dispatch_source_refs_unregister() that
		// are required after this partial initialization.
		_dispatch_source_refs_finalize_unregistration(ds);
	} else {
		_dispatch_debug("kevent-source[%p]: armed kevent[%p]", ds, dr);
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

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_source_install(dispatch_source_t ds, dispatch_wlh_t wlh,
		dispatch_priority_t pri)
{
	_dispatch_source_refs_register(ds, wlh, pri);
	ds->ds_is_installed = true;
}

void
_dispatch_source_finalize_activation(dispatch_source_t ds, bool *allow_resume)
{
	dispatch_continuation_t dc;
	dispatch_source_refs_t dr = ds->ds_refs;
	dispatch_priority_t pri;
	dispatch_wlh_t wlh;

	if (unlikely(dr->du_is_direct &&
			(_dispatch_queue_atomic_flags(ds->_as_dq) & DSF_CANCELED))) {
		return _dispatch_source_refs_unregister(ds, 0);
	}

	dc = _dispatch_source_get_event_handler(dr);
	if (dc) {
		if (_dispatch_object_is_barrier(dc)) {
			_dispatch_queue_atomic_flags_set(ds->_as_dq, DQF_BARRIER_BIT);
		}
		ds->dq_priority = _dispatch_priority_from_pp_strip_flags(dc->dc_priority);
		if (dc->dc_flags & DISPATCH_OBJ_CTXT_FETCH_BIT) {
			_dispatch_barrier_async_detached_f(ds->_as_dq, ds,
					_dispatch_source_set_event_handler_context);
		}
	}

	// call "super"
	_dispatch_queue_finalize_activation(ds->_as_dq, allow_resume);

	if (dr->du_is_direct && !ds->ds_is_installed) {
		dispatch_queue_t dq = ds->_as_dq;
		pri = _dispatch_queue_compute_priority_and_wlh(dq, &wlh);
		if (pri) _dispatch_source_install(ds, wlh, pri);
	}
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_wakeup_target_t
_dispatch_source_invoke2(dispatch_object_t dou, dispatch_invoke_context_t dic,
		dispatch_invoke_flags_t flags, uint64_t *owned)
{
	dispatch_source_t ds = dou._ds;
	dispatch_queue_wakeup_target_t retq = DISPATCH_QUEUE_WAKEUP_NONE;
	dispatch_queue_t dq = _dispatch_queue_get_current();
	dispatch_source_refs_t dr = ds->ds_refs;
	dispatch_queue_flags_t dqf;

	if (!(flags & DISPATCH_INVOKE_MANAGER_DRAIN) &&
			_dispatch_unote_wlh_changed(dr, _dispatch_get_wlh())) {
		dqf = _dispatch_queue_atomic_flags_set_orig(ds->_as_dq,
				DSF_WLH_CHANGED);
		if (!(dqf & DSF_WLH_CHANGED)) {
			_dispatch_bug_deprecated("Changing target queue "
					"hierarchy after source was activated");
		}
	}

	if (_dispatch_queue_class_probe(ds)) {
		// Intentionally always drain even when on the manager queue
		// and not the source's regular target queue: we need to be able
		// to drain timer setting and the like there.
		dispatch_with_disabled_narrowing(dic, {
			retq = _dispatch_queue_serial_drain(ds->_as_dq, dic, flags, owned);
		});
	}

	// This function performs all source actions. Each action is responsible
	// for verifying that it takes place on the appropriate queue. If the
	// current queue is not the correct queue for this action, the correct queue
	// will be returned and the invoke will be re-driven on that queue.

	// The order of tests here in invoke and in wakeup should be consistent.

	dispatch_queue_t dkq = &_dispatch_mgr_q;
	bool prevent_starvation = false;

	if (dr->du_is_direct) {
		dkq = ds->do_targetq;
	}

	if (dr->du_is_timer &&
			os_atomic_load2o(ds, ds_timer_refs->dt_pending_config, relaxed)) {
		dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
		if (!(dqf & (DSF_CANCELED | DQF_RELEASED))) {
			// timer has to be configured on the kevent queue
			if (dq != dkq) {
				return dkq;
			}
			_dispatch_source_timer_configure(ds);
		}
	}

	if (!ds->ds_is_installed) {
		// The source needs to be installed on the kevent queue.
		if (dq != dkq) {
			return dkq;
		}
		_dispatch_source_install(ds, _dispatch_get_wlh(),
				_dispatch_get_basepri());
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

	dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	if ((dqf & DSF_DEFERRED_DELETE) && !(dqf & DSF_ARMED)) {
unregister_event:
		// DSF_DELETE: Pending source kevent unregistration has been completed
		// !DSF_ARMED: event was delivered and can safely be unregistered
		if (dq != dkq) {
			return dkq;
		}
		_dispatch_source_refs_unregister(ds, DU_UNREGISTER_IMMEDIATE_DELETE);
		dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	}

	if (!(dqf & (DSF_CANCELED | DQF_RELEASED)) &&
			os_atomic_load2o(ds, ds_pending_data, relaxed)) {
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
			// however, if the source is directly targeting an overcommit root
			// queue, this would requeue the source and ask for a new overcommit
			// thread right away.
			prevent_starvation = dq->do_targetq ||
					!(dq->dq_priority & DISPATCH_PRIORITY_FLAG_OVERCOMMIT);
			if (prevent_starvation &&
					os_atomic_load2o(ds, ds_pending_data, relaxed)) {
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
			if (dr->du_is_timer && !(dqf & DSF_ARMED)) {
				// timers can cheat if not armed because there's nothing left
				// to do on the manager queue and unregistration can happen
				// on the regular target queue
			} else if (dq != dkq) {
				return dkq;
			}
			_dispatch_source_refs_unregister(ds, 0);
			dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
			if (unlikely(dqf & DSF_DEFERRED_DELETE)) {
				if (!(dqf & DSF_ARMED)) {
					goto unregister_event;
				}
				// we need to wait for the EV_DELETE
				return retq ? retq : DISPATCH_QUEUE_WAKEUP_WAIT_FOR_EVENT;
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

	if (_dispatch_unote_needs_rearm(dr) &&
			!(dqf & (DSF_ARMED|DSF_DELETED|DSF_CANCELED|DQF_RELEASED))) {
		// The source needs to be rearmed on the kevent queue.
		if (dq != dkq) {
			return dkq;
		}
		if (unlikely(dqf & DSF_DEFERRED_DELETE)) {
			// no need for resume when we can directly unregister the kevent
			goto unregister_event;
		}
		if (unlikely(DISPATCH_QUEUE_IS_SUSPENDED(ds))) {
			// do not try to rearm the kevent if the source is suspended
			// from the source handler
			return ds->do_targetq;
		}
		if (prevent_starvation && dr->du_wlh == DISPATCH_WLH_ANON) {
			// keep the old behavior to force re-enqueue to our target queue
			// for the rearm.
			//
			// if the handler didn't run, or this is a pending delete
			// or our target queue is a global queue, then starvation is
			// not a concern and we can rearm right away.
			return ds->do_targetq;
		}
		if (unlikely(!_dispatch_source_refs_resume(ds))) {
			goto unregister_event;
		}
		if (!prevent_starvation && _dispatch_wlh_should_poll_unote(dr)) {
			// try to redrive the drain from under the lock for sources
			// targeting an overcommit root queue to avoid parking
			// when the next event has already fired
			_dispatch_event_loop_drain(KEVENT_FLAG_IMMEDIATE);
		}
	}

	return retq;
}

DISPATCH_NOINLINE
void
_dispatch_source_invoke(dispatch_source_t ds, dispatch_invoke_context_t dic,
		dispatch_invoke_flags_t flags)
{
	_dispatch_queue_class_invoke(ds, dic, flags,
			DISPATCH_INVOKE_DISALLOW_SYNC_WAITERS, _dispatch_source_invoke2);
}

void
_dispatch_source_wakeup(dispatch_source_t ds, dispatch_qos_t qos,
		dispatch_wakeup_flags_t flags)
{
	// This function determines whether the source needs to be invoked.
	// The order of tests here in wakeup and in invoke should be consistent.

	dispatch_source_refs_t dr = ds->ds_refs;
	dispatch_queue_wakeup_target_t dkq = DISPATCH_QUEUE_WAKEUP_MGR;
	dispatch_queue_wakeup_target_t tq = DISPATCH_QUEUE_WAKEUP_NONE;
	dispatch_queue_flags_t dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	bool deferred_delete = (dqf & DSF_DEFERRED_DELETE);

	if (dr->du_is_direct) {
		dkq = DISPATCH_QUEUE_WAKEUP_TARGET;
	}

	if (!(dqf & (DSF_CANCELED | DQF_RELEASED)) && dr->du_is_timer &&
			os_atomic_load2o(ds, ds_timer_refs->dt_pending_config, relaxed)) {
		// timer has to be configured on the kevent queue
		tq = dkq;
	} else if (!ds->ds_is_installed) {
		// The source needs to be installed on the kevent queue.
		tq = dkq;
	} else if (_dispatch_source_get_registration_handler(dr)) {
		// The registration handler needs to be delivered to the target queue.
		tq = DISPATCH_QUEUE_WAKEUP_TARGET;
	} else if (deferred_delete && !(dqf & DSF_ARMED)) {
		// Pending source kevent unregistration has been completed
		// or EV_ONESHOT event can be acknowledged
		tq = dkq;
	} else if (!(dqf & (DSF_CANCELED | DQF_RELEASED)) &&
			os_atomic_load2o(ds, ds_pending_data, relaxed)) {
		// The source has pending data to deliver to the target queue.
		tq = DISPATCH_QUEUE_WAKEUP_TARGET;
	} else if ((dqf & (DSF_CANCELED | DQF_RELEASED)) && !deferred_delete) {
		// The source needs to be uninstalled from the kevent queue, or the
		// cancellation handler needs to be delivered to the target queue.
		// Note: cancellation assumes installation.
		if (!(dqf & DSF_DELETED)) {
			if (dr->du_is_timer && !(dqf & DSF_ARMED)) {
				// timers can cheat if not armed because there's nothing left
				// to do on the manager queue and unregistration can happen
				// on the regular target queue
				tq = DISPATCH_QUEUE_WAKEUP_TARGET;
			} else {
				tq = dkq;
			}
		} else if (_dispatch_source_get_event_handler(dr) ||
				_dispatch_source_get_cancel_handler(dr) ||
				_dispatch_source_get_registration_handler(dr)) {
			tq = DISPATCH_QUEUE_WAKEUP_TARGET;
		}
	} else if (_dispatch_unote_needs_rearm(dr) &&
			!(dqf & (DSF_ARMED|DSF_DELETED|DSF_CANCELED|DQF_RELEASED))) {
		// The source needs to be rearmed on the kevent queue.
		tq = dkq;
	}
	if (!tq && _dispatch_queue_class_probe(ds)) {
		tq = DISPATCH_QUEUE_WAKEUP_TARGET;
	}

	if ((tq == DISPATCH_QUEUE_WAKEUP_TARGET) &&
			ds->do_targetq == &_dispatch_mgr_q) {
		tq = DISPATCH_QUEUE_WAKEUP_MGR;
	}

	return _dispatch_queue_class_wakeup(ds->_as_dq, qos, flags, tq);
}

void
dispatch_source_cancel(dispatch_source_t ds)
{
	_dispatch_object_debug(ds, "%s", __func__);
	// Right after we set the cancel flag, someone else
	// could potentially invoke the source, do the cancellation,
	// unregister the source, and deallocate it. We would
	// need to therefore retain/release before setting the bit
	_dispatch_retain_2(ds);

	dispatch_queue_t q = ds->_as_dq;
	if (_dispatch_queue_atomic_flags_set_orig(q, DSF_CANCELED) & DSF_CANCELED) {
		_dispatch_release_2_tailcall(ds);
	} else {
		dx_wakeup(ds, 0, DISPATCH_WAKEUP_MAKE_DIRTY | DISPATCH_WAKEUP_CONSUME_2);
	}
}

void
dispatch_source_cancel_and_wait(dispatch_source_t ds)
{
	dispatch_queue_flags_t old_dqf, dqf, new_dqf;
	dispatch_source_refs_t dr = ds->ds_refs;

	if (unlikely(_dispatch_source_get_cancel_handler(dr))) {
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
		} else if ((old_dqf & DSF_DEFERRED_DELETE) || !dr->du_is_direct) {
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
		goto wakeup;
	}

	// simplified version of _dispatch_queue_drain_try_lock
	// that also sets the DIRTY bit on failure to lock
	uint64_t set_owner_and_set_full_width = _dispatch_lock_value_for_self() |
			DISPATCH_QUEUE_WIDTH_FULL_BIT | DISPATCH_QUEUE_IN_BARRIER;
	uint64_t old_state, new_state;

	os_atomic_rmw_loop2o(ds, dq_state, old_state, new_state, seq_cst, {
		new_state = old_state;
		if (likely(_dq_state_is_runnable(old_state) &&
				!_dq_state_drain_locked(old_state))) {
			new_state &= DISPATCH_QUEUE_DRAIN_PRESERVED_BITS_MASK;
			new_state |= set_owner_and_set_full_width;
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
			_dispatch_source_refs_unregister(ds, 0);
			dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
			if (likely((dqf & DSF_STATE_MASK) == DSF_DELETED)) {
				_dispatch_source_cancel_callout(ds, NULL, DISPATCH_INVOKE_NONE);
			}
		}
		dx_wakeup(ds, 0, DISPATCH_WAKEUP_BARRIER_COMPLETE);
	} else if (unlikely(_dq_state_drain_locked_by_self(old_state))) {
		DISPATCH_CLIENT_CRASH(ds, "dispatch_source_cancel_and_wait "
				"called from a source handler");
	} else {
		dispatch_qos_t qos;
wakeup:
		qos = _dispatch_qos_from_pp(_dispatch_get_priority());
		dx_wakeup(ds, qos, DISPATCH_WAKEUP_MAKE_DIRTY);
		dispatch_activate(ds);
	}

	dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	while (unlikely((dqf & DSF_STATE_MASK) != DSF_DELETED)) {
		if (unlikely(!(dqf & DSF_CANCEL_WAITER))) {
			if (!os_atomic_cmpxchgv2o(ds, dq_atomic_flags,
					dqf, dqf | DSF_CANCEL_WAITER, &dqf, relaxed)) {
				continue;
			}
			dqf |= DSF_CANCEL_WAITER;
		}
		_dispatch_wait_on_address(&ds->dq_atomic_flags, dqf, DLOCK_LOCK_NONE);
		dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	}
}

void
_dispatch_source_merge_evt(dispatch_unote_t du, uint32_t flags, uintptr_t data,
		uintptr_t status, pthread_priority_t pp)
{
	dispatch_source_refs_t dr = du._dr;
	dispatch_source_t ds = _dispatch_source_from_refs(dr);
	dispatch_wakeup_flags_t wflags = 0;
	dispatch_queue_flags_t dqf;

	if (_dispatch_unote_needs_rearm(dr) || (flags & (EV_DELETE | EV_ONESHOT))) {
		// once we modify the queue atomic flags below, it will allow concurrent
		// threads running _dispatch_source_invoke2 to dispose of the source,
		// so we can't safely borrow the reference we get from the muxnote udata
		// anymore, and need our own
		wflags = DISPATCH_WAKEUP_CONSUME_2;
		_dispatch_retain_2(ds); // rdar://20382435
	}

	if ((flags & EV_UDATA_SPECIFIC) && (flags & EV_ONESHOT) &&
			!(flags & EV_DELETE)) {
		dqf = _dispatch_queue_atomic_flags_set_and_clear(ds->_as_dq,
				DSF_DEFERRED_DELETE, DSF_ARMED);
		if (flags & EV_VANISHED) {
			_dispatch_bug_kevent_client("kevent", dr->du_type->dst_kind,
					"monitored resource vanished before the source "
					"cancel handler was invoked", 0);
		}
		_dispatch_debug("kevent-source[%p]: %s kevent[%p]", ds,
				(flags & EV_VANISHED) ? "vanished" :
				"deferred delete oneshot", dr);
	} else if (flags & (EV_DELETE | EV_ONESHOT)) {
		_dispatch_source_refs_unregister(ds, DU_UNREGISTER_ALREADY_DELETED);
		_dispatch_debug("kevent-source[%p]: deleted kevent[%p]", ds, dr);
		if (flags & EV_DELETE) goto done;
		dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	} else if (_dispatch_unote_needs_rearm(dr)) {
		dqf = _dispatch_queue_atomic_flags_clear(ds->_as_dq, DSF_ARMED);
		_dispatch_debug("kevent-source[%p]: disarmed kevent[%p]", ds, dr);
	} else {
		dqf = _dispatch_queue_atomic_flags(ds->_as_dq);
	}

	if (dqf & (DSF_CANCELED | DQF_RELEASED)) {
		goto done; // rdar://20204025
	}

	dispatch_unote_action_t action = dr->du_data_action;
	if ((flags & EV_UDATA_SPECIFIC) && (flags & EV_ONESHOT) &&
			(flags & EV_VANISHED)) {
		// if the resource behind the ident vanished, the event handler can't
		// do anything useful anymore, so do not try to call it at all
		//
		// Note: if the kernel doesn't support EV_VANISHED we always get it
		// back unchanged from the flags passed at EV_ADD (registration) time
		// Since we never ask for both EV_ONESHOT and EV_VANISHED for sources,
		// if we get both bits it was a real EV_VANISHED delivery
		os_atomic_store2o(ds, ds_pending_data, 0, relaxed);
#if HAVE_MACH
	} else if (dr->du_filter == EVFILT_MACHPORT) {
		os_atomic_store2o(ds, ds_pending_data, data, relaxed);
#endif
	} else if (action == DISPATCH_UNOTE_ACTION_DATA_SET) {
		os_atomic_store2o(ds, ds_pending_data, data, relaxed);
	} else if (action == DISPATCH_UNOTE_ACTION_DATA_ADD) {
		os_atomic_add2o(ds, ds_pending_data, data, relaxed);
	} else if (data && action == DISPATCH_UNOTE_ACTION_DATA_OR) {
		os_atomic_or2o(ds, ds_pending_data, data, relaxed);
	} else if (data && action == DISPATCH_UNOTE_ACTION_DATA_OR_STATUS_SET) {
		// We combine the data and status into a single 64-bit value.
		uint64_t odata, ndata;
		uint64_t value = DISPATCH_SOURCE_COMBINE_DATA_AND_STATUS(data, status);
		os_atomic_rmw_loop2o(ds, ds_pending_data, odata, ndata, relaxed, {
            ndata = DISPATCH_SOURCE_GET_DATA(odata) | value;
		});
	} else if (data) {
		DISPATCH_INTERNAL_CRASH(action, "Unexpected source action value");
	}
	_dispatch_debug("kevent-source[%p]: merged kevent[%p]", ds, dr);

done:
	_dispatch_object_debug(ds, "%s", __func__);
	dx_wakeup(ds, _dispatch_qos_from_pp(pp), wflags | DISPATCH_WAKEUP_MAKE_DIRTY);
}

#pragma mark -
#pragma mark dispatch_source_timer

#if DISPATCH_USE_DTRACE
static dispatch_timer_source_refs_t
		_dispatch_trace_next_timer[DISPATCH_TIMER_QOS_COUNT];
#define _dispatch_trace_next_timer_set(x, q) \
		_dispatch_trace_next_timer[(q)] = (x)
#define _dispatch_trace_next_timer_program(d, q) \
		_dispatch_trace_timer_program(_dispatch_trace_next_timer[(q)], (d))
DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_mgr_trace_timers_wakes(void)
{
	uint32_t qos;

	if (_dispatch_timers_will_wake) {
		if (slowpath(DISPATCH_TIMER_WAKE_ENABLED())) {
			for (qos = 0; qos < DISPATCH_TIMER_QOS_COUNT; qos++) {
				if (_dispatch_timers_will_wake & (1 << qos)) {
					_dispatch_trace_timer_wake(_dispatch_trace_next_timer[qos]);
				}
			}
		}
		_dispatch_timers_will_wake = 0;
	}
}
#else
#define _dispatch_trace_next_timer_set(x, q)
#define _dispatch_trace_next_timer_program(d, q)
#define _dispatch_mgr_trace_timers_wakes()
#endif

#define _dispatch_source_timer_telemetry_enabled() false

DISPATCH_NOINLINE
static void
_dispatch_source_timer_telemetry_slow(dispatch_source_t ds,
		dispatch_clock_t clock, struct dispatch_timer_source_s *values)
{
	if (_dispatch_trace_timer_configure_enabled()) {
		_dispatch_trace_timer_configure(ds, clock, values);
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_source_timer_telemetry(dispatch_source_t ds, dispatch_clock_t clock,
		struct dispatch_timer_source_s *values)
{
	if (_dispatch_trace_timer_configure_enabled() ||
			_dispatch_source_timer_telemetry_enabled()) {
		_dispatch_source_timer_telemetry_slow(ds, clock, values);
		asm(""); // prevent tailcall
	}
}

DISPATCH_NOINLINE
static void
_dispatch_source_timer_configure(dispatch_source_t ds)
{
	dispatch_timer_source_refs_t dt = ds->ds_timer_refs;
	dispatch_timer_config_t dtc;

	dtc = os_atomic_xchg2o(dt, dt_pending_config, NULL, dependency);
	if (dtc->dtc_clock == DISPATCH_CLOCK_MACH) {
		dt->du_fflags |= DISPATCH_TIMER_CLOCK_MACH;
	} else {
		dt->du_fflags &= ~(uint32_t)DISPATCH_TIMER_CLOCK_MACH;
	}
	dt->dt_timer = dtc->dtc_timer;
	free(dtc);
	if (ds->ds_is_installed) {
		// Clear any pending data that might have accumulated on
		// older timer params <rdar://problem/8574886>
		os_atomic_store2o(ds, ds_pending_data, 0, relaxed);
		_dispatch_timers_update(dt, 0);
	}
}

static dispatch_timer_config_t
_dispatch_source_timer_config_create(dispatch_time_t start,
		uint64_t interval, uint64_t leeway)
{
	dispatch_timer_config_t dtc;
	dtc = _dispatch_calloc(1ul, sizeof(struct dispatch_timer_config_s));
	if (unlikely(interval == 0)) {
		if (start != DISPATCH_TIME_FOREVER) {
			_dispatch_bug_deprecated("Setting timer interval to 0 requests "
					"a 1ns timer, did you mean FOREVER (a one-shot timer)?");
		}
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
		dtc->dtc_clock = DISPATCH_CLOCK_WALL;
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
		dtc->dtc_clock = DISPATCH_CLOCK_MACH;
	}
	if (interval < INT64_MAX && leeway > interval / 2) {
		leeway = interval / 2;
	}

	dtc->dtc_timer.target = start;
	dtc->dtc_timer.interval = interval;
	if (start + leeway < INT64_MAX) {
		dtc->dtc_timer.deadline = start + leeway;
	} else {
		dtc->dtc_timer.deadline = INT64_MAX;
	}
	return dtc;
}

DISPATCH_NOINLINE
void
dispatch_source_set_timer(dispatch_source_t ds, dispatch_time_t start,
		uint64_t interval, uint64_t leeway)
{
	dispatch_timer_source_refs_t dt = ds->ds_timer_refs;
	dispatch_timer_config_t dtc;

	if (unlikely(!dt->du_is_timer || (dt->du_fflags&DISPATCH_TIMER_INTERVAL))) {
		DISPATCH_CLIENT_CRASH(ds, "Attempt to set timer on a non-timer source");
	}

	dtc = _dispatch_source_timer_config_create(start, interval, leeway);
	_dispatch_source_timer_telemetry(ds, dtc->dtc_clock, &dtc->dtc_timer);
	dtc = os_atomic_xchg2o(dt, dt_pending_config, dtc, release);
	if (dtc) free(dtc);
	dx_wakeup(ds, 0, DISPATCH_WAKEUP_MAKE_DIRTY);
}

static void
_dispatch_source_set_interval(dispatch_source_t ds, uint64_t interval)
{
#define NSEC_PER_FRAME (NSEC_PER_SEC/60)
// approx 1 year (60s * 60m * 24h * 365d)
#define FOREVER_NSEC 31536000000000000ull

	dispatch_timer_source_refs_t dr = ds->ds_timer_refs;
	const bool animation = dr->du_fflags & DISPATCH_INTERVAL_UI_ANIMATION;
	if (fastpath(interval <= (animation ? FOREVER_NSEC/NSEC_PER_FRAME :
			FOREVER_NSEC/NSEC_PER_MSEC))) {
		interval *= animation ? NSEC_PER_FRAME : NSEC_PER_MSEC;
	} else {
		interval = FOREVER_NSEC;
	}
	interval = _dispatch_time_nano2mach(interval);
	uint64_t target = _dispatch_absolute_time() + interval;
	target -= (target % interval);
	const uint64_t leeway = animation ?
			_dispatch_time_nano2mach(NSEC_PER_FRAME) : interval / 2;
	dr->dt_timer.target = target;
	dr->dt_timer.deadline = target + leeway;
	dr->dt_timer.interval = interval;
	_dispatch_source_timer_telemetry(ds, DISPATCH_CLOCK_MACH, &dr->dt_timer);
}

#pragma mark -
#pragma mark dispatch_after

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_after(dispatch_time_t when, dispatch_queue_t queue,
		void *ctxt, void *handler, bool block)
{
	dispatch_timer_source_refs_t dt;
	dispatch_source_t ds;
	uint64_t leeway, delta;

	if (when == DISPATCH_TIME_FOREVER) {
#if DISPATCH_DEBUG
		DISPATCH_CLIENT_CRASH(0, "dispatch_after called with 'when' == infinity");
#endif
		return;
	}

	delta = _dispatch_timeout(when);
	if (delta == 0) {
		if (block) {
			return dispatch_async(queue, handler);
		}
		return dispatch_async_f(queue, ctxt, handler);
	}
	leeway = delta / 10; // <rdar://problem/13447496>

	if (leeway < NSEC_PER_MSEC) leeway = NSEC_PER_MSEC;
	if (leeway > 60 * NSEC_PER_SEC) leeway = 60 * NSEC_PER_SEC;

	// this function can and should be optimized to not use a dispatch source
	ds = dispatch_source_create(&_dispatch_source_type_after, 0, 0, queue);
	dt = ds->ds_timer_refs;

	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	if (block) {
		_dispatch_continuation_init(dc, ds, handler, 0, 0, 0);
	} else {
		_dispatch_continuation_init_f(dc, ds, ctxt, handler, 0, 0, 0);
	}
	// reference `ds` so that it doesn't show up as a leak
	dc->dc_data = ds;
	_dispatch_trace_continuation_push(ds->_as_dq, dc);
	os_atomic_store2o(dt, ds_handler[DS_EVENT_HANDLER], dc, relaxed);

	if ((int64_t)when < 0) {
		// wall clock
		when = (dispatch_time_t)-((int64_t)when);
	} else {
		// absolute clock
		dt->du_fflags |= DISPATCH_TIMER_CLOCK_MACH;
		leeway = _dispatch_time_nano2mach(leeway);
	}
	dt->dt_timer.target = when;
	dt->dt_timer.interval = UINT64_MAX;
	dt->dt_timer.deadline = when + leeway;
	dispatch_activate(ds);
}

DISPATCH_NOINLINE
void
dispatch_after_f(dispatch_time_t when, dispatch_queue_t queue, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_after(when, queue, ctxt, func, false);
}

#ifdef __BLOCKS__
void
dispatch_after(dispatch_time_t when, dispatch_queue_t queue,
		dispatch_block_t work)
{
	_dispatch_after(when, queue, NULL, work, true);
}
#endif

#pragma mark -
#pragma mark dispatch_timers

/*
 * The dispatch_timer_heap_t structure is a double min-heap of timers,
 * interleaving the by-target min-heap in the even slots, and the by-deadline
 * in the odd ones.
 *
 * The min element of these is held inline in the dispatch_timer_heap_t
 * structure, and further entries are held in segments.
 *
 * dth_segments is the number of allocated segments.
 *
 * Segment 0 has a size of `DISPATCH_HEAP_INIT_SEGMENT_CAPACITY` pointers
 * Segment k has a size of (DISPATCH_HEAP_INIT_SEGMENT_CAPACITY << (k - 1))
 *
 * Segment n (dth_segments - 1) is the last segment and points its final n
 * entries to previous segments. Its address is held in the `dth_heap` field.
 *
 * segment n   [ regular timer pointers | n-1 | k | 0 ]
 *                                         |    |   |
 * segment n-1 <---------------------------'    |   |
 * segment k   <--------------------------------'   |
 * segment 0   <------------------------------------'
 */
#define DISPATCH_HEAP_INIT_SEGMENT_CAPACITY 8u

/*
 * There are two min-heaps stored interleaved in a single array,
 * even indices are for the by-target min-heap, and odd indices for
 * the by-deadline one.
 */
#define DTH_HEAP_ID_MASK (DTH_ID_COUNT - 1)
#define DTH_HEAP_ID(idx) ((idx) & DTH_HEAP_ID_MASK)
#define DTH_IDX_FOR_HEAP_ID(idx, heap_id) \
		(((idx) & ~DTH_HEAP_ID_MASK) | (heap_id))

DISPATCH_ALWAYS_INLINE
static inline uint32_t
_dispatch_timer_heap_capacity(uint32_t segments)
{
	if (segments == 0) return 2;
	uint32_t seg_no = segments - 1;
	// for C = DISPATCH_HEAP_INIT_SEGMENT_CAPACITY,
	// 2 + C + SUM(C << (i-1), i = 1..seg_no) - seg_no
	return 2 + (DISPATCH_HEAP_INIT_SEGMENT_CAPACITY << seg_no) - seg_no;
}

DISPATCH_NOINLINE
static void
_dispatch_timer_heap_grow(dispatch_timer_heap_t dth)
{
	uint32_t seg_capacity = DISPATCH_HEAP_INIT_SEGMENT_CAPACITY;
	uint32_t seg_no = dth->dth_segments++;
	void **heap, **heap_prev = dth->dth_heap;

	if (seg_no > 0) {
		seg_capacity <<= (seg_no - 1);
	}
	heap = _dispatch_calloc(seg_capacity, sizeof(void *));
	if (seg_no > 1) {
		uint32_t prev_seg_no = seg_no - 1;
		uint32_t prev_seg_capacity = seg_capacity >> 1;
		memcpy(&heap[seg_capacity - prev_seg_no],
				&heap_prev[prev_seg_capacity - prev_seg_no],
				prev_seg_no * sizeof(void *));
	}
	if (seg_no > 0) {
		heap[seg_capacity - seg_no] = heap_prev;
	}
	dth->dth_heap = heap;
}

DISPATCH_NOINLINE
static void
_dispatch_timer_heap_shrink(dispatch_timer_heap_t dth)
{
	uint32_t seg_capacity = DISPATCH_HEAP_INIT_SEGMENT_CAPACITY;
	uint32_t seg_no = --dth->dth_segments;
	void **heap = dth->dth_heap, **heap_prev = NULL;

	if (seg_no > 0) {
		seg_capacity <<= (seg_no - 1);
		heap_prev = heap[seg_capacity - seg_no];
	}
	if (seg_no > 1) {
		uint32_t prev_seg_no = seg_no - 1;
		uint32_t prev_seg_capacity = seg_capacity >> 1;
		memcpy(&heap_prev[prev_seg_capacity - prev_seg_no],
				&heap[seg_capacity - prev_seg_no],
				prev_seg_no * sizeof(void *));
	}
	dth->dth_heap = heap_prev;
	free(heap);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_timer_source_refs_t *
_dispatch_timer_heap_get_slot(dispatch_timer_heap_t dth, uint32_t idx)
{
	uint32_t seg_no, segments = dth->dth_segments;
	void **segment;

	if (idx < DTH_ID_COUNT) {
		return &dth->dth_min[idx];
	}
	idx -= DTH_ID_COUNT;

	// Derive the segment number from the index. Naming
	// DISPATCH_HEAP_INIT_SEGMENT_CAPACITY `C`, the segments index ranges are:
	// 0: 0 .. (C - 1)
	// 1: C .. 2 * C - 1
	// k: 2^(k-1) * C .. 2^k * C - 1
	// so `k` can be derived from the first bit set in `idx`
	seg_no = (uint32_t)(__builtin_clz(DISPATCH_HEAP_INIT_SEGMENT_CAPACITY - 1) -
			__builtin_clz(idx | (DISPATCH_HEAP_INIT_SEGMENT_CAPACITY - 1)));
	if (seg_no + 1 == segments) {
		segment = dth->dth_heap;
	} else {
		uint32_t seg_capacity = DISPATCH_HEAP_INIT_SEGMENT_CAPACITY;
		seg_capacity <<= (segments - 2);
		segment = dth->dth_heap[seg_capacity - seg_no - 1];
	}
	if (seg_no) {
		idx -= DISPATCH_HEAP_INIT_SEGMENT_CAPACITY << (seg_no - 1);
	}
	return (dispatch_timer_source_refs_t *)(segment + idx);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_timer_heap_set(dispatch_timer_source_refs_t *slot,
		dispatch_timer_source_refs_t dt, uint32_t idx)
{
	*slot = dt;
	dt->dt_heap_entry[DTH_HEAP_ID(idx)] = idx;
}

DISPATCH_ALWAYS_INLINE
static inline uint32_t
_dispatch_timer_heap_parent(uint32_t idx)
{
	uint32_t heap_id = DTH_HEAP_ID(idx);
	idx = (idx - DTH_ID_COUNT) / 2; // go to the parent
	return DTH_IDX_FOR_HEAP_ID(idx, heap_id);
}

DISPATCH_ALWAYS_INLINE
static inline uint32_t
_dispatch_timer_heap_left_child(uint32_t idx)
{
	uint32_t heap_id = DTH_HEAP_ID(idx);
	// 2 * (idx - heap_id) + DTH_ID_COUNT + heap_id
	return 2 * idx + DTH_ID_COUNT - heap_id;
}

#if DISPATCH_HAVE_TIMER_COALESCING
DISPATCH_ALWAYS_INLINE
static inline uint32_t
_dispatch_timer_heap_walk_skip(uint32_t idx, uint32_t count)
{
	uint32_t heap_id = DTH_HEAP_ID(idx);

	idx -= heap_id;
	if (unlikely(idx + DTH_ID_COUNT == count)) {
		// reaching `count` doesn't mean we're done, but there is a weird
		// corner case if the last item of the heap is a left child:
		//
		//     /\
		//    /  \
		//   /  __\
		//  /__/
		//     ^
		//
		// The formula below would return the sibling of `idx` which is
		// out of bounds. Fortunately, the correct answer is the same
		// as for idx's parent
		idx = _dispatch_timer_heap_parent(idx);
	}

	//
	// When considering the index in a non interleaved, 1-based array
	// representation of a heap, hence looking at (idx / DTH_ID_COUNT + 1)
	// for a given idx in our dual-heaps, that index is in one of two forms:
	//
	//     (a) 1xxxx011111    or    (b) 111111111
	//         d    i    0              d       0
	//
	// The first bit set is the row of the binary tree node (0-based).
	// The following digits from most to least significant represent the path
	// to that node, where `0` is a left turn and `1` a right turn.
	//
	// For example 0b0101 (5) is a node on row 2 accessed going left then right:
	//
	// row 0          1
	//              /   .
	// row 1      2       3
	//           . \     . .
	// row 2    4   5   6   7
	//         : : : : : : : :
	//
	// Skipping a sub-tree in walk order means going to the sibling of the last
	// node reached after we turned left. If the node was of the form (a),
	// this node is 1xxxx1, which for the above example is 0b0011 (3).
	// If the node was of the form (b) then we never took a left, meaning
	// we reached the last element in traversal order.
	//

	//
	// we want to find
	// - the least significant bit set to 0 in (idx / DTH_ID_COUNT + 1)
	// - which is offset by log_2(DTH_ID_COUNT) from the position of the least
	//   significant 0 in (idx + DTH_ID_COUNT + DTH_ID_COUNT - 1)
	//   since idx is a multiple of DTH_ID_COUNT and DTH_ID_COUNT a power of 2.
	// - which in turn is the same as the position of the least significant 1 in
	//   ~(idx + DTH_ID_COUNT + DTH_ID_COUNT - 1)
	//
	dispatch_static_assert(powerof2(DTH_ID_COUNT));
	idx += DTH_ID_COUNT + DTH_ID_COUNT - 1;
	idx >>= __builtin_ctz(~idx);

	//
	// `idx` is now either:
	// - 0 if it was the (b) case above, in which case the walk is done
	// - 1xxxx0 as the position in a 0 based array representation of a non
	//   interleaved heap, so we just have to compute the interleaved index.
	//
	return likely(idx) ? DTH_ID_COUNT * idx + heap_id : UINT32_MAX;
}

DISPATCH_ALWAYS_INLINE
static inline uint32_t
_dispatch_timer_heap_walk_next(uint32_t idx, uint32_t count)
{
	//
	// Goes to the next element in heap walk order, which is the prefix ordered
	// walk of the tree.
	//
	// From a given node, the next item to return is the left child if it
	// exists, else the first right sibling we find by walking our parent chain,
	// which is exactly what _dispatch_timer_heap_walk_skip() returns.
	//
	uint32_t lchild = _dispatch_timer_heap_left_child(idx);
	if (lchild < count) {
		return lchild;
	}
	return _dispatch_timer_heap_walk_skip(idx, count);
}

DISPATCH_NOINLINE
static uint64_t
_dispatch_timer_heap_max_target_before(dispatch_timer_heap_t dth, uint64_t limit)
{
	dispatch_timer_source_refs_t dri;
	uint32_t idx = _dispatch_timer_heap_left_child(DTH_TARGET_ID);
	uint32_t count = dth->dth_count;
	uint64_t tmp, target = dth->dth_min[DTH_TARGET_ID]->dt_timer.target;

	while (idx < count) {
		dri = *_dispatch_timer_heap_get_slot(dth, idx);
		tmp = dri->dt_timer.target;
		if (tmp > limit) {
			// skip subtree since none of the targets below can be before limit
			idx = _dispatch_timer_heap_walk_skip(idx, count);
		} else {
			target = tmp;
			idx = _dispatch_timer_heap_walk_next(idx, count);
		}
	}
	return target;
}
#endif // DISPATCH_HAVE_TIMER_COALESCING

DISPATCH_NOINLINE
static void
_dispatch_timer_heap_resift(dispatch_timer_heap_t dth,
		dispatch_timer_source_refs_t dt, uint32_t idx)
{
	dispatch_static_assert(offsetof(struct dispatch_timer_source_s, target) ==
			offsetof(struct dispatch_timer_source_s, heap_key[DTH_TARGET_ID]));
	dispatch_static_assert(offsetof(struct dispatch_timer_source_s, deadline) ==
			offsetof(struct dispatch_timer_source_s, heap_key[DTH_DEADLINE_ID]));
#define dth_cmp(hid, dt1, op, dt2) \
		(((dt1)->dt_timer.heap_key)[hid] op ((dt2)->dt_timer.heap_key)[hid])

	dispatch_timer_source_refs_t *pslot, pdt;
	dispatch_timer_source_refs_t *cslot, cdt;
	dispatch_timer_source_refs_t *rslot, rdt;
	uint32_t cidx, dth_count = dth->dth_count;
	dispatch_timer_source_refs_t *slot;
	int heap_id = DTH_HEAP_ID(idx);
	bool sifted_up = false;

	// try to sift up

	slot = _dispatch_timer_heap_get_slot(dth, idx);
	while (idx >= DTH_ID_COUNT) {
		uint32_t pidx = _dispatch_timer_heap_parent(idx);
		pslot = _dispatch_timer_heap_get_slot(dth, pidx);
		pdt = *pslot;
		if (dth_cmp(heap_id, pdt, <=, dt)) {
			break;
		}
		_dispatch_timer_heap_set(slot, pdt, idx);
		slot = pslot;
		idx = pidx;
		sifted_up = true;
	}
	if (sifted_up) {
		goto done;
	}

	// try to sift down

	while ((cidx = _dispatch_timer_heap_left_child(idx)) < dth_count) {
		uint32_t ridx = cidx + DTH_ID_COUNT;
		cslot = _dispatch_timer_heap_get_slot(dth, cidx);
		cdt = *cslot;
		if (ridx < dth_count) {
			rslot = _dispatch_timer_heap_get_slot(dth, ridx);
			rdt = *rslot;
			if (dth_cmp(heap_id, cdt, >, rdt)) {
				cidx = ridx;
				cdt = rdt;
				cslot = rslot;
			}
		}
		if (dth_cmp(heap_id, dt, <=, cdt)) {
			break;
		}
		_dispatch_timer_heap_set(slot, cdt, idx);
		slot = cslot;
		idx = cidx;
	}

done:
	_dispatch_timer_heap_set(slot, dt, idx);
#undef dth_cmp
}

DISPATCH_ALWAYS_INLINE
static void
_dispatch_timer_heap_insert(dispatch_timer_heap_t dth,
		dispatch_timer_source_refs_t dt)
{
	uint32_t idx = (dth->dth_count += DTH_ID_COUNT) - DTH_ID_COUNT;

	DISPATCH_TIMER_ASSERT(dt->dt_heap_entry[DTH_TARGET_ID], ==,
			DTH_INVALID_ID, "target idx");
	DISPATCH_TIMER_ASSERT(dt->dt_heap_entry[DTH_DEADLINE_ID], ==,
			DTH_INVALID_ID, "deadline idx");

	if (idx == 0) {
		dt->dt_heap_entry[DTH_TARGET_ID] = DTH_TARGET_ID;
		dt->dt_heap_entry[DTH_DEADLINE_ID] = DTH_DEADLINE_ID;
		dth->dth_min[DTH_TARGET_ID] = dth->dth_min[DTH_DEADLINE_ID] = dt;
		return;
	}

	if (unlikely(idx + DTH_ID_COUNT >
			_dispatch_timer_heap_capacity(dth->dth_segments))) {
		_dispatch_timer_heap_grow(dth);
	}
	_dispatch_timer_heap_resift(dth, dt, idx + DTH_TARGET_ID);
	_dispatch_timer_heap_resift(dth, dt, idx + DTH_DEADLINE_ID);
}

DISPATCH_NOINLINE
static void
_dispatch_timer_heap_remove(dispatch_timer_heap_t dth,
		dispatch_timer_source_refs_t dt)
{
	uint32_t idx = (dth->dth_count -= DTH_ID_COUNT);

	DISPATCH_TIMER_ASSERT(dt->dt_heap_entry[DTH_TARGET_ID], !=,
			DTH_INVALID_ID, "target idx");
	DISPATCH_TIMER_ASSERT(dt->dt_heap_entry[DTH_DEADLINE_ID], !=,
			DTH_INVALID_ID, "deadline idx");

	if (idx == 0) {
		DISPATCH_TIMER_ASSERT(dth->dth_min[DTH_TARGET_ID], ==, dt,
				"target slot");
		DISPATCH_TIMER_ASSERT(dth->dth_min[DTH_DEADLINE_ID], ==, dt,
				"deadline slot");
		dth->dth_min[DTH_TARGET_ID] = dth->dth_min[DTH_DEADLINE_ID] = NULL;
		goto clear_heap_entry;
	}

	for (uint32_t heap_id = 0; heap_id < DTH_ID_COUNT; heap_id++) {
		dispatch_timer_source_refs_t *slot, last_dt;
		slot = _dispatch_timer_heap_get_slot(dth, idx + heap_id);
		last_dt = *slot; *slot = NULL;
		if (last_dt != dt) {
			uint32_t removed_idx = dt->dt_heap_entry[heap_id];
			_dispatch_timer_heap_resift(dth, last_dt, removed_idx);
		}
	}
	if (unlikely(idx <= _dispatch_timer_heap_capacity(dth->dth_segments - 1))) {
		_dispatch_timer_heap_shrink(dth);
	}

clear_heap_entry:
	dt->dt_heap_entry[DTH_TARGET_ID] = DTH_INVALID_ID;
	dt->dt_heap_entry[DTH_DEADLINE_ID] = DTH_INVALID_ID;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_timer_heap_update(dispatch_timer_heap_t dth,
		dispatch_timer_source_refs_t dt)
{
	DISPATCH_TIMER_ASSERT(dt->dt_heap_entry[DTH_TARGET_ID], !=,
			DTH_INVALID_ID, "target idx");
	DISPATCH_TIMER_ASSERT(dt->dt_heap_entry[DTH_DEADLINE_ID], !=,
			DTH_INVALID_ID, "deadline idx");


	_dispatch_timer_heap_resift(dth, dt, dt->dt_heap_entry[DTH_TARGET_ID]);
	_dispatch_timer_heap_resift(dth, dt, dt->dt_heap_entry[DTH_DEADLINE_ID]);
}

DISPATCH_ALWAYS_INLINE
static bool
_dispatch_timer_heap_has_new_min(dispatch_timer_heap_t dth,
		uint32_t count, uint32_t mask)
{
	dispatch_timer_source_refs_t dt;
	bool changed = false;
	uint64_t tmp;
	uint32_t tidx;

	for (tidx = 0; tidx < count; tidx++) {
		if (!(mask & (1u << tidx))) {
			continue;
		}

		dt = dth[tidx].dth_min[DTH_TARGET_ID];
		tmp = dt ? dt->dt_timer.target : UINT64_MAX;
		if (dth[tidx].dth_target != tmp) {
			dth[tidx].dth_target = tmp;
			changed = true;
		}
		dt = dth[tidx].dth_min[DTH_DEADLINE_ID];
		tmp = dt ? dt->dt_timer.deadline : UINT64_MAX;
		if (dth[tidx].dth_deadline != tmp) {
			dth[tidx].dth_deadline = tmp;
			changed = true;
		}
	}
	return changed;
}

static inline void
_dispatch_timers_unregister(dispatch_timer_source_refs_t dt)
{
	uint32_t tidx = dt->du_ident;
	dispatch_timer_heap_t heap = &_dispatch_timers_heap[tidx];

	_dispatch_timer_heap_remove(heap, dt);
	_dispatch_timers_reconfigure = true;
	_dispatch_timers_processing_mask |= 1 << tidx;
	dispatch_assert(dt->du_wlh == NULL || dt->du_wlh == DISPATCH_WLH_ANON);
	dt->du_wlh = NULL;
}

static inline void
_dispatch_timers_register(dispatch_timer_source_refs_t dt, uint32_t tidx)
{
	dispatch_timer_heap_t heap = &_dispatch_timers_heap[tidx];
	if (_dispatch_unote_registered(dt)) {
		DISPATCH_TIMER_ASSERT(dt->du_ident, ==, tidx, "tidx");
		_dispatch_timer_heap_update(heap, dt);
	} else {
		dt->du_ident = tidx;
		_dispatch_timer_heap_insert(heap, dt);
	}
	_dispatch_timers_reconfigure = true;
	_dispatch_timers_processing_mask |= 1 << tidx;
	dispatch_assert(dt->du_wlh == NULL || dt->du_wlh == DISPATCH_WLH_ANON);
	dt->du_wlh = DISPATCH_WLH_ANON;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_source_timer_tryarm(dispatch_source_t ds)
{
	dispatch_queue_flags_t oqf, nqf;
	return os_atomic_rmw_loop2o(ds, dq_atomic_flags, oqf, nqf, relaxed, {
		if (oqf & (DSF_CANCELED | DQF_RELEASED)) {
			// do not install a cancelled timer
			os_atomic_rmw_loop_give_up(break);
		}
		nqf = oqf | DSF_ARMED;
	});
}

// Updates the ordered list of timers based on next fire date for changes to ds.
// Should only be called from the context of _dispatch_mgr_q.
static void
_dispatch_timers_update(dispatch_unote_t du, uint32_t flags)
{
	dispatch_timer_source_refs_t dr = du._dt;
	dispatch_source_t ds = _dispatch_source_from_refs(dr);
	const char *verb = "updated";
	bool will_register, disarm = false;

	DISPATCH_ASSERT_ON_MANAGER_QUEUE();

	if (unlikely(dr->du_ident == DISPATCH_TIMER_IDENT_CANCELED)) {
		dispatch_assert((flags & DISPATCH_TIMERS_RETAIN_2) == 0);
		return;
	}

	// Unregister timers that are unconfigured, disabled, suspended or have
	// missed intervals. Rearm after dispatch_set_timer(), resume or source
	// invoke will reenable them
	will_register = !(flags & DISPATCH_TIMERS_UNREGISTER) &&
			dr->dt_timer.target < INT64_MAX &&
			!os_atomic_load2o(ds, ds_pending_data, relaxed) &&
			!DISPATCH_QUEUE_IS_SUSPENDED(ds) &&
			!os_atomic_load2o(dr, dt_pending_config, relaxed);
	if (likely(!_dispatch_unote_registered(dr))) {
		dispatch_assert((flags & DISPATCH_TIMERS_RETAIN_2) == 0);
		if (unlikely(!will_register || !_dispatch_source_timer_tryarm(ds))) {
			return;
		}
		verb = "armed";
	} else if (unlikely(!will_register)) {
		disarm = true;
		verb = "disarmed";
	}

	// The heap owns a +2 on dispatch sources it references
	//
	// _dispatch_timers_run2() also sometimes passes DISPATCH_TIMERS_RETAIN_2
	// when it wants to take over this +2 at the same time we are unregistering
	// the timer from the heap.
	//
	// Compute our refcount balance according to these rules, if our balance
	// would become negative we retain the source upfront, if it is positive, we
	// get rid of the extraneous refcounts after we're done touching the source.
	int refs = will_register ? -2 : 0;
	if (_dispatch_unote_registered(dr) && !(flags & DISPATCH_TIMERS_RETAIN_2)) {
		refs += 2;
	}
	if (refs < 0) {
		dispatch_assert(refs == -2);
		_dispatch_retain_2(ds);
	}

	uint32_t tidx = _dispatch_source_timer_idx(dr);
	if (unlikely(_dispatch_unote_registered(dr) &&
			(!will_register || dr->du_ident != tidx))) {
		_dispatch_timers_unregister(dr);
	}
	if (likely(will_register)) {
		_dispatch_timers_register(dr, tidx);
	}

	if (disarm) {
		_dispatch_queue_atomic_flags_clear(ds->_as_dq, DSF_ARMED);
	}
	_dispatch_debug("kevent-source[%p]: %s timer[%p]", ds, verb, dr);
	_dispatch_object_debug(ds, "%s", __func__);
	if (refs > 0) {
		dispatch_assert(refs == 2);
		_dispatch_release_2_tailcall(ds);
	}
}

#define DISPATCH_TIMER_MISSED_MARKER  1ul

DISPATCH_ALWAYS_INLINE
static inline unsigned long
_dispatch_source_timer_compute_missed(dispatch_timer_source_refs_t dt,
		uint64_t now, unsigned long prev)
{
	uint64_t missed = (now - dt->dt_timer.target) / dt->dt_timer.interval;
	if (++missed + prev > LONG_MAX) {
		missed = LONG_MAX - prev;
	}
	if (dt->dt_timer.interval < INT64_MAX) {
		uint64_t push_by = missed * dt->dt_timer.interval;
		dt->dt_timer.target += push_by;
		dt->dt_timer.deadline += push_by;
	} else {
		dt->dt_timer.target = UINT64_MAX;
		dt->dt_timer.deadline = UINT64_MAX;
	}
	prev += missed;
	return prev;
}

DISPATCH_ALWAYS_INLINE
static inline unsigned long
_dispatch_source_timer_data(dispatch_source_t ds, dispatch_unote_t du)
{
	dispatch_timer_source_refs_t dr = du._dt;
	unsigned long data, prev, clear_prev = 0;

	os_atomic_rmw_loop2o(ds, ds_pending_data, prev, clear_prev, relaxed, {
		data = prev >> 1;
		if (unlikely(prev & DISPATCH_TIMER_MISSED_MARKER)) {
			os_atomic_rmw_loop_give_up(goto handle_missed_intervals);
		}
	});
	return data;

handle_missed_intervals:
	// The timer may be in _dispatch_source_invoke2() already for other
	// reasons such as running the registration handler when ds_pending_data
	// is changed by _dispatch_timers_run2() without holding the drain lock.
	//
	// We hence need dependency ordering to pair with the release barrier
	// done by _dispatch_timers_run2() when setting the MISSED_MARKER bit.
	os_atomic_thread_fence(dependency);
	dr = os_atomic_force_dependency_on(dr, data);

	uint64_t now = _dispatch_time_now(DISPATCH_TIMER_CLOCK(dr->du_ident));
	if (now >= dr->dt_timer.target) {
		OS_COMPILER_CAN_ASSUME(dr->dt_timer.interval < INT64_MAX);
		data = _dispatch_source_timer_compute_missed(dr, now, data);
	}

	// When we see the MISSED_MARKER the manager has given up on this timer
	// and expects the handler to call "resume".
	//
	// However, it may not have reflected this into the atomic flags yet
	// so make sure _dispatch_source_invoke2() sees the timer is disarmed
	//
	// The subsequent _dispatch_source_refs_resume() will enqueue the source
	// on the manager and make the changes to `ds_timer` above visible.
	_dispatch_queue_atomic_flags_clear(ds->_as_dq, DSF_ARMED);
	os_atomic_store2o(ds, ds_pending_data, 0, relaxed);
	return data;
}

static inline void
_dispatch_timers_run2(dispatch_clock_now_cache_t nows, uint32_t tidx)
{
	dispatch_timer_source_refs_t dr;
	dispatch_source_t ds;
	uint64_t data, pending_data;
	uint64_t now = _dispatch_time_now_cached(DISPATCH_TIMER_CLOCK(tidx), nows);

	while ((dr = _dispatch_timers_heap[tidx].dth_min[DTH_TARGET_ID])) {
		DISPATCH_TIMER_ASSERT(dr->du_filter, ==, DISPATCH_EVFILT_TIMER,
				"invalid filter");
		DISPATCH_TIMER_ASSERT(dr->du_ident, ==, tidx, "tidx");
		DISPATCH_TIMER_ASSERT(dr->dt_timer.target, !=, 0, "missing target");
		ds = _dispatch_source_from_refs(dr);
		if (dr->dt_timer.target > now) {
			// Done running timers for now.
			break;
		}
		if (dr->du_fflags & DISPATCH_TIMER_AFTER) {
			_dispatch_trace_timer_fire(dr, 1, 1);
			_dispatch_source_merge_evt(dr, EV_ONESHOT, 1, 0, 0);
			_dispatch_debug("kevent-source[%p]: fired after timer[%p]", ds, dr);
			_dispatch_object_debug(ds, "%s", __func__);
			continue;
		}

		data = os_atomic_load2o(ds, ds_pending_data, relaxed);
		if (unlikely(data)) {
			// the release barrier is required to make the changes
			// to `ds_timer` visible to _dispatch_source_timer_data()
			if (os_atomic_cmpxchg2o(ds, ds_pending_data, data,
					data | DISPATCH_TIMER_MISSED_MARKER, release)) {
				_dispatch_timers_update(dr, DISPATCH_TIMERS_UNREGISTER);
				continue;
			}
		}

		data = _dispatch_source_timer_compute_missed(dr, now, 0);
		_dispatch_timers_update(dr, DISPATCH_TIMERS_RETAIN_2);
		pending_data = data << 1;
		if (!_dispatch_unote_registered(dr) && dr->dt_timer.target < INT64_MAX){
			// if we unregistered because of suspension we have to fake we
			// missed events.
			pending_data |= DISPATCH_TIMER_MISSED_MARKER;
			os_atomic_store2o(ds, ds_pending_data, pending_data, release);
		} else {
			os_atomic_store2o(ds, ds_pending_data, pending_data, relaxed);
		}
		_dispatch_trace_timer_fire(dr, data, data);
		_dispatch_debug("kevent-source[%p]: fired timer[%p]", ds, dr);
		_dispatch_object_debug(ds, "%s", __func__);
		dx_wakeup(ds, 0, DISPATCH_WAKEUP_MAKE_DIRTY | DISPATCH_WAKEUP_CONSUME_2);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_timers_run(dispatch_clock_now_cache_t nows)
{
	uint32_t tidx;
	for (tidx = 0; tidx < DISPATCH_TIMER_COUNT; tidx++) {
		if (_dispatch_timers_heap[tidx].dth_count) {
			_dispatch_timers_run2(nows, tidx);
		}
	}
}

#if DISPATCH_HAVE_TIMER_COALESCING
#define DISPATCH_KEVENT_COALESCING_WINDOW_INIT(qos, ms) \
		[DISPATCH_TIMER_QOS_##qos] = 2ull * (ms) * NSEC_PER_MSEC

static const uint64_t _dispatch_kevent_coalescing_window[] = {
	DISPATCH_KEVENT_COALESCING_WINDOW_INIT(NORMAL, 75),
#if DISPATCH_HAVE_TIMER_QOS
	DISPATCH_KEVENT_COALESCING_WINDOW_INIT(CRITICAL, 1),
	DISPATCH_KEVENT_COALESCING_WINDOW_INIT(BACKGROUND, 100),
#endif
};
#endif // DISPATCH_HAVE_TIMER_COALESCING

static inline dispatch_timer_delay_s
_dispatch_timers_get_delay(dispatch_timer_heap_t dth, dispatch_clock_t clock,
		uint32_t qos, dispatch_clock_now_cache_t nows)
{
	uint64_t target = dth->dth_target, deadline = dth->dth_deadline;
	uint64_t delta = INT64_MAX, dldelta = INT64_MAX;
	dispatch_timer_delay_s rc;

	dispatch_assert(target <= deadline);
	if (delta == 0 || target >= INT64_MAX) {
		goto done;
	}

	if (qos < DISPATCH_TIMER_QOS_COUNT && dth->dth_count > 2) {
#if DISPATCH_HAVE_TIMER_COALESCING
		// Timer pre-coalescing <rdar://problem/13222034>
		// When we have several timers with this target/deadline bracket:
		//
		//      Target        window  Deadline
		//        V           <-------V
		// t1:    [...........|.................]
		// t2:         [......|.......]
		// t3:             [..|..........]
		// t4:                | [.............]
		//                 ^
		//          Optimal Target
		//
		// Coalescing works better if the Target is delayed to "Optimal", by
		// picking the latest target that isn't too close to the deadline.
		uint64_t window = _dispatch_kevent_coalescing_window[qos];
		if (target + window < deadline) {
			uint64_t latest = deadline - window;
			target = _dispatch_timer_heap_max_target_before(dth, latest);
		}
#endif
	}

	uint64_t now = _dispatch_time_now_cached(clock, nows);
	if (target <= now) {
		delta = 0;
		dldelta = 0;
		goto done;
	}

	uint64_t tmp = target - now;
	if (clock != DISPATCH_CLOCK_WALL) {
		tmp = _dispatch_time_mach2nano(tmp);
	}
	if (tmp < delta) {
		delta = tmp;
	}

	tmp = deadline - now;
	if (clock != DISPATCH_CLOCK_WALL) {
		tmp = _dispatch_time_mach2nano(tmp);
	}
	if (tmp < dldelta) {
		dldelta = tmp;
	}

done:
	rc.delay = delta;
	rc.leeway = delta < INT64_MAX ? dldelta - delta : INT64_MAX;
	return rc;
}

static bool
_dispatch_timers_program2(dispatch_clock_now_cache_t nows, uint32_t tidx)
{
	uint32_t qos = DISPATCH_TIMER_QOS(tidx);
	dispatch_clock_t clock = DISPATCH_TIMER_CLOCK(tidx);
	dispatch_timer_heap_t heap = &_dispatch_timers_heap[tidx];
	dispatch_timer_delay_s range;

	range = _dispatch_timers_get_delay(heap, clock, qos, nows);
	if (range.delay == 0 || range.delay >= INT64_MAX) {
		_dispatch_trace_next_timer_set(NULL, qos);
		if (heap->dth_flags & DTH_ARMED) {
			_dispatch_event_loop_timer_delete(tidx);
		}
		return range.delay == 0;
	}

	_dispatch_trace_next_timer_set(heap->dth_min[DTH_TARGET_ID], qos);
	_dispatch_trace_next_timer_program(range.delay, qos);
	_dispatch_event_loop_timer_arm(tidx, range, nows);
	return false;
}

DISPATCH_NOINLINE
static bool
_dispatch_timers_program(dispatch_clock_now_cache_t nows)
{
	bool poll = false;
	uint32_t tidx, timerm = _dispatch_timers_processing_mask;

	for (tidx = 0; tidx < DISPATCH_TIMER_COUNT; tidx++) {
		if (timerm & (1 << tidx)) {
			poll |= _dispatch_timers_program2(nows, tidx);
		}
	}
	return poll;
}

DISPATCH_NOINLINE
static bool
_dispatch_timers_configure(void)
{
	// Find out if there is a new target/deadline on the timer lists
	return _dispatch_timer_heap_has_new_min(_dispatch_timers_heap,
			countof(_dispatch_timers_heap), _dispatch_timers_processing_mask);
}

static inline bool
_dispatch_mgr_timers(void)
{
	dispatch_clock_now_cache_s nows = { };
	bool expired = _dispatch_timers_expired;
	if (unlikely(expired)) {
		_dispatch_timers_run(&nows);
	}
	_dispatch_mgr_trace_timers_wakes();
	bool reconfigure = _dispatch_timers_reconfigure;
	if (unlikely(reconfigure || expired)) {
		if (reconfigure) {
			reconfigure = _dispatch_timers_configure();
			_dispatch_timers_reconfigure = false;
		}
		if (reconfigure || expired) {
			expired = _dispatch_timers_expired = _dispatch_timers_program(&nows);
		}
		_dispatch_timers_processing_mask = 0;
	}
	return expired;
}

#pragma mark -
#pragma mark dispatch_mgr

void
_dispatch_mgr_queue_push(dispatch_queue_t dq, dispatch_object_t dou,
		DISPATCH_UNUSED dispatch_qos_t qos)
{
	uint64_t dq_state;
	_dispatch_trace_continuation_push(dq, dou._do);
	if (unlikely(_dispatch_queue_push_update_tail(dq, dou._do))) {
		_dispatch_queue_push_update_head(dq, dou._do);
		dq_state = os_atomic_or2o(dq, dq_state, DISPATCH_QUEUE_DIRTY, release);
		if (!_dq_state_drain_locked_by_self(dq_state)) {
			_dispatch_event_loop_poke(DISPATCH_WLH_MANAGER, 0, 0);
		}
	}
}

DISPATCH_NORETURN
void
_dispatch_mgr_queue_wakeup(DISPATCH_UNUSED dispatch_queue_t dq,
		DISPATCH_UNUSED dispatch_qos_t qos,
		DISPATCH_UNUSED dispatch_wakeup_flags_t flags)
{
	DISPATCH_INTERNAL_CRASH(0, "Don't try to wake up or override the manager");
}

#if DISPATCH_USE_MGR_THREAD
DISPATCH_NOINLINE DISPATCH_NORETURN
static void
_dispatch_mgr_invoke(void)
{
#if DISPATCH_EVENT_BACKEND_KEVENT
	dispatch_kevent_s evbuf[DISPATCH_DEFERRED_ITEMS_EVENT_COUNT];
#endif
	dispatch_deferred_items_s ddi = {
#if DISPATCH_EVENT_BACKEND_KEVENT
		.ddi_maxevents = DISPATCH_DEFERRED_ITEMS_EVENT_COUNT,
		.ddi_eventlist = evbuf,
#endif
	};
	bool poll;

	_dispatch_deferred_items_set(&ddi);
	for (;;) {
		_dispatch_mgr_queue_drain();
		poll = _dispatch_mgr_timers();
		poll = poll || _dispatch_queue_class_probe(&_dispatch_mgr_q);
		_dispatch_event_loop_drain(poll ? KEVENT_FLAG_IMMEDIATE : 0);
	}
}
#endif // DISPATCH_USE_MGR_THREAD

DISPATCH_NORETURN
void
_dispatch_mgr_thread(dispatch_queue_t dq DISPATCH_UNUSED,
		dispatch_invoke_context_t dic DISPATCH_UNUSED,
		dispatch_invoke_flags_t flags DISPATCH_UNUSED)
{
#if DISPATCH_USE_KEVENT_WORKQUEUE
	if (_dispatch_kevent_workqueue_enabled) {
		DISPATCH_INTERNAL_CRASH(0, "Manager queue invoked with "
				"kevent workqueue enabled");
	}
#endif
#if DISPATCH_USE_MGR_THREAD
	_dispatch_queue_set_current(&_dispatch_mgr_q);
	_dispatch_mgr_priority_init();
	_dispatch_queue_mgr_lock(&_dispatch_mgr_q);
	// never returns, so burn bridges behind us & clear stack 2k ahead
	_dispatch_clear_stack(2048);
	_dispatch_mgr_invoke();
#endif
}

#if DISPATCH_USE_KEVENT_WORKQUEUE

#define DISPATCH_KEVENT_WORKER_IS_NOT_MANAGER ((dispatch_priority_t)~0u)

_Static_assert(WORKQ_KEVENT_EVENT_BUFFER_LEN >=
		DISPATCH_DEFERRED_ITEMS_EVENT_COUNT,
		"our list should not be longer than the kernel's");

DISPATCH_ALWAYS_INLINE
static inline dispatch_priority_t
_dispatch_wlh_worker_thread_init(dispatch_wlh_t wlh,
		dispatch_deferred_items_t ddi)
{
	dispatch_assert(wlh);
	dispatch_priority_t old_dbp;

	pthread_priority_t pp = _dispatch_get_priority();
	if (!(pp & _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG)) {
		// If this thread does not have the event manager flag set, don't setup
		// as the dispatch manager and let the caller know to only process
		// the delivered events.
		//
		// Also add the NEEDS_UNBIND flag so that
		// _dispatch_priority_compute_update knows it has to unbind
		pp &= _PTHREAD_PRIORITY_OVERCOMMIT_FLAG | ~_PTHREAD_PRIORITY_FLAGS_MASK;
		if (wlh == DISPATCH_WLH_ANON) {
			pp |= _PTHREAD_PRIORITY_NEEDS_UNBIND_FLAG;
		} else {
			// pthread sets the flag when it is an event delivery thread
			// so we need to explicitly clear it
			pp &= ~(pthread_priority_t)_PTHREAD_PRIORITY_NEEDS_UNBIND_FLAG;
		}
		_dispatch_thread_setspecific(dispatch_priority_key,
				(void *)(uintptr_t)pp);
		if (wlh != DISPATCH_WLH_ANON) {
			_dispatch_debug("wlh[%p]: handling events", wlh);
		} else {
			ddi->ddi_can_stash = true;
		}
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
	old_dbp = _dispatch_set_basepri(DISPATCH_PRIORITY_FLAG_MANAGER);
	_dispatch_queue_set_current(&_dispatch_mgr_q);
	_dispatch_queue_mgr_lock(&_dispatch_mgr_q);
	return old_dbp;
}

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline bool
_dispatch_wlh_worker_thread_reset(dispatch_priority_t old_dbp)
{
	bool needs_poll = _dispatch_queue_mgr_unlock(&_dispatch_mgr_q);
	_dispatch_reset_basepri(old_dbp);
	_dispatch_reset_basepri_override();
	_dispatch_queue_set_current(NULL);
	return needs_poll;
}

DISPATCH_ALWAYS_INLINE
static void
_dispatch_wlh_worker_thread(dispatch_wlh_t wlh, dispatch_kevent_t events,
		int *nevents)
{
	_dispatch_introspection_thread_add();
	DISPATCH_PERF_MON_VAR_INIT

	dispatch_deferred_items_s ddi = {
		.ddi_eventlist = events,
	};
	dispatch_priority_t old_dbp;

	old_dbp = _dispatch_wlh_worker_thread_init(wlh, &ddi);
	if (old_dbp == DISPATCH_KEVENT_WORKER_IS_NOT_MANAGER) {
		_dispatch_perfmon_start_impl(true);
	} else {
		dispatch_assert(wlh == DISPATCH_WLH_ANON);
		wlh = DISPATCH_WLH_ANON;
	}
	_dispatch_deferred_items_set(&ddi);
	_dispatch_event_loop_merge(events, *nevents);

	if (old_dbp != DISPATCH_KEVENT_WORKER_IS_NOT_MANAGER) {
		_dispatch_mgr_queue_drain();
		bool poll = _dispatch_mgr_timers();
		if (_dispatch_wlh_worker_thread_reset(old_dbp)) {
			poll = true;
		}
		if (poll) _dispatch_event_loop_poke(DISPATCH_WLH_MANAGER, 0, 0);
	} else if (ddi.ddi_stashed_dou._do) {
		_dispatch_debug("wlh[%p]: draining deferred item %p", wlh,
				ddi.ddi_stashed_dou._do);
		if (wlh == DISPATCH_WLH_ANON) {
			dispatch_assert(ddi.ddi_nevents == 0);
			_dispatch_deferred_items_set(NULL);
			_dispatch_root_queue_drain_deferred_item(&ddi
					DISPATCH_PERF_MON_ARGS);
		} else {
			_dispatch_root_queue_drain_deferred_wlh(&ddi
					DISPATCH_PERF_MON_ARGS);
		}
	}

	_dispatch_deferred_items_set(NULL);
	if (old_dbp == DISPATCH_KEVENT_WORKER_IS_NOT_MANAGER &&
			!ddi.ddi_stashed_dou._do) {
		_dispatch_perfmon_end(perfmon_thread_event_no_steal);
	}
	_dispatch_debug("returning %d deferred kevents", ddi.ddi_nevents);
	*nevents = ddi.ddi_nevents;
}

DISPATCH_NOINLINE
void
_dispatch_kevent_worker_thread(dispatch_kevent_t *events, int *nevents)
{
	if (!events && !nevents) {
		// events for worker thread request have already been delivered earlier
		return;
	}
	if (!dispatch_assume(*nevents && *events)) return;
	_dispatch_adopt_wlh_anon();
	_dispatch_wlh_worker_thread(DISPATCH_WLH_ANON, *events, nevents);
	_dispatch_reset_wlh();
}


#endif // DISPATCH_USE_KEVENT_WORKQUEUE
#pragma mark -
#pragma mark dispatch_source_debug

static size_t
_dispatch_source_debug_attr(dispatch_source_t ds, char* buf, size_t bufsiz)
{
	dispatch_queue_t target = ds->do_targetq;
	dispatch_source_refs_t dr = ds->ds_refs;
	return dsnprintf(buf, bufsiz, "target = %s[%p], ident = 0x%x, "
			"mask = 0x%x, pending_data = 0x%llx, registered = %d, "
			"armed = %d, deleted = %d%s, canceled = %d, ",
			target && target->dq_label ? target->dq_label : "", target,
			dr->du_ident, dr->du_fflags, (unsigned long long)ds->ds_pending_data,
			ds->ds_is_installed, (bool)(ds->dq_atomic_flags & DSF_ARMED),
			(bool)(ds->dq_atomic_flags & DSF_DELETED),
			(ds->dq_atomic_flags & DSF_DEFERRED_DELETE) ? " (pending)" : "",
			(bool)(ds->dq_atomic_flags & DSF_CANCELED));
}

static size_t
_dispatch_timer_debug_attr(dispatch_source_t ds, char* buf, size_t bufsiz)
{
	dispatch_timer_source_refs_t dr = ds->ds_timer_refs;
	return dsnprintf(buf, bufsiz, "timer = { target = 0x%llx, deadline = 0x%llx"
			", interval = 0x%llx, flags = 0x%x }, ",
			(unsigned long long)dr->dt_timer.target,
			(unsigned long long)dr->dt_timer.deadline,
			(unsigned long long)dr->dt_timer.interval, dr->du_fflags);
}

size_t
_dispatch_source_debug(dispatch_source_t ds, char *buf, size_t bufsiz)
{
	dispatch_source_refs_t dr = ds->ds_refs;
	size_t offset = 0;
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			dx_kind(ds), ds);
	offset += _dispatch_object_debug_attr(ds, &buf[offset], bufsiz - offset);
	offset += _dispatch_source_debug_attr(ds, &buf[offset], bufsiz - offset);
	if (dr->du_is_timer) {
		offset += _dispatch_timer_debug_attr(ds, &buf[offset], bufsiz - offset);
	}
	offset += dsnprintf(&buf[offset], bufsiz - offset, "kevent = %p%s, "
			"filter = %s }", dr,  dr->du_is_direct ? " (direct)" : "",
			dr->du_type->dst_kind);
	return offset;
}
