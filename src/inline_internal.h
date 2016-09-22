/*
 * Copyright (c) 2008-2013 Apple Inc. All rights reserved.
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

/*
 * IMPORTANT: This header file describes INTERNAL interfaces to libdispatch
 * which are subject to change in future releases of Mac OS X. Any applications
 * relying on these interfaces WILL break.
 */

#ifndef __DISPATCH_INLINE_INTERNAL__
#define __DISPATCH_INLINE_INTERNAL__

#ifndef __DISPATCH_INDIRECT__
#error "Please #include <dispatch/dispatch.h> instead of this file directly."
#include <dispatch/base.h> // for HeaderDoc
#endif

#if DISPATCH_USE_CLIENT_CALLOUT

DISPATCH_NOTHROW void
_dispatch_client_callout(void *ctxt, dispatch_function_t f);
DISPATCH_NOTHROW void
_dispatch_client_callout2(void *ctxt, size_t i, void (*f)(void *, size_t));
#if HAVE_MACH
DISPATCH_NOTHROW void
_dispatch_client_callout4(void *ctxt, dispatch_mach_reason_t reason,
		dispatch_mach_msg_t dmsg, mach_error_t error,
		dispatch_mach_handler_function_t f);
#endif // HAVE_MACH

#else // !DISPATCH_USE_CLIENT_CALLOUT

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_client_callout(void *ctxt, dispatch_function_t f)
{
	return f(ctxt);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_client_callout2(void *ctxt, size_t i, void (*f)(void *, size_t))
{
	return f(ctxt, i);
}

#if HAVE_MACH
DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_client_callout4(void *ctxt, dispatch_mach_reason_t reason,
		dispatch_mach_msg_t dmsg, mach_error_t error,
		dispatch_mach_handler_function_t f)
{
	return f(ctxt, reason, dmsg, error);
}
#endif // HAVE_MACH

#endif // !DISPATCH_USE_CLIENT_CALLOUT

#pragma mark -
#pragma mark _os_object_t & dispatch_object_t
#if DISPATCH_PURE_C

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_object_has_vtable(dispatch_object_t dou)
{
	uintptr_t dc_flags = dou._dc->dc_flags;

	// vtables are pointers far away from the low page in memory
	return dc_flags > 0xffful;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_object_is_continuation(dispatch_object_t dou)
{
	if (_dispatch_object_has_vtable(dou)) {
		return dx_metatype(dou._do) == _DISPATCH_CONTINUATION_TYPE;
	}
	return true;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_object_has_type(dispatch_object_t dou, unsigned long type)
{
	return _dispatch_object_has_vtable(dou) && dx_type(dou._do) == type;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_object_is_redirection(dispatch_object_t dou)
{
	return _dispatch_object_has_type(dou,
			DISPATCH_CONTINUATION_TYPE(ASYNC_REDIRECT));
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_object_is_barrier(dispatch_object_t dou)
{
	dispatch_queue_flags_t dq_flags;

	if (!_dispatch_object_has_vtable(dou)) {
		return (dou._dc->dc_flags & DISPATCH_OBJ_BARRIER_BIT);
	}
	switch (dx_metatype(dou._do)) {
	case _DISPATCH_QUEUE_TYPE:
	case _DISPATCH_SOURCE_TYPE:
		dq_flags = os_atomic_load2o(dou._dq, dq_atomic_flags, relaxed);
		return dq_flags & DQF_BARRIER_BIT;
	default:
		return false;
	}
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_object_is_slow_item(dispatch_object_t dou)
{
	if (_dispatch_object_has_vtable(dou)) {
		return false;
	}
	return (dou._dc->dc_flags & DISPATCH_OBJ_SYNC_SLOW_BIT);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_object_is_slow_non_barrier(dispatch_object_t dou)
{
	if (_dispatch_object_has_vtable(dou)) {
		return false;
	}
	return ((dou._dc->dc_flags &
				(DISPATCH_OBJ_BARRIER_BIT | DISPATCH_OBJ_SYNC_SLOW_BIT)) ==
				(DISPATCH_OBJ_SYNC_SLOW_BIT));
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_object_is_slow_barrier(dispatch_object_t dou)
{
	if (_dispatch_object_has_vtable(dou)) {
		return false;
	}
	return ((dou._dc->dc_flags &
				(DISPATCH_OBJ_BARRIER_BIT | DISPATCH_OBJ_SYNC_SLOW_BIT)) ==
				(DISPATCH_OBJ_BARRIER_BIT | DISPATCH_OBJ_SYNC_SLOW_BIT));
}

DISPATCH_ALWAYS_INLINE
static inline _os_object_t
_os_object_retain_internal_inline(_os_object_t obj)
{
	int ref_cnt = _os_object_refcnt_inc(obj);
	if (unlikely(ref_cnt <= 0)) {
		_OS_OBJECT_CLIENT_CRASH("Resurrection of an object");
	}
	return obj;
}

DISPATCH_ALWAYS_INLINE
static inline void
_os_object_release_internal_inline_no_dispose(_os_object_t obj)
{
	int ref_cnt = _os_object_refcnt_dec(obj);
	if (likely(ref_cnt >= 0)) {
		return;
	}
	if (ref_cnt == 0) {
		_OS_OBJECT_CLIENT_CRASH("Unexpected release of an object");
	}
	_OS_OBJECT_CLIENT_CRASH("Over-release of an object");
}

DISPATCH_ALWAYS_INLINE
static inline void
_os_object_release_internal_inline(_os_object_t obj)
{
	int ref_cnt = _os_object_refcnt_dec(obj);
	if (likely(ref_cnt >= 0)) {
		return;
	}
	if (unlikely(ref_cnt < -1)) {
		_OS_OBJECT_CLIENT_CRASH("Over-release of an object");
	}
#if DISPATCH_DEBUG
	int xref_cnt = obj->os_obj_xref_cnt;
	if (unlikely(xref_cnt >= 0)) {
		DISPATCH_INTERNAL_CRASH(xref_cnt,
				"Release while external references exist");
	}
#endif
	// _os_object_refcnt_dispose_barrier() is in _os_object_dispose()
	return _os_object_dispose(obj);
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_retain(dispatch_object_t dou)
{
	(void)_os_object_retain_internal_inline(dou._os_obj);
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_release(dispatch_object_t dou)
{
	_os_object_release_internal_inline(dou._os_obj);
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_release_tailcall(dispatch_object_t dou)
{
	_os_object_release_internal(dou._os_obj);
}

DISPATCH_ALWAYS_INLINE DISPATCH_NONNULL_ALL
static inline void
_dispatch_object_set_target_queue_inline(dispatch_object_t dou,
		dispatch_queue_t tq)
{
	_dispatch_retain(tq);
	tq = os_atomic_xchg2o(dou._do, do_targetq, tq, release);
	if (tq) _dispatch_release(tq);
	_dispatch_object_debug(dou._do, "%s", __func__);
}

#endif // DISPATCH_PURE_C
#pragma mark -
#pragma mark dispatch_thread
#if DISPATCH_PURE_C

#define DISPATCH_DEFERRED_ITEMS_MAGIC  0xdefe55edul /* deferred */
#define DISPATCH_DEFERRED_ITEMS_EVENT_COUNT 8
#ifdef WORKQ_KEVENT_EVENT_BUFFER_LEN
_Static_assert(WORKQ_KEVENT_EVENT_BUFFER_LEN >=
		DISPATCH_DEFERRED_ITEMS_EVENT_COUNT,
		"our list should not be longer than the kernel's");
#endif

typedef struct dispatch_deferred_items_s {
	uint32_t ddi_magic;
	dispatch_queue_t ddi_stashed_dq;
	struct dispatch_object_s *ddi_stashed_dou;
	dispatch_priority_t ddi_stashed_pp;
	int ddi_nevents;
	int ddi_maxevents;
	_dispatch_kevent_qos_s ddi_eventlist[DISPATCH_DEFERRED_ITEMS_EVENT_COUNT];
} dispatch_deferred_items_s, *dispatch_deferred_items_t;

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_deferred_items_set(dispatch_deferred_items_t ddi)
{
	_dispatch_thread_setspecific(dispatch_deferred_items_key, (void *)ddi);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_deferred_items_t
_dispatch_deferred_items_get(void)
{
	dispatch_deferred_items_t ddi = (dispatch_deferred_items_t)
			_dispatch_thread_getspecific(dispatch_deferred_items_key);
	if (ddi && ddi->ddi_magic == DISPATCH_DEFERRED_ITEMS_MAGIC) {
		return ddi;
	}
	return NULL;
}

#endif // DISPATCH_PURE_C
#pragma mark -
#pragma mark dispatch_thread
#if DISPATCH_PURE_C

DISPATCH_ALWAYS_INLINE
static inline dispatch_thread_context_t
_dispatch_thread_context_find(const void *key)
{
	dispatch_thread_context_t dtc =
			_dispatch_thread_getspecific(dispatch_context_key);
	while (dtc) {
		if (dtc->dtc_key == key) {
			return dtc;
		}
		dtc = dtc->dtc_prev;
	}
	return NULL;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_context_push(dispatch_thread_context_t ctxt)
{
	ctxt->dtc_prev = _dispatch_thread_getspecific(dispatch_context_key);
	_dispatch_thread_setspecific(dispatch_context_key, ctxt);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_context_pop(dispatch_thread_context_t ctxt)
{
	dispatch_assert(_dispatch_thread_getspecific(dispatch_context_key) == ctxt);
	_dispatch_thread_setspecific(dispatch_context_key, ctxt->dtc_prev);
}

typedef struct dispatch_thread_frame_iterator_s {
	dispatch_queue_t dtfi_queue;
	dispatch_thread_frame_t dtfi_frame;
} *dispatch_thread_frame_iterator_t;

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_frame_iterate_start(dispatch_thread_frame_iterator_t it)
{
	_dispatch_thread_getspecific_pair(
			dispatch_queue_key, (void **)&it->dtfi_queue,
			dispatch_frame_key, (void **)&it->dtfi_frame);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_frame_iterate_next(dispatch_thread_frame_iterator_t it)
{
	dispatch_thread_frame_t dtf = it->dtfi_frame;
	dispatch_queue_t dq = it->dtfi_queue;

	if (dtf) {
		if (dq->do_targetq) {
			// redirections and trysync_f may skip some frames,
			// so we need to simulate seeing the missing links
			// however the bottom root queue is always present
			it->dtfi_queue = dq->do_targetq;
			if (it->dtfi_queue == dtf->dtf_queue) {
				it->dtfi_frame = dtf->dtf_prev;
			}
		} else {
			it->dtfi_queue = dtf->dtf_queue;
			it->dtfi_frame = dtf->dtf_prev;
		}
	} else if (dq) {
		it->dtfi_queue = dq->do_targetq;
	}
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_thread_frame_find_queue(dispatch_queue_t dq)
{
	struct dispatch_thread_frame_iterator_s it;

	_dispatch_thread_frame_iterate_start(&it);
	while (it.dtfi_queue) {
		if (it.dtfi_queue == dq) {
			return true;
		}
		_dispatch_thread_frame_iterate_next(&it);
	}
	return false;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_thread_frame_t
_dispatch_thread_frame_get_current(void)
{
	return _dispatch_thread_getspecific(dispatch_frame_key);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_frame_set_current(dispatch_thread_frame_t dtf)
{
	_dispatch_thread_setspecific(dispatch_frame_key, dtf);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_frame_save_state(dispatch_thread_frame_t dtf)
{
	_dispatch_thread_getspecific_packed_pair(
			dispatch_queue_key, dispatch_frame_key, (void **)&dtf->dtf_queue);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_frame_push(dispatch_thread_frame_t dtf, dispatch_queue_t dq)
{
	_dispatch_thread_frame_save_state(dtf);
	_dispatch_thread_setspecific_pair(dispatch_queue_key, dq,
			dispatch_frame_key, dtf);
	dtf->dtf_deferred = NULL;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_frame_push_and_rebase(dispatch_thread_frame_t dtf,
		dispatch_queue_t dq, dispatch_thread_frame_t new_base)
{
	_dispatch_thread_frame_save_state(dtf);
	_dispatch_thread_setspecific_pair(dispatch_queue_key, dq,
			dispatch_frame_key, new_base);
	dtf->dtf_deferred = NULL;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_frame_pop(dispatch_thread_frame_t dtf)
{
	_dispatch_thread_setspecific_packed_pair(
			dispatch_queue_key, dispatch_frame_key, (void **)&dtf->dtf_queue);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_t
_dispatch_thread_frame_stash(dispatch_thread_frame_t dtf)
{
	_dispatch_thread_getspecific_pair(
			dispatch_queue_key, (void **)&dtf->dtf_queue,
			dispatch_frame_key, (void **)&dtf->dtf_prev);
	_dispatch_thread_frame_pop(dtf->dtf_prev);
	return dtf->dtf_queue;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_frame_unstash(dispatch_thread_frame_t dtf)
{
	_dispatch_thread_frame_pop(dtf);
}

DISPATCH_ALWAYS_INLINE
static inline int
_dispatch_wqthread_override_start_check_owner(mach_port_t thread,
		pthread_priority_t pp, mach_port_t *ulock_addr)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	if (!_dispatch_set_qos_class_enabled) return 0;
	return _pthread_workqueue_override_start_direct_check_owner(thread,
			pp, ulock_addr);
#else
	(void)thread; (void)pp; (void)ulock_addr;
	return 0;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_wqthread_override_start(mach_port_t thread,
		pthread_priority_t pp)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	if (!_dispatch_set_qos_class_enabled) return;
	(void)_pthread_workqueue_override_start_direct(thread, pp);
#else
	(void)thread; (void)pp;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_wqthread_override_reset(void)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	if (!_dispatch_set_qos_class_enabled) return;
	(void)_pthread_workqueue_override_reset();
#endif
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_override_start(mach_port_t thread, pthread_priority_t pp,
		void *resource)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	if (!_dispatch_set_qos_class_enabled) return;
	(void)_pthread_qos_override_start_direct(thread, pp, resource);
#else
	(void)thread; (void)pp; (void)resource;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_override_end(mach_port_t thread, void *resource)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	if (!_dispatch_set_qos_class_enabled) return;
	(void)_pthread_qos_override_end_direct(thread, resource);
#else
	(void)thread; (void)resource;
#endif
}

#if DISPATCH_DEBUG_QOS && HAVE_PTHREAD_WORKQUEUE_QOS
DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_qos_class_is_valid(pthread_priority_t pp)
{
	pp &= _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	if (pp > (1UL << (DISPATCH_QUEUE_QOS_COUNT +
			_PTHREAD_PRIORITY_QOS_CLASS_SHIFT))) {
		return false;
	}
	return true;
}
#define _dispatch_assert_is_valid_qos_class(pp)  ({ typeof(pp) _pp = (pp); \
		if (unlikely(!_dispatch_qos_class_is_valid(_pp))) { \
			DISPATCH_INTERNAL_CRASH(_pp, "Invalid qos class"); \
		} \
	})

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_qos_override_is_valid(pthread_priority_t pp)
{
	if (pp & (pthread_priority_t)_PTHREAD_PRIORITY_FLAGS_MASK) {
		return false;
	}
	return _dispatch_qos_class_is_valid(pp);
}
#define _dispatch_assert_is_valid_qos_override(pp)  ({ typeof(pp) _pp = (pp); \
		if (unlikely(!_dispatch_qos_override_is_valid(_pp))) { \
			DISPATCH_INTERNAL_CRASH(_pp, "Invalid override"); \
		} \
	})
#else
#define _dispatch_assert_is_valid_qos_override(pp) (void)(pp)
#define _dispatch_assert_is_valid_qos_class(pp) (void)(pp)
#endif

#endif // DISPATCH_PURE_C
#pragma mark -
#pragma mark dispatch_queue_t state accessors
#if DISPATCH_PURE_C

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_flags_t
_dispatch_queue_atomic_flags(dispatch_queue_t dq)
{
	return os_atomic_load2o(dq, dq_atomic_flags, relaxed);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_flags_t
_dispatch_queue_atomic_flags_set(dispatch_queue_t dq,
		dispatch_queue_flags_t bits)
{
	return os_atomic_or2o(dq, dq_atomic_flags, bits, relaxed);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_flags_t
_dispatch_queue_atomic_flags_set_and_clear_orig(dispatch_queue_t dq,
		dispatch_queue_flags_t add_bits, dispatch_queue_flags_t clr_bits)
{
	dispatch_queue_flags_t oflags, nflags;
	os_atomic_rmw_loop2o(dq, dq_atomic_flags, oflags, nflags, relaxed, {
		nflags = (oflags | add_bits) & ~clr_bits;
	});
	return oflags;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_flags_t
_dispatch_queue_atomic_flags_set_and_clear(dispatch_queue_t dq,
		dispatch_queue_flags_t add_bits, dispatch_queue_flags_t clr_bits)
{
	dispatch_queue_flags_t oflags, nflags;
	os_atomic_rmw_loop2o(dq, dq_atomic_flags, oflags, nflags, relaxed, {
		nflags = (oflags | add_bits) & ~clr_bits;
	});
	return nflags;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_flags_t
_dispatch_queue_atomic_flags_set_orig(dispatch_queue_t dq,
		dispatch_queue_flags_t bits)
{
	return os_atomic_or_orig2o(dq, dq_atomic_flags, bits, relaxed);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_flags_t
_dispatch_queue_atomic_flags_clear(dispatch_queue_t dq,
		dispatch_queue_flags_t bits)
{
	return os_atomic_and2o(dq, dq_atomic_flags, ~bits, relaxed);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_is_thread_bound(dispatch_queue_t dq)
{
	return _dispatch_queue_atomic_flags(dq) & DQF_THREAD_BOUND;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_cannot_trysync(dispatch_queue_t dq)
{
	return _dispatch_queue_atomic_flags(dq) & DQF_CANNOT_TRYSYNC;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_label_needs_free(dispatch_queue_t dq)
{
	return _dispatch_queue_atomic_flags(dq) & DQF_LABEL_NEEDS_FREE;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_invoke_flags_t
_dispatch_queue_autorelease_frequency(dispatch_queue_t dq)
{
	const unsigned long factor =
			DISPATCH_INVOKE_AUTORELEASE_ALWAYS / DQF_AUTORELEASE_ALWAYS;
	dispatch_static_assert(factor > 0);

	dispatch_queue_flags_t qaf = _dispatch_queue_atomic_flags(dq);

	qaf &= _DQF_AUTORELEASE_MASK;
	return (dispatch_invoke_flags_t)qaf * factor;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_invoke_flags_t
_dispatch_queue_merge_autorelease_frequency(dispatch_queue_t dq,
		dispatch_invoke_flags_t flags)
{
	dispatch_invoke_flags_t qaf = _dispatch_queue_autorelease_frequency(dq);

	if (qaf) {
		flags &= ~_DISPATCH_INVOKE_AUTORELEASE_MASK;
		flags |= qaf;
	}
	return flags;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_has_immutable_target(dispatch_queue_t dq)
{
	if (dx_metatype(dq) != _DISPATCH_QUEUE_TYPE) {
		return false;
	}
	return dx_type(dq) != DISPATCH_QUEUE_LEGACY_TYPE;
}

#endif // DISPATCH_PURE_C
#ifndef __cplusplus

DISPATCH_ALWAYS_INLINE
static inline uint32_t
_dq_state_suspend_cnt(uint64_t dq_state)
{
	return (uint32_t)(dq_state / DISPATCH_QUEUE_SUSPEND_INTERVAL);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_has_side_suspend_cnt(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_HAS_SIDE_SUSPEND_CNT;
}

DISPATCH_ALWAYS_INLINE
static inline uint32_t
_dq_state_extract_width_bits(uint64_t dq_state)
{
	dq_state &= DISPATCH_QUEUE_WIDTH_MASK;
	return (uint32_t)(dq_state >> DISPATCH_QUEUE_WIDTH_SHIFT);
}

DISPATCH_ALWAYS_INLINE
static inline uint32_t
_dq_state_available_width(uint64_t dq_state)
{
	uint32_t full = DISPATCH_QUEUE_WIDTH_FULL;
	if (fastpath(!(dq_state & DISPATCH_QUEUE_WIDTH_FULL_BIT))) {
		return full - _dq_state_extract_width_bits(dq_state);
	}
	return 0;
}

DISPATCH_ALWAYS_INLINE
static inline uint32_t
_dq_state_used_width(uint64_t dq_state, uint16_t dq_width)
{
	uint32_t full = DISPATCH_QUEUE_WIDTH_FULL;
	uint32_t width = _dq_state_extract_width_bits(dq_state);

	if (dq_state & DISPATCH_QUEUE_PENDING_BARRIER) {
		// DISPATCH_QUEUE_PENDING_BARRIER means (dq_width - 1) of the used width
		// is pre-reservation that we want to ignore
		return width - (full - dq_width) - (dq_width - 1);
	}
	return width - (full - dq_width);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_is_suspended(uint64_t dq_state)
{
	return dq_state >= DISPATCH_QUEUE_NEEDS_ACTIVATION;
}
#define DISPATCH_QUEUE_IS_SUSPENDED(x)  _dq_state_is_suspended((x)->dq_state)

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_is_inactive(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_INACTIVE;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_needs_activation(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_NEEDS_ACTIVATION;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_is_in_barrier(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_IN_BARRIER;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_has_available_width(uint64_t dq_state)
{
	return !(dq_state & DISPATCH_QUEUE_WIDTH_FULL_BIT);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_has_pending_barrier(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_PENDING_BARRIER;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_is_dirty(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_DIRTY;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_is_enqueued(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_ENQUEUED;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_has_override(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_HAS_OVERRIDE;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_lock_owner
_dq_state_drain_owner(uint64_t dq_state)
{
	return _dispatch_lock_owner((dispatch_lock)dq_state);
}
#define DISPATCH_QUEUE_DRAIN_OWNER(dq) \
	_dq_state_drain_owner(os_atomic_load2o(dq, dq_state, relaxed))

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_drain_pended(uint64_t dq_state)
{
	return (dq_state & DISPATCH_QUEUE_DRAIN_PENDED);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_drain_locked_by(uint64_t dq_state, uint32_t owner)
{
	if (_dq_state_drain_pended(dq_state)) {
		return false;
	}
	return _dq_state_drain_owner(dq_state) == owner;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_drain_locked(uint64_t dq_state)
{
	return (dq_state & DISPATCH_QUEUE_DRAIN_OWNER_MASK) != 0;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_has_waiters(uint64_t dq_state)
{
	return _dispatch_lock_has_waiters((dispatch_lock)dq_state);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_is_sync_runnable(uint64_t dq_state)
{
	return dq_state < DISPATCH_QUEUE_IN_BARRIER;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_is_runnable(uint64_t dq_state)
{
	return dq_state < DISPATCH_QUEUE_WIDTH_FULL_BIT;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_should_wakeup(uint64_t dq_state)
{
	return _dq_state_is_runnable(dq_state) &&
			!_dq_state_is_enqueued(dq_state) &&
			!_dq_state_drain_locked(dq_state);
}

#endif // __cplusplus
#pragma mark -
#pragma mark dispatch_queue_t state machine
#ifndef __cplusplus

static inline bool _dispatch_queue_need_override(dispatch_queue_class_t dqu,
		pthread_priority_t pp);
static inline bool _dispatch_queue_need_override_retain(
		dispatch_queue_class_t dqu, pthread_priority_t pp);
static inline dispatch_priority_t _dispatch_queue_reset_override_priority(
		dispatch_queue_class_t dqu, bool qp_is_floor);
static inline bool _dispatch_queue_reinstate_override_priority(dispatch_queue_class_t dqu,
		dispatch_priority_t new_op);
static inline pthread_priority_t _dispatch_get_defaultpriority(void);
static inline void _dispatch_set_defaultpriority_override(void);
static inline void _dispatch_reset_defaultpriority(pthread_priority_t pp);
static inline pthread_priority_t _dispatch_get_priority(void);
static inline pthread_priority_t _dispatch_set_defaultpriority(
		pthread_priority_t pp, pthread_priority_t *new_pp);

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_xref_dispose(struct dispatch_queue_s *dq)
{
	if (slowpath(DISPATCH_QUEUE_IS_SUSPENDED(dq))) {
		// Arguments for and against this assert are within 6705399
		DISPATCH_CLIENT_CRASH(dq, "Release of a suspended object");
	}
	os_atomic_or2o(dq, dq_atomic_flags, DQF_RELEASED, relaxed);
}

#endif
#if DISPATCH_PURE_C

// Note to later developers: ensure that any initialization changes are
// made for statically allocated queues (i.e. _dispatch_main_q).
static inline void
_dispatch_queue_init(dispatch_queue_t dq, dispatch_queue_flags_t dqf,
		uint16_t width, bool inactive)
{
	uint64_t dq_state = DISPATCH_QUEUE_STATE_INIT_VALUE(width);

	if (inactive) {
		dq_state += DISPATCH_QUEUE_INACTIVE + DISPATCH_QUEUE_NEEDS_ACTIVATION;
		dq->do_ref_cnt++; // rdar://8181908 see _dispatch_queue_resume
	}
	dq->do_next = (struct dispatch_queue_s *)DISPATCH_OBJECT_LISTLESS;
	dqf |= (dispatch_queue_flags_t)width << DQF_WIDTH_SHIFT;
	os_atomic_store2o(dq, dq_atomic_flags, dqf, relaxed);
	dq->dq_state = dq_state;
	dq->dq_override_voucher = DISPATCH_NO_VOUCHER;
	dq->dq_serialnum =
			os_atomic_inc_orig(&_dispatch_queue_serial_numbers, relaxed);
}

/* Used by:
 * - _dispatch_queue_set_target_queue
 * - changing dispatch source handlers
 *
 * Tries to prevent concurrent wakeup of an inactive queue by suspending it.
 */
DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline bool
_dispatch_queue_try_inactive_suspend(dispatch_queue_t dq)
{
	uint64_t dq_state, value;

	(void)os_atomic_rmw_loop2o(dq, dq_state, dq_state, value, relaxed, {
		if (!fastpath(_dq_state_is_inactive(dq_state))) {
			os_atomic_rmw_loop_give_up(return false);
		}
		value = dq_state + DISPATCH_QUEUE_SUSPEND_INTERVAL;
	});
	if (slowpath(!_dq_state_is_suspended(dq_state)) ||
			slowpath(_dq_state_has_side_suspend_cnt(dq_state))) {
		// Crashing here means that 128+ dispatch_suspend() calls have been
		// made on an inactive object and then dispatch_set_target_queue() or
		// dispatch_set_*_handler() has been called.
		//
		// We don't want to handle the side suspend count in a codepath that
		// needs to be fast.
		DISPATCH_CLIENT_CRASH(dq, "Too many calls to dispatch_suspend() "
				"prior to calling dispatch_set_target_queue() "
				"or dispatch_set_*_handler()");
	}
	return true;
}

/* Must be used by any caller meaning to do a speculative wakeup when the caller
 * was preventing other wakeups (for example dispatch_resume() or a drainer not
 * doing a drain_try_unlock() and not observing DIRTY)
 *
 * In that case this call loads DIRTY with an acquire barrier so that when
 * other threads have made changes (such as dispatch_source_cancel()) the
 * caller can take these state machine changes into account in its decision to
 * wake up the object.
 */
DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_try_wakeup(dispatch_queue_t dq, uint64_t dq_state,
		dispatch_wakeup_flags_t flags)
{
	if (_dq_state_should_wakeup(dq_state)) {
		if (slowpath(_dq_state_is_dirty(dq_state))) {
			// <rdar://problem/14637483>
			// seq_cst wrt state changes that were flushed and not acted upon
			os_atomic_thread_fence(acquire);
		}
		return dx_wakeup(dq, 0, flags);
	}
	if (flags & DISPATCH_WAKEUP_CONSUME) {
		return _dispatch_release_tailcall(dq);
	}
}

/* Used by:
 * - _dispatch_queue_class_invoke (normal path)
 * - _dispatch_queue_override_invoke (stealer)
 *
 * Initial state must be { sc:0, ib:0, qf:0, dl:0 }
 * Final state forces { dl:self, qf:1, d: 0 }
 *    ib:1 is forced when the width acquired is equivalent to the barrier width
 */
DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline uint64_t
_dispatch_queue_drain_try_lock(dispatch_queue_t dq,
		dispatch_invoke_flags_t flags, uint64_t *dq_state)
{
	uint64_t pending_barrier_width =
			(dq->dq_width - 1) * DISPATCH_QUEUE_WIDTH_INTERVAL;
	uint64_t xor_owner_and_set_full_width =
			_dispatch_tid_self() | DISPATCH_QUEUE_WIDTH_FULL_BIT;
	uint64_t clear_enqueued_bit, old_state, new_state;

	if (flags & DISPATCH_INVOKE_STEALING) {
		clear_enqueued_bit = 0;
	} else {
		clear_enqueued_bit = DISPATCH_QUEUE_ENQUEUED;
	}

	os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, acquire, {
		new_state = old_state;
		new_state ^= clear_enqueued_bit;
		if (likely(_dq_state_is_runnable(old_state) &&
				!_dq_state_drain_locked(old_state))) {
			//
			// Only keep the HAS_WAITER bit (and ENQUEUED if stealing).
			// In particular acquiring the drain lock clears the DIRTY bit
			//
			new_state &= DISPATCH_QUEUE_DRAIN_PRESERVED_BITS_MASK;
			//
			// For the NOWAITERS_BIT case, the thread identity
			// has NOWAITERS_BIT set, and NOWAITERS_BIT was kept above,
			// so the xor below flips the NOWAITERS_BIT to 0 as expected.
			//
			// For the non inverted WAITERS_BIT case, WAITERS_BIT is not set in
			// the thread identity, and the xor leaves the bit alone.
			//
			new_state ^= xor_owner_and_set_full_width;
			if (_dq_state_has_pending_barrier(old_state) ||
					old_state + pending_barrier_width <
					DISPATCH_QUEUE_WIDTH_FULL_BIT) {
				new_state |= DISPATCH_QUEUE_IN_BARRIER;
			}
		} else if (!clear_enqueued_bit) {
			os_atomic_rmw_loop_give_up(break);
		}
	});

	if (dq_state) *dq_state = new_state;
	if (likely(_dq_state_is_runnable(old_state) &&
			!_dq_state_drain_locked(old_state))) {
		new_state &= DISPATCH_QUEUE_IN_BARRIER | DISPATCH_QUEUE_WIDTH_FULL_BIT;
		old_state &= DISPATCH_QUEUE_WIDTH_MASK;
		return new_state - old_state;
	}
	return 0;
}

/* Used by _dispatch_barrier_{try,}sync
 *
 * Note, this fails if any of e:1 or dl!=0, but that allows this code to be a
 * simple cmpxchg which is significantly faster on Intel, and makes a
 * significant difference on the uncontended codepath.
 *
 * See discussion for DISPATCH_QUEUE_DIRTY in queue_internal.h
 *
 * Initial state must be `completely idle`
 * Final state forces { ib:1, qf:1, w:0 }
 */
DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline bool
_dispatch_queue_try_acquire_barrier_sync(dispatch_queue_t dq)
{
	uint64_t value = DISPATCH_QUEUE_WIDTH_FULL_BIT | DISPATCH_QUEUE_IN_BARRIER;
	value |= _dispatch_tid_self();

	return os_atomic_cmpxchg2o(dq, dq_state,
			DISPATCH_QUEUE_STATE_INIT_VALUE(dq->dq_width), value, acquire);
}

/* Used by _dispatch_sync for root queues and some drain codepaths
 *
 * Root queues have no strict orderning and dispatch_sync() always goes through.
 * Drain is the sole setter of `dl` hence can use this non failing version of
 * _dispatch_queue_try_acquire_sync().
 *
 * Final state: { w += 1 }
 */
DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_reserve_sync_width(dispatch_queue_t dq)
{
	(void)os_atomic_add2o(dq, dq_state,
			DISPATCH_QUEUE_WIDTH_INTERVAL, relaxed);
}

/* Used by _dispatch_sync on non-serial queues
 *
 * Initial state must be { sc:0, ib:0, pb:0, d:0 }
 * Final state: { w += 1 }
 */
DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline bool
_dispatch_queue_try_reserve_sync_width(dispatch_queue_t dq)
{
	uint64_t dq_state, value;

	return os_atomic_rmw_loop2o(dq, dq_state, dq_state, value, relaxed, {
		if (!fastpath(_dq_state_is_sync_runnable(dq_state)) ||
				slowpath(_dq_state_is_dirty(dq_state)) ||
				slowpath(_dq_state_has_pending_barrier(dq_state))) {
			os_atomic_rmw_loop_give_up(return false);
		}
		value = dq_state + DISPATCH_QUEUE_WIDTH_INTERVAL;
	});
}

/* Used by _dispatch_apply_redirect
 *
 * Try to acquire at most da_width and returns what could be acquired,
 * possibly 0
 */
DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline uint32_t
_dispatch_queue_try_reserve_apply_width(dispatch_queue_t dq, uint32_t da_width)
{
	uint64_t dq_state, value;
	uint32_t width;

	(void)os_atomic_rmw_loop2o(dq, dq_state, dq_state, value, relaxed, {
		width = _dq_state_available_width(dq_state);
		if (!fastpath(width)) {
			os_atomic_rmw_loop_give_up(return 0);
		}
		if (width > da_width) {
			width = da_width;
		}
		value = dq_state + width * DISPATCH_QUEUE_WIDTH_INTERVAL;
	});
	return width;
}

/* Used by _dispatch_apply_redirect
 *
 * Release width acquired by _dispatch_queue_try_acquire_width
 */
DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_relinquish_width(dispatch_queue_t dq, uint32_t da_width)
{
	(void)os_atomic_sub2o(dq, dq_state,
			da_width * DISPATCH_QUEUE_WIDTH_INTERVAL, relaxed);
}

/* Used by target-queue recursing code
 *
 * Initial state must be { sc:0, ib:0, qf:0, pb:0, d:0 }
 * Final state: { w += 1 }
 */
DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline bool
_dispatch_queue_try_acquire_async(dispatch_queue_t dq)
{
	uint64_t dq_state, value;

	return os_atomic_rmw_loop2o(dq, dq_state, dq_state, value, acquire, {
		if (!fastpath(_dq_state_is_runnable(dq_state)) ||
				slowpath(_dq_state_is_dirty(dq_state)) ||
				slowpath(_dq_state_has_pending_barrier(dq_state))) {
			os_atomic_rmw_loop_give_up(return false);
		}
		value = dq_state + DISPATCH_QUEUE_WIDTH_INTERVAL;
	});
}

/* Used at the end of Drainers
 *
 * This adjusts the `owned` width when the next continuation is already known
 * to account for its barrierness.
 */
DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dispatch_queue_adjust_owned(dispatch_queue_t dq, uint64_t owned,
		struct dispatch_object_s *next_dc)
{
	uint64_t reservation;

	if (slowpath(dq->dq_width > 1)) {
		if (next_dc && _dispatch_object_is_barrier(next_dc)) {
			reservation  = DISPATCH_QUEUE_PENDING_BARRIER;
			reservation += (dq->dq_width - 1) * DISPATCH_QUEUE_WIDTH_INTERVAL;
			owned -= reservation;
		}
	}
	return owned;
}

/* Used at the end of Drainers
 *
 * Unlocking fails if the DIRTY bit is seen (and the queue is not suspended).
 * In that case, only the DIRTY bit is cleared. The DIRTY bit is therefore used
 * as a signal to renew the drain lock instead of releasing it.
 *
 * Successful unlock forces { dl:0, d:0, qo:0 } and gives back `owned`
 */
DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline bool
_dispatch_queue_drain_try_unlock(dispatch_queue_t dq, uint64_t owned)
{
	uint64_t old_state = os_atomic_load2o(dq, dq_state, relaxed);
	uint64_t new_state;
	dispatch_priority_t pp = 0, op;

	do {
		if (unlikely(_dq_state_is_dirty(old_state) &&
				!_dq_state_is_suspended(old_state))) {
			// just renew the drain lock with an acquire barrier, to see
			// what the enqueuer that set DIRTY has done.
			os_atomic_and2o(dq, dq_state, ~DISPATCH_QUEUE_DIRTY, acquire);
			_dispatch_queue_reinstate_override_priority(dq, pp);
			return false;
		}
		new_state = old_state - owned;
		if ((new_state & DISPATCH_QUEUE_WIDTH_FULL_BIT) ||
				_dq_state_is_suspended(old_state)) {
			// the test for the WIDTH_FULL_BIT is about narrow concurrent queues
			// releasing the drain lock while being at the width limit
			//
			// _non_barrier_complete() will set the DIRTY bit when going back
			// under the limit which will cause the try_unlock to fail
			new_state = DISPATCH_QUEUE_DRAIN_UNLOCK_PRESERVE_WAITERS_BIT(new_state);
		} else {
			new_state &= ~DISPATCH_QUEUE_DIRTY;
			new_state &= ~DISPATCH_QUEUE_DRAIN_UNLOCK_MASK;
			// This current owner is the only one that can clear HAS_OVERRIDE,
			// so accumulating reset overrides here is valid.
			if (unlikely(_dq_state_has_override(new_state))) {
				new_state &= ~DISPATCH_QUEUE_HAS_OVERRIDE;
				dispatch_assert(!_dispatch_queue_is_thread_bound(dq));
				op = _dispatch_queue_reset_override_priority(dq, false);
				if (op > pp) pp = op;
			}
		}
	} while (!fastpath(os_atomic_cmpxchgvw2o(dq, dq_state,
			old_state, new_state, &old_state, release)));

	if (_dq_state_has_override(old_state)) {
		// Ensure that the root queue sees that this thread was overridden.
		_dispatch_set_defaultpriority_override();
	}
	return true;
}

/* Used at the end of Drainers when the next work item is known
 * and that the dirty-head check isn't needed.
 *
 * This releases `owned`, clears DIRTY, and handles HAS_OVERRIDE when seen.
 */
DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dispatch_queue_drain_lock_transfer_or_unlock(dispatch_queue_t dq,
		uint64_t owned, mach_port_t next_owner, uint64_t *orig_state)
{
	uint64_t dq_state, value;

#ifdef DLOCK_NOWAITERS_BIT
	// The NOWAITERS_BIT state must not change through the transfer. It means
	// that if next_owner is 0 the bit must be flipped in the rmw_loop below,
	// and if next_owner is set, then the bit must be left unchanged.
	//
	// - when next_owner is 0, the xor below sets NOWAITERS_BIT in next_owner,
	//   which causes the second xor to flip the bit as expected.
	// - if next_owner is not 0, it has the NOWAITERS_BIT set, so we have to
	//   clear it so that the second xor leaves the NOWAITERS_BIT alone.
	next_owner ^= DLOCK_NOWAITERS_BIT;
#endif
	os_atomic_rmw_loop2o(dq, dq_state, dq_state, value, release, {
		value = dq_state - owned;
		// same as DISPATCH_QUEUE_DRAIN_UNLOCK_PRESERVE_WAITERS_BIT
		// but we want to be more efficient wrt the WAITERS_BIT
		value &= ~DISPATCH_QUEUE_DRAIN_OWNER_MASK;
		value &= ~DISPATCH_QUEUE_DRAIN_PENDED;
		value &= ~DISPATCH_QUEUE_DIRTY;
		value ^= next_owner;
	});

	if (_dq_state_has_override(dq_state)) {
		// Ensure that the root queue sees that this thread was overridden.
		_dispatch_set_defaultpriority_override();
	}
	if (orig_state) *orig_state = dq_state;
	return value;
}
#define _dispatch_queue_drain_unlock(dq, owned, orig) \
		_dispatch_queue_drain_lock_transfer_or_unlock(dq, owned, 0, orig)

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_drain_transfer_lock(dispatch_queue_t dq,
		uint64_t to_unlock, dispatch_object_t dou)
{
	mach_port_t th_next = 0;
	if (dou._dc->dc_flags & DISPATCH_OBJ_BARRIER_BIT) {
		th_next = (mach_port_t)dou._dc->dc_data;
	}
	_dispatch_queue_drain_lock_transfer_or_unlock(dq, to_unlock, th_next, NULL);
}


#pragma mark -
#pragma mark os_mpsc_queue

// type_t * {volatile,const,_Atomic,...} -> type_t *
// type_t[] -> type_t *
#define os_unqualified_pointer_type(expr) \
		typeof(typeof(*(expr)) *)

#define os_mpsc_node_type(q, _ns)  \
		os_unqualified_pointer_type((q)->_ns##_head)

//
// Multi Producer calls, can be used safely concurrently
//

// Returns true when the queue was empty and the head must be set
#define os_mpsc_push_update_tail_list(q, _ns, head, tail, _o_next)  ({ \
		os_mpsc_node_type(q, _ns) _head = (head), _tail = (tail), _prev; \
		_tail->_o_next = NULL; \
		_prev = os_atomic_xchg2o((q), _ns##_tail, _tail, release); \
		if (fastpath(_prev)) { \
			os_atomic_store2o(_prev, _o_next, _head, relaxed); \
		} \
		(_prev == NULL); \
	})

// Returns true when the queue was empty and the head must be set
#define os_mpsc_push_update_tail(q, _ns, o, _o_next)  ({ \
		os_mpsc_node_type(q, _ns) _o = (o); \
		os_mpsc_push_update_tail_list(q, _ns, _o, _o, _o_next); \
	})

#define os_mpsc_push_update_head(q, _ns, o)  ({ \
		os_atomic_store2o((q), _ns##_head, o, relaxed); \
	})

//
// Single Consumer calls, can NOT be used safely concurrently
//

#define os_mpsc_get_head(q, _ns)  ({ \
		os_mpsc_node_type(q, _ns) _head; \
		_dispatch_wait_until(_head = (q)->_ns##_head); \
		_head; \
	})

#define os_mpsc_pop_head(q, _ns, head, _o_next)  ({ \
		typeof(q) _q = (q); \
		os_mpsc_node_type(_q, _ns) _head = (head), _n = fastpath(_head->_o_next); \
		os_atomic_store2o(_q, _ns##_head, _n, relaxed); \
		/* 22708742: set tail to NULL with release, so that NULL write */ \
		/* to head above doesn't clobber head from concurrent enqueuer */ \
		if (!_n && !os_atomic_cmpxchg2o(_q, _ns##_tail, _head, NULL, release)) { \
			_dispatch_wait_until(_n = fastpath(_head->_o_next)); \
			os_atomic_store2o(_q, _ns##_head, _n, relaxed); \
		} \
		_n; \
	})

#define os_mpsc_undo_pop_head(q, _ns, head, next, _o_next)  ({ \
		typeof(q) _q = (q); \
		os_mpsc_node_type(_q, _ns) _head = (head), _n = (next); \
		if (!_n && !os_atomic_cmpxchg2o(_q, _ns##_tail, NULL, _head, relaxed)) { \
			_dispatch_wait_until(_n = _q->_ns##_head); \
			_head->_o_next = _n; \
		} \
		os_atomic_store2o(_q, _ns##_head, _head, relaxed); \
	})

#define os_mpsc_capture_snapshot(q, _ns, tail)  ({ \
		typeof(q) _q = (q); \
		os_mpsc_node_type(_q, _ns) _head; \
		_dispatch_wait_until(_head = _q->_ns##_head); \
		os_atomic_store2o(_q, _ns##_head, NULL, relaxed); \
		/* 22708742: set tail to NULL with release, so that NULL write */ \
		/* to head above doesn't clobber head from concurrent enqueuer */ \
		*(tail) = os_atomic_xchg2o(_q, _ns##_tail, NULL, release); \
		_head; \
	})

#define os_mpsc_pop_snapshot_head(head, tail, _o_next) ({ \
		os_unqualified_pointer_type(head) _head = (head), _n = NULL; \
		if (_head != (tail)) { \
			_dispatch_wait_until(_n = _head->_o_next); \
		}; \
		_n; })

#define os_mpsc_prepend(q, _ns, head, tail, _o_next)  ({ \
		typeof(q) _q = (q); \
		os_mpsc_node_type(_q, _ns) _head = (head), _tail = (tail), _n; \
		_tail->_o_next = NULL; \
		if (!os_atomic_cmpxchg2o(_q, _ns##_tail, NULL, _tail, release)) { \
			_dispatch_wait_until(_n = _q->_ns##_head); \
			_tail->_o_next = _n; \
		} \
		os_atomic_store2o(_q, _ns##_head, _head, relaxed); \
	})

#pragma mark -
#pragma mark dispatch_queue_t tq lock

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_sidelock_trylock(dispatch_queue_t dq, pthread_priority_t pp)
{
	dispatch_lock_owner owner;
	if (_dispatch_unfair_lock_trylock(&dq->dq_sidelock, &owner)) {
		return true;
	}
	_dispatch_wqthread_override_start_check_owner(owner, pp,
			&dq->dq_sidelock.dul_lock);
	return false;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_sidelock_lock(dispatch_queue_t dq)
{
	return _dispatch_unfair_lock_lock(&dq->dq_sidelock);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_sidelock_tryunlock(dispatch_queue_t dq)
{
	if (_dispatch_unfair_lock_tryunlock(&dq->dq_sidelock)) {
		return true;
	}
	// Ensure that the root queue sees that this thread was overridden.
	_dispatch_set_defaultpriority_override();
	return false;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_sidelock_unlock(dispatch_queue_t dq)
{
	if (_dispatch_unfair_lock_unlock_had_failed_trylock(&dq->dq_sidelock)) {
		// Ensure that the root queue sees that this thread was overridden.
		_dispatch_set_defaultpriority_override();
	}
}

#pragma mark -
#pragma mark dispatch_queue_t misc

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_t
_dispatch_queue_get_current(void)
{
	return (dispatch_queue_t)_dispatch_thread_getspecific(dispatch_queue_key);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_set_current(dispatch_queue_t dq)
{
	_dispatch_thread_setspecific(dispatch_queue_key, dq);
}

DISPATCH_ALWAYS_INLINE
static inline struct dispatch_object_s*
_dispatch_queue_head(dispatch_queue_t dq)
{
	return os_mpsc_get_head(dq, dq_items);
}

DISPATCH_ALWAYS_INLINE
static inline struct dispatch_object_s*
_dispatch_queue_next(dispatch_queue_t dq, struct dispatch_object_s *dc)
{
	return os_mpsc_pop_head(dq, dq_items, dc, do_next);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_push_update_tail(dispatch_queue_t dq,
		struct dispatch_object_s *tail)
{
	// if we crash here with a value less than 0x1000, then we are
	// at a known bug in client code. for example, see
	// _dispatch_queue_dispose or _dispatch_atfork_child
	return os_mpsc_push_update_tail(dq, dq_items, tail, do_next);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_push_update_tail_list(dispatch_queue_t dq,
		struct dispatch_object_s *head, struct dispatch_object_s *tail)
{
	// if we crash here with a value less than 0x1000, then we are
	// at a known bug in client code. for example, see
	// _dispatch_queue_dispose or _dispatch_atfork_child
	return os_mpsc_push_update_tail_list(dq, dq_items, head, tail, do_next);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_push_update_head(dispatch_queue_t dq,
		struct dispatch_object_s *head, bool retained)
{
	if (dx_type(dq) == DISPATCH_QUEUE_GLOBAL_ROOT_TYPE) {
		dispatch_assert(!retained);
		// Lie about "retained" here, it generates better assembly in this
		// hotpath, and _dispatch_root_queue_wakeup knows to ignore this
		// fake "WAKEUP_CONSUME" bit when it also sees WAKEUP_FLUSH.
		//
		// We need to bypass the retain below because pthread root queues
		// are not global and retaining them would be wrong.
		//
		// We should eventually have a typeflag for "POOL" kind of root queues.
		retained = true;
	}
	// The queue must be retained before dq_items_head is written in order
	// to ensure that the reference is still valid when _dispatch_queue_wakeup
	// is called. Otherwise, if preempted between the assignment to
	// dq_items_head and _dispatch_queue_wakeup, the blocks submitted to the
	// queue may release the last reference to the queue when invoked by
	// _dispatch_queue_drain. <rdar://problem/6932776>
	if (!retained) _dispatch_retain(dq);
	os_mpsc_push_update_head(dq, dq_items, head);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_push_list(dispatch_queue_t dq, dispatch_object_t _head,
		dispatch_object_t _tail, pthread_priority_t pp, unsigned int n)
{
	struct dispatch_object_s *head = _head._do, *tail = _tail._do;
	bool override = _dispatch_queue_need_override_retain(dq, pp);
	dispatch_queue_flags_t flags;
	if (slowpath(_dispatch_queue_push_update_tail_list(dq, head, tail))) {
		_dispatch_queue_push_update_head(dq, head, override);
		if (fastpath(dx_type(dq) == DISPATCH_QUEUE_GLOBAL_ROOT_TYPE)) {
			return _dispatch_queue_push_list_slow(dq, n);
		}
		flags = DISPATCH_WAKEUP_CONSUME | DISPATCH_WAKEUP_FLUSH;
	} else if (override) {
		flags = DISPATCH_WAKEUP_CONSUME | DISPATCH_WAKEUP_OVERRIDING;
	} else {
		return;
	}
	dx_wakeup(dq, pp, flags);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_push_inline(dispatch_queue_t dq, dispatch_object_t _tail,
		pthread_priority_t pp, dispatch_wakeup_flags_t flags)
{
	struct dispatch_object_s *tail = _tail._do;
	bool override = _dispatch_queue_need_override(dq, pp);
	if (flags & DISPATCH_WAKEUP_SLOW_WAITER) {
		// when SLOW_WAITER is set, we borrow the reference of the caller
		if (unlikely(_dispatch_queue_push_update_tail(dq, tail))) {
			_dispatch_queue_push_update_head(dq, tail, true);
			flags = DISPATCH_WAKEUP_SLOW_WAITER | DISPATCH_WAKEUP_FLUSH;
		} else if (override) {
			flags = DISPATCH_WAKEUP_SLOW_WAITER | DISPATCH_WAKEUP_OVERRIDING;
		} else {
			flags = DISPATCH_WAKEUP_SLOW_WAITER;
		}
	} else {
		if (override) _dispatch_retain(dq);
		if (unlikely(_dispatch_queue_push_update_tail(dq, tail))) {
			_dispatch_queue_push_update_head(dq, tail, override);
			flags = DISPATCH_WAKEUP_CONSUME | DISPATCH_WAKEUP_FLUSH;
		} else if (override) {
			flags = DISPATCH_WAKEUP_CONSUME | DISPATCH_WAKEUP_OVERRIDING;
		} else {
			return;
		}
	}
	return dx_wakeup(dq, pp, flags);
}

struct _dispatch_identity_s {
	pthread_priority_t old_pp;
};

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_root_queue_identity_assume(struct _dispatch_identity_s *di,
		pthread_priority_t pp)
{
	// assumed_rq was set by the caller, we need to fake the priorities
	dispatch_queue_t assumed_rq = _dispatch_queue_get_current();

	dispatch_assert(dx_type(assumed_rq) == DISPATCH_QUEUE_GLOBAL_ROOT_TYPE);

	di->old_pp = _dispatch_get_defaultpriority();

	if (!(assumed_rq->dq_priority & _PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG)) {
		if (!pp) {
			pp = _dispatch_get_priority();
			// _dispatch_root_queue_drain_deferred_item() may turn a manager
			// thread into a regular root queue, and we must never try to
			// restore the manager flag once we became a regular work queue
			// thread.
			pp &= ~(pthread_priority_t)_PTHREAD_PRIORITY_EVENT_MANAGER_FLAG;
		}
		if ((pp & _PTHREAD_PRIORITY_QOS_CLASS_MASK) >
				(assumed_rq->dq_priority & _PTHREAD_PRIORITY_QOS_CLASS_MASK)) {
			_dispatch_wqthread_override_start(_dispatch_tid_self(), pp);
			// Ensure that the root queue sees that this thread was overridden.
			_dispatch_set_defaultpriority_override();
		}
	}
	_dispatch_reset_defaultpriority(assumed_rq->dq_priority);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_root_queue_identity_restore(struct _dispatch_identity_s *di)
{
	_dispatch_reset_defaultpriority(di->old_pp);
}

typedef dispatch_queue_t
_dispatch_queue_class_invoke_handler_t(dispatch_object_t,
		dispatch_invoke_flags_t, uint64_t *owned, struct dispatch_object_s **);

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_class_invoke(dispatch_object_t dou,
		dispatch_invoke_flags_t flags,
		_dispatch_queue_class_invoke_handler_t invoke)
{
	dispatch_queue_t dq = dou._dq;
	struct dispatch_object_s *dc = NULL;
	dispatch_queue_t tq = NULL;
	uint64_t dq_state, to_unlock = 0;
	bool owning = !slowpath(flags & DISPATCH_INVOKE_STEALING);
	bool overriding = slowpath(flags & DISPATCH_INVOKE_OVERRIDING);

	// When called from a plain _dispatch_queue_drain:
	//   overriding = false
	//   owning = true
	//
	// When called from an override continuation:
	//   overriding = true
	//   owning depends on whether the override embedded the queue or steals
	DISPATCH_COMPILER_CAN_ASSUME(owning || overriding);

	if (owning) {
		dq->do_next = DISPATCH_OBJECT_LISTLESS;
	}
	to_unlock = _dispatch_queue_drain_try_lock(dq, flags, &dq_state);
	if (likely(to_unlock)) {
		struct _dispatch_identity_s di;
		pthread_priority_t old_dp;

drain_pending_barrier:
		if (overriding) {
			_dispatch_object_debug(dq, "stolen onto thread 0x%x, 0x%lx",
					_dispatch_tid_self(), _dispatch_get_defaultpriority());
			_dispatch_root_queue_identity_assume(&di, 0);
		}

		if (!(flags & DISPATCH_INVOKE_MANAGER_DRAIN)) {
			pthread_priority_t op, dp;

			old_dp = _dispatch_set_defaultpriority(dq->dq_priority, &dp);
			op = dq->dq_override;
			if (op > (dp & _PTHREAD_PRIORITY_QOS_CLASS_MASK)) {
				_dispatch_wqthread_override_start(_dispatch_tid_self(), op);
				// Ensure that the root queue sees that this thread was overridden.
				_dispatch_set_defaultpriority_override();
			}
		}

		flags = _dispatch_queue_merge_autorelease_frequency(dq, flags);
attempt_running_slow_head:
		tq = invoke(dq, flags, &to_unlock, &dc);
		if (slowpath(tq)) {
			// Either dc is set, which is a deferred invoke case
			//
			// or only tq is and it means a reenqueue is required, because of:
			// a retarget, a suspension, or a width change.
			//
			// In both cases, we want to bypass the check for DIRTY.
			// That may cause us to leave DIRTY in place but all drain lock
			// acquirers clear it
		} else {
			if (!_dispatch_queue_drain_try_unlock(dq, to_unlock)) {
				goto attempt_running_slow_head;
			}
			to_unlock = 0;
		}
		if (overriding) {
			_dispatch_root_queue_identity_restore(&di);
		}
		if (!(flags & DISPATCH_INVOKE_MANAGER_DRAIN)) {
			_dispatch_reset_defaultpriority(old_dp);
		}
	} else if (overriding) {
		uint32_t owner = _dq_state_drain_owner(dq_state);
		pthread_priority_t p = dq->dq_override;
		if (owner && p) {
			_dispatch_object_debug(dq, "overriding thr 0x%x to priority 0x%lx",
					owner, p);
			_dispatch_wqthread_override_start_check_owner(owner, p,
					&dq->dq_state_lock);
		}
	}

	if (owning) {
		_dispatch_introspection_queue_item_complete(dq);
	}

	if (tq && dc) {
		return _dispatch_queue_drain_deferred_invoke(dq, flags, to_unlock, dc);
	}

	if (tq) {
		bool full_width_upgrade_allowed = (tq == _dispatch_queue_get_current());
		uint64_t old_state, new_state;

		os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, release, {
			new_state = old_state - to_unlock;
			if (full_width_upgrade_allowed && _dq_state_is_runnable(new_state) &&
					_dq_state_has_pending_barrier(new_state)) {
				new_state += DISPATCH_QUEUE_IN_BARRIER;
				new_state += DISPATCH_QUEUE_WIDTH_INTERVAL;
				new_state -= DISPATCH_QUEUE_PENDING_BARRIER;
				new_state += to_unlock & DISPATCH_QUEUE_DRAIN_PRESERVED_BITS_MASK;
			} else {
				new_state = DISPATCH_QUEUE_DRAIN_UNLOCK_PRESERVE_WAITERS_BIT(new_state);
				if (_dq_state_should_wakeup(new_state)) {
					// drain was not interupted for suspension
					// we will reenqueue right away, just put ENQUEUED back
					new_state |= DISPATCH_QUEUE_ENQUEUED;
					new_state |= DISPATCH_QUEUE_DIRTY;
				}
			}
		});
		if (_dq_state_is_in_barrier(new_state)) {
			// we did a "full width upgrade" and just added IN_BARRIER
			// so adjust what we own and drain again
			to_unlock &= DISPATCH_QUEUE_ENQUEUED;
			to_unlock += DISPATCH_QUEUE_IN_BARRIER;
			to_unlock += dq->dq_width * DISPATCH_QUEUE_WIDTH_INTERVAL;
			goto drain_pending_barrier;
		}
		if (_dq_state_has_override(old_state)) {
			// Ensure that the root queue sees that this thread was overridden.
			_dispatch_set_defaultpriority_override();
		}

		if ((old_state ^ new_state) & DISPATCH_QUEUE_ENQUEUED) {
			return _dispatch_queue_push(tq, dq, 0);
		}
	}

	return _dispatch_release_tailcall(dq);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_class_probe(dispatch_queue_class_t dqu)
{
	struct dispatch_object_s *tail;
	// seq_cst wrt atomic store to dq_state <rdar://problem/14637483>
	// seq_cst wrt atomic store to dq_flags <rdar://problem/22623242>
	tail = os_atomic_load2o(dqu._oq, oq_items_tail, ordered);
	return slowpath(tail != NULL);
}

DISPATCH_ALWAYS_INLINE DISPATCH_CONST
static inline bool
_dispatch_is_in_root_queues_array(dispatch_queue_t dq)
{
	return (dq >= _dispatch_root_queues) &&
			(dq < _dispatch_root_queues + _DISPATCH_ROOT_QUEUE_IDX_COUNT);
}

DISPATCH_ALWAYS_INLINE DISPATCH_CONST
static inline dispatch_queue_t
_dispatch_get_root_queue(qos_class_t priority, bool overcommit)
{
	if (overcommit) switch (priority) {
	case _DISPATCH_QOS_CLASS_MAINTENANCE:
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS_OVERCOMMIT];
	case _DISPATCH_QOS_CLASS_BACKGROUND:
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS_OVERCOMMIT];
	case _DISPATCH_QOS_CLASS_UTILITY:
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS_OVERCOMMIT];
	case _DISPATCH_QOS_CLASS_DEFAULT:
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS_OVERCOMMIT];
	case _DISPATCH_QOS_CLASS_USER_INITIATED:
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS_OVERCOMMIT];
	case _DISPATCH_QOS_CLASS_USER_INTERACTIVE:
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS_OVERCOMMIT];
	} else switch (priority) {
	case _DISPATCH_QOS_CLASS_MAINTENANCE:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS];
	case _DISPATCH_QOS_CLASS_BACKGROUND:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS];
	case _DISPATCH_QOS_CLASS_UTILITY:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS];
	case _DISPATCH_QOS_CLASS_DEFAULT:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS];
	case _DISPATCH_QOS_CLASS_USER_INITIATED:
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS];
	case _DISPATCH_QOS_CLASS_USER_INTERACTIVE:
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS];
	}
	return NULL;
}

#if HAVE_PTHREAD_WORKQUEUE_QOS
DISPATCH_ALWAYS_INLINE DISPATCH_CONST
static inline dispatch_queue_t
_dispatch_get_root_queue_for_priority(pthread_priority_t pp, bool overcommit)
{
	uint32_t idx;

	pp &= _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	idx = (uint32_t)__builtin_ffs((int)pp);
	if (unlikely(!_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS]
			.dq_priority)) {
		// If kernel doesn't support maintenance, bottom bit is background.
		// Shift to our idea of where background bit is.
		idx++;
	}
	// ffs starts at 1, and account for the QOS_CLASS_SHIFT
	// if pp is 0, idx is 0 or 1 and this will wrap to a value larger than
	// DISPATCH_QOS_COUNT
	idx -= (_PTHREAD_PRIORITY_QOS_CLASS_SHIFT + 1);
	if (unlikely(idx >= DISPATCH_QUEUE_QOS_COUNT)) {
		DISPATCH_CLIENT_CRASH(pp, "Corrupted priority");
	}
	return &_dispatch_root_queues[2 * idx + overcommit];
}
#endif

DISPATCH_ALWAYS_INLINE DISPATCH_CONST
static inline dispatch_queue_t
_dispatch_get_root_queue_with_overcommit(dispatch_queue_t rq, bool overcommit)
{
	bool rq_overcommit = (rq->dq_priority & _PTHREAD_PRIORITY_OVERCOMMIT_FLAG);
	// root queues in _dispatch_root_queues are not overcommit for even indices
	// and overcommit for odd ones, so fixing overcommit is either returning
	// the same queue, or picking its neighbour in _dispatch_root_queues
	if (overcommit && !rq_overcommit) {
		return rq + 1;
	}
	if (!overcommit && rq_overcommit) {
		return rq - 1;
	}
	return rq;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_set_bound_thread(dispatch_queue_t dq)
{
	// Tag thread-bound queues with the owning thread
	dispatch_assert(_dispatch_queue_is_thread_bound(dq));
	mach_port_t old_owner, self = _dispatch_tid_self();
	uint64_t dq_state = os_atomic_or_orig2o(dq, dq_state, self, relaxed);
	if (unlikely(old_owner = _dq_state_drain_owner(dq_state))) {
		DISPATCH_INTERNAL_CRASH(old_owner, "Queue bound twice");
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_clear_bound_thread(dispatch_queue_t dq)
{
	uint64_t dq_state, value;

	dispatch_assert(_dispatch_queue_is_thread_bound(dq));
	os_atomic_rmw_loop2o(dq, dq_state, dq_state, value, relaxed, {
		value = DISPATCH_QUEUE_DRAIN_UNLOCK_PRESERVE_WAITERS_BIT(dq_state);
	});
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_pthread_root_queue_observer_hooks_t
_dispatch_get_pthread_root_queue_observer_hooks(void)
{
	return _dispatch_thread_getspecific(
			dispatch_pthread_root_queue_observer_hooks_key);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_set_pthread_root_queue_observer_hooks(
		dispatch_pthread_root_queue_observer_hooks_t observer_hooks)
{
	_dispatch_thread_setspecific(dispatch_pthread_root_queue_observer_hooks_key,
			observer_hooks);
}

#pragma mark -
#pragma mark dispatch_priority

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_get_defaultpriority(void)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pthread_priority_t pp = (uintptr_t)_dispatch_thread_getspecific(
			dispatch_defaultpriority_key);
	return pp;
#else
	return 0;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_reset_defaultpriority(pthread_priority_t pp)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pthread_priority_t old_pp = _dispatch_get_defaultpriority();
	// If an inner-loop or'd in the override flag to the per-thread priority,
	// it needs to be propagated up the chain.
	pp |= old_pp & _PTHREAD_PRIORITY_OVERRIDE_FLAG;
	_dispatch_thread_setspecific(dispatch_defaultpriority_key, (void*)pp);
#else
	(void)pp;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_set_defaultpriority_override(void)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pthread_priority_t old_pp = _dispatch_get_defaultpriority();
	pthread_priority_t pp = old_pp | _PTHREAD_PRIORITY_OVERRIDE_FLAG;

	_dispatch_thread_setspecific(dispatch_defaultpriority_key, (void*)pp);
#endif
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_reset_defaultpriority_override(void)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pthread_priority_t old_pp = _dispatch_get_defaultpriority();
	pthread_priority_t pp = old_pp &
			~((pthread_priority_t)_PTHREAD_PRIORITY_OVERRIDE_FLAG);

	_dispatch_thread_setspecific(dispatch_defaultpriority_key, (void*)pp);
	return unlikely(pp != old_pp);
#endif
	return false;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_priority_inherit_from_target(dispatch_queue_t dq,
		dispatch_queue_t tq)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	const dispatch_priority_t rootqueue_flag = _PTHREAD_PRIORITY_ROOTQUEUE_FLAG;
	const dispatch_priority_t inherited_flag = _PTHREAD_PRIORITY_INHERIT_FLAG;
	const dispatch_priority_t defaultqueue_flag =
			_PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG;
	dispatch_priority_t dqp = dq->dq_priority, tqp = tq->dq_priority;
	if ((!(dqp & ~_PTHREAD_PRIORITY_FLAGS_MASK) || (dqp & inherited_flag)) &&
			(tqp & rootqueue_flag)) {
		if (tqp & defaultqueue_flag) {
			dq->dq_priority = 0;
		} else {
			dq->dq_priority = (tqp & ~rootqueue_flag) | inherited_flag;
		}
	}
#else
	(void)dq; (void)tq;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_set_defaultpriority(pthread_priority_t pp, pthread_priority_t *new_pp)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	const pthread_priority_t default_priority_preserved_flags =
			_PTHREAD_PRIORITY_OVERRIDE_FLAG|_PTHREAD_PRIORITY_OVERCOMMIT_FLAG;
	pthread_priority_t old_pp = _dispatch_get_defaultpriority();
	if (old_pp) {
		pthread_priority_t flags, defaultqueue, basepri;
		flags = (pp & _PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG);
		defaultqueue = (old_pp & _PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG);
		basepri = (old_pp & ~_PTHREAD_PRIORITY_FLAGS_MASK);
		pp &= ~_PTHREAD_PRIORITY_FLAGS_MASK;
		if (!pp) {
			flags = _PTHREAD_PRIORITY_INHERIT_FLAG | defaultqueue;
			pp = basepri;
		} else if (pp < basepri && !defaultqueue) { // rdar://16349734
			pp = basepri;
		}
		pp |= flags | (old_pp & default_priority_preserved_flags);
	}
	_dispatch_thread_setspecific(dispatch_defaultpriority_key, (void*)pp);
	if (new_pp) *new_pp = pp;
	return old_pp;
#else
	(void)pp; (void)new_pp;
	return 0;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_priority_adopt(pthread_priority_t pp, unsigned long flags)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pthread_priority_t defaultpri = _dispatch_get_defaultpriority();
	bool enforce, inherited, defaultqueue;
	enforce = (flags & DISPATCH_PRIORITY_ENFORCE) ||
			(pp & _PTHREAD_PRIORITY_ENFORCE_FLAG);
	inherited = (defaultpri & _PTHREAD_PRIORITY_INHERIT_FLAG);
	defaultqueue = (defaultpri & _PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG);
	defaultpri &= ~_PTHREAD_PRIORITY_FLAGS_MASK;
	pp &= ~_PTHREAD_PRIORITY_FLAGS_MASK;

	if (!pp) {
		return defaultpri;
	} else if (defaultqueue) { // rdar://16349734
		return pp;
	} else if (pp < defaultpri) {
		return defaultpri;
	} else if (enforce || inherited) {
		return pp;
	} else {
		return defaultpri;
	}
#else
	(void)pp; (void)flags;
	return 0;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_priority_inherit_from_root_queue(pthread_priority_t pp,
		dispatch_queue_t rq)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pthread_priority_t p = pp & ~_PTHREAD_PRIORITY_FLAGS_MASK;
	pthread_priority_t rqp = rq->dq_priority & ~_PTHREAD_PRIORITY_FLAGS_MASK;
	pthread_priority_t defaultqueue =
			rq->dq_priority & _PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG;

	if (!p || (!defaultqueue && p < rqp)) {
		p = rqp | defaultqueue;
	}
	return p | (rq->dq_priority & _PTHREAD_PRIORITY_OVERCOMMIT_FLAG);
#else
	(void)rq; (void)pp;
	return 0;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_get_priority(void)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pthread_priority_t pp = (uintptr_t)
			_dispatch_thread_getspecific(dispatch_priority_key);
	return pp;
#else
	return 0;
#endif
}

#if HAVE_PTHREAD_WORKQUEUE_QOS
DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_priority_compute_update(pthread_priority_t pp)
{
	dispatch_assert(pp != DISPATCH_NO_PRIORITY);
	if (!_dispatch_set_qos_class_enabled) return 0;
	// the priority in _dispatch_get_priority() only tracks manager-ness
	// and overcommit, which is inherited from the current value for each update
	// however if the priority had the NEEDS_UNBIND flag set we need to clear it
	// the first chance we get
	//
	// the manager bit is invalid input, but we keep it to get meaningful
	// assertions in _dispatch_set_priority_and_voucher_slow()
	pp &= _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG | ~_PTHREAD_PRIORITY_FLAGS_MASK;
	pthread_priority_t cur_priority = _dispatch_get_priority();
	pthread_priority_t unbind = _PTHREAD_PRIORITY_NEEDS_UNBIND_FLAG;
	pthread_priority_t overcommit = _PTHREAD_PRIORITY_OVERCOMMIT_FLAG;
	if (unlikely(cur_priority & unbind)) {
		// else we always need an update if the NEEDS_UNBIND flag is set
		// the slowpath in _dispatch_set_priority_and_voucher_slow() will
		// adjust the priority further with the proper overcommitness
		return pp ? pp : (cur_priority & ~unbind);
	} else {
		cur_priority &= ~overcommit;
	}
	if (unlikely(pp != cur_priority)) return pp;
	return 0;
}
#endif

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline voucher_t
_dispatch_set_priority_and_voucher(pthread_priority_t pp,
		voucher_t v, _dispatch_thread_set_self_t flags)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pp = _dispatch_priority_compute_update(pp);
	if (likely(!pp)) {
		if (v == DISPATCH_NO_VOUCHER) {
			return DISPATCH_NO_VOUCHER;
		}
		if (likely(v == _voucher_get())) {
			bool retained = flags & DISPATCH_VOUCHER_CONSUME;
			if (flags & DISPATCH_VOUCHER_REPLACE) {
				if (retained && v) _voucher_release_no_dispose(v);
				v = DISPATCH_NO_VOUCHER;
			} else {
				if (!retained && v) _voucher_retain(v);
			}
			return v;
		}
	}
	return _dispatch_set_priority_and_voucher_slow(pp, v, flags);
#else
	(void)pp; (void)v; (void)flags;
	return DISPATCH_NO_VOUCHER;
#endif
}

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline voucher_t
_dispatch_adopt_priority_and_set_voucher(pthread_priority_t pp,
		voucher_t v, _dispatch_thread_set_self_t flags)
{
	pthread_priority_t p = 0;
	if (pp != DISPATCH_NO_PRIORITY) {
		p = _dispatch_priority_adopt(pp, flags);
	}
	return _dispatch_set_priority_and_voucher(p, v, flags);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_reset_priority_and_voucher(pthread_priority_t pp, voucher_t v)
{
	if (pp == DISPATCH_NO_PRIORITY) pp = 0;
	(void)_dispatch_set_priority_and_voucher(pp, v,
			DISPATCH_VOUCHER_CONSUME | DISPATCH_VOUCHER_REPLACE);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_reset_voucher(voucher_t v, _dispatch_thread_set_self_t flags)
{
	flags |= DISPATCH_VOUCHER_CONSUME | DISPATCH_VOUCHER_REPLACE;
	(void)_dispatch_set_priority_and_voucher(0, v, flags);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_need_override(dispatch_queue_class_t dqu, pthread_priority_t pp)
{
	// global queues have their override set to DISPATCH_SATURATED_OVERRIDE
	// which makes this test always return false for them.
	return dqu._oq->oq_override < (pp & _PTHREAD_PRIORITY_QOS_CLASS_MASK);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_received_override(dispatch_queue_class_t dqu,
		pthread_priority_t pp)
{
	dispatch_assert(dqu._oq->oq_override != DISPATCH_SATURATED_OVERRIDE);
	return dqu._oq->oq_override > (pp & _PTHREAD_PRIORITY_QOS_CLASS_MASK);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_need_override_retain(dispatch_queue_class_t dqu,
		pthread_priority_t pp)
{
	if (_dispatch_queue_need_override(dqu, pp)) {
		_os_object_retain_internal_inline(dqu._oq->_as_os_obj);
		return true;
	}
	return false;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_reinstate_override_priority(dispatch_queue_class_t dqu,
		dispatch_priority_t new_op)
{
	dispatch_priority_t old_op;
	new_op &= _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	if (!new_op) return false;
	os_atomic_rmw_loop2o(dqu._oq, oq_override, old_op, new_op, relaxed, {
		if (new_op <= old_op) {
			os_atomic_rmw_loop_give_up(return false);
		}
	});
	return true;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_override_priority(dispatch_queue_class_t dqu,
		pthread_priority_t *pp, dispatch_wakeup_flags_t *flags)
{
	os_mpsc_queue_t oq = dqu._oq;
	dispatch_priority_t qp = oq->oq_priority & _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	dispatch_priority_t np = (*pp & _PTHREAD_PRIORITY_QOS_CLASS_MASK);
	dispatch_priority_t o;

	_dispatch_assert_is_valid_qos_override(np);
	if (oq->oq_priority & _PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG) {
		qp = 0;
	} else if (*flags & DISPATCH_WAKEUP_SLOW_WAITER) {
		// when a queue is used as a lock its priority doesn't count
	} else if (np < qp) {
		// for asynchronous workitems, queue priority is the floor for overrides
		np = qp;
	}
	*flags &= ~_DISPATCH_WAKEUP_OVERRIDE_BITS;

	// this optimizes for the case when no update of the override is required
	// os_atomic_rmw_loop2o optimizes for the case when the update happens,
	// and can't be used.
	o = os_atomic_load2o(oq, oq_override, relaxed);
	do {
		if (likely(np <= o)) break;
	} while (unlikely(!os_atomic_cmpxchgvw2o(oq, oq_override, o, np, &o, relaxed)));

	if (np <= o) {
		*pp = o;
	} else {
		*flags |= DISPATCH_WAKEUP_OVERRIDING;
		*pp = np;
	}
	if (o > qp) {
		*flags |= DISPATCH_WAKEUP_WAS_OVERRIDDEN;
	}
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_priority_t
_dispatch_queue_reset_override_priority(dispatch_queue_class_t dqu,
		bool qp_is_floor)
{
	os_mpsc_queue_t oq = dqu._oq;
	dispatch_priority_t p = 0;
	if (qp_is_floor) {
		// thread bound queues floor their dq_override to their
		// priority to avoid receiving useless overrides
		p = oq->oq_priority & _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	}
	dispatch_priority_t o = os_atomic_xchg2o(oq, oq_override, p, relaxed);
	dispatch_assert(o != DISPATCH_SATURATED_OVERRIDE);
	return (o > p) ? o : 0;
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_priority_propagate(void)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pthread_priority_t pp = _dispatch_get_priority();
	pp &= ~_PTHREAD_PRIORITY_FLAGS_MASK;
	if (pp > _dispatch_user_initiated_priority) {
		// Cap QOS for propagation at user-initiated <rdar://16681262&16998036>
		pp = _dispatch_user_initiated_priority;
	}
	return pp;
#else
	return 0;
#endif
}

// including maintenance
DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_is_background_thread(void)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pthread_priority_t pp = _dispatch_get_priority();
	pp &= ~_PTHREAD_PRIORITY_FLAGS_MASK;
	return pp && (pp <= _dispatch_background_priority);
#else
	return false;
#endif
}

#pragma mark -
#pragma mark dispatch_block_t

#ifdef __BLOCKS__

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_block_has_private_data(const dispatch_block_t block)
{
	extern void (*_dispatch_block_special_invoke)(void*);
	return (_dispatch_Block_invoke(block) == _dispatch_block_special_invoke);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_block_sync_should_enforce_qos_class(dispatch_block_flags_t flags)
{
	/*
	 * Generates better assembly than the actual readable test:
	 *	 (flags & ENFORCE_QOS_CLASS) || !(flags & INHERIT_QOS_FLAGS)
	 */
	flags &= DISPATCH_BLOCK_ENFORCE_QOS_CLASS | DISPATCH_BLOCK_INHERIT_QOS_CLASS;
	return flags != DISPATCH_BLOCK_INHERIT_QOS_CLASS;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_block_private_data_t
_dispatch_block_get_data(const dispatch_block_t db)
{
	if (!_dispatch_block_has_private_data(db)) {
		return NULL;
	}
	// Keep in sync with _dispatch_block_create implementation
	uint8_t *x = (uint8_t *)db;
	// x points to base of struct Block_layout
	x += sizeof(struct Block_layout);
	// x points to base of captured dispatch_block_private_data_s object
	dispatch_block_private_data_t dbpd = (dispatch_block_private_data_t)x;
	if (dbpd->dbpd_magic != DISPATCH_BLOCK_PRIVATE_DATA_MAGIC) {
		DISPATCH_CLIENT_CRASH(dbpd->dbpd_magic,
				"Corruption of dispatch block object");
	}
	return dbpd;
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_block_get_priority(const dispatch_block_t db)
{
	dispatch_block_private_data_t dbpd = _dispatch_block_get_data(db);
	return dbpd ? dbpd->dbpd_priority : 0;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_block_flags_t
_dispatch_block_get_flags(const dispatch_block_t db)
{
	dispatch_block_private_data_t dbpd = _dispatch_block_get_data(db);
	return dbpd ? dbpd->dbpd_flags : 0;
}

#endif

#pragma mark -
#pragma mark dispatch_continuation_t

DISPATCH_ALWAYS_INLINE
static inline dispatch_continuation_t
_dispatch_continuation_alloc_cacheonly(void)
{
	dispatch_continuation_t dc = (dispatch_continuation_t)
			_dispatch_thread_getspecific(dispatch_cache_key);
	if (likely(dc)) {
		_dispatch_thread_setspecific(dispatch_cache_key, dc->do_next);
	}
	return dc;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_continuation_t
_dispatch_continuation_alloc(void)
{
	dispatch_continuation_t dc =
			_dispatch_continuation_alloc_cacheonly();
	if (unlikely(!dc)) {
		return _dispatch_continuation_alloc_from_heap();
	}
	return dc;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_continuation_t
_dispatch_continuation_free_cacheonly(dispatch_continuation_t dc)
{
	dispatch_continuation_t prev_dc = (dispatch_continuation_t)
			_dispatch_thread_getspecific(dispatch_cache_key);
	int cnt = prev_dc ? prev_dc->dc_cache_cnt + 1 : 1;
	// Cap continuation cache
	if (unlikely(cnt > _dispatch_continuation_cache_limit)) {
		return dc;
	}
	dc->do_next = prev_dc;
	dc->dc_cache_cnt = cnt;
	_dispatch_thread_setspecific(dispatch_cache_key, dc);
	return NULL;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_free(dispatch_continuation_t dc)
{
	dc = _dispatch_continuation_free_cacheonly(dc);
	if (unlikely(dc)) {
		_dispatch_continuation_free_to_cache_limit(dc);
	}
}

#include "trace.h"

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_with_group_invoke(dispatch_continuation_t dc)
{
	struct dispatch_object_s *dou = dc->dc_data;
	unsigned long type = dx_type(dou);
	if (type == DISPATCH_GROUP_TYPE) {
		_dispatch_client_callout(dc->dc_ctxt, dc->dc_func);
		_dispatch_introspection_queue_item_complete(dou);
		dispatch_group_leave((dispatch_group_t)dou);
	} else {
		DISPATCH_INTERNAL_CRASH(dx_type(dou), "Unexpected object type");
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_invoke_inline(dispatch_object_t dou, voucher_t ov,
		dispatch_invoke_flags_t flags)
{
	dispatch_continuation_t dc = dou._dc, dc1;
	dispatch_invoke_with_autoreleasepool(flags, {
		uintptr_t dc_flags = dc->dc_flags;
		// Add the item back to the cache before calling the function. This
		// allows the 'hot' continuation to be used for a quick callback.
		//
		// The ccache version is per-thread.
		// Therefore, the object has not been reused yet.
		// This generates better assembly.
		_dispatch_continuation_voucher_adopt(dc, ov, dc_flags);
		if (dc_flags & DISPATCH_OBJ_CONSUME_BIT) {
			dc1 = _dispatch_continuation_free_cacheonly(dc);
		} else {
			dc1 = NULL;
		}
		if (unlikely(dc_flags & DISPATCH_OBJ_GROUP_BIT)) {
			_dispatch_continuation_with_group_invoke(dc);
		} else {
			_dispatch_client_callout(dc->dc_ctxt, dc->dc_func);
			_dispatch_introspection_queue_item_complete(dou);
		}
		if (unlikely(dc1)) {
			_dispatch_continuation_free_to_cache_limit(dc1);
		}
	});
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_continuation_pop_inline(dispatch_object_t dou, dispatch_queue_t dq,
		dispatch_invoke_flags_t flags)
{
	dispatch_pthread_root_queue_observer_hooks_t observer_hooks =
			_dispatch_get_pthread_root_queue_observer_hooks();
	if (observer_hooks) observer_hooks->queue_will_execute(dq);
	_dispatch_trace_continuation_pop(dq, dou);
	flags &= _DISPATCH_INVOKE_PROPAGATE_MASK;
	if (_dispatch_object_has_vtable(dou)) {
		dx_invoke(dou._do, flags);
	} else {
		voucher_t ov = dq->dq_override_voucher;
		_dispatch_continuation_invoke_inline(dou, ov, flags);
	}
	if (observer_hooks) observer_hooks->queue_did_execute(dq);
}

// used to forward the do_invoke of a continuation with a vtable to its real
// implementation.
#define _dispatch_continuation_pop_forwarded(dc, ov, dc_flags, ...) \
	({ \
		dispatch_continuation_t _dc = (dc), _dc1; \
		uintptr_t _dc_flags = (dc_flags); \
		_dispatch_continuation_voucher_adopt(_dc, ov, _dc_flags); \
		if (_dc_flags & DISPATCH_OBJ_CONSUME_BIT) { \
			_dc1 = _dispatch_continuation_free_cacheonly(_dc); \
		} else { \
			_dc1 = NULL; \
		} \
		__VA_ARGS__; \
		_dispatch_introspection_queue_item_complete(_dc); \
		if (unlikely(_dc1)) { \
			_dispatch_continuation_free_to_cache_limit(_dc1); \
		} \
	})

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_priority_set(dispatch_continuation_t dc,
		pthread_priority_t pp, dispatch_block_flags_t flags)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	if (likely(!(flags & DISPATCH_BLOCK_HAS_PRIORITY))) {
		pp = _dispatch_priority_propagate();
	}
	if (flags & DISPATCH_BLOCK_ENFORCE_QOS_CLASS) {
		pp |= _PTHREAD_PRIORITY_ENFORCE_FLAG;
	}
	dc->dc_priority = pp;
#else
	(void)dc; (void)pp; (void)flags;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_continuation_get_override_priority(dispatch_queue_t dq,
		dispatch_continuation_t dc)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pthread_priority_t p = dc->dc_priority & _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	bool enforce = dc->dc_priority & _PTHREAD_PRIORITY_ENFORCE_FLAG;
	pthread_priority_t dqp = dq->dq_priority & _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	bool defaultqueue = dq->dq_priority & _PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG;

	dispatch_assert(dc->dc_priority != DISPATCH_NO_PRIORITY);
	if (p && (enforce || !dqp || defaultqueue)) {
		return p;
	}
	return dqp;
#else
	(void)dq; (void)dc;
	return 0;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_init_f(dispatch_continuation_t dc,
		dispatch_queue_class_t dqu, void *ctxt, dispatch_function_t func,
		pthread_priority_t pp, dispatch_block_flags_t flags, uintptr_t dc_flags)
{
	dc->dc_flags = dc_flags;
	dc->dc_func = func;
	dc->dc_ctxt = ctxt;
	_dispatch_continuation_voucher_set(dc, dqu, flags);
	_dispatch_continuation_priority_set(dc, pp, flags);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_init(dispatch_continuation_t dc,
		dispatch_queue_class_t dqu, dispatch_block_t work,
		pthread_priority_t pp, dispatch_block_flags_t flags, uintptr_t dc_flags)
{
	dc->dc_flags = dc_flags | DISPATCH_OBJ_BLOCK_BIT;
	dc->dc_ctxt = _dispatch_Block_copy(work);
	_dispatch_continuation_priority_set(dc, pp, flags);

	if (unlikely(_dispatch_block_has_private_data(work))) {
		// always sets dc_func & dc_voucher
		// may update dc_priority & do_vtable
		return _dispatch_continuation_init_slow(dc, dqu, flags);
	}

	if (dc_flags & DISPATCH_OBJ_CONSUME_BIT) {
		dc->dc_func = _dispatch_call_block_and_release;
	} else {
		dc->dc_func = _dispatch_Block_invoke(work);
	}
	_dispatch_continuation_voucher_set(dc, dqu, flags);
}

#endif // DISPATCH_PURE_C

#endif /* __DISPATCH_INLINE_INTERNAL__ */
