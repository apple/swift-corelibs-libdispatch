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
_dispatch_client_callout3(void *ctxt, dispatch_mach_reason_t reason,
		dispatch_mach_msg_t dmsg, dispatch_mach_async_reply_callback_t f);
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
_dispatch_client_callout3(void *ctxt, dispatch_mach_reason_t reason,
		dispatch_mach_msg_t dmsg, dispatch_mach_async_reply_callback_t f)
{
	return f(ctxt, reason, dmsg);
}

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
_dispatch_object_is_queue(dispatch_object_t dou)
{
	return _dispatch_object_has_vtable(dou) && dx_vtable(dou._do)->do_push;
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
_dispatch_object_is_sync_waiter(dispatch_object_t dou)
{
	if (_dispatch_object_has_vtable(dou)) {
		return false;
	}
	return (dou._dc->dc_flags & DISPATCH_OBJ_SYNC_WAITER_BIT);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_object_is_sync_waiter_non_barrier(dispatch_object_t dou)
{
	if (_dispatch_object_has_vtable(dou)) {
		return false;
	}
	return ((dou._dc->dc_flags &
				(DISPATCH_OBJ_BARRIER_BIT | DISPATCH_OBJ_SYNC_WAITER_BIT)) ==
				(DISPATCH_OBJ_SYNC_WAITER_BIT));
}

DISPATCH_ALWAYS_INLINE
static inline _os_object_t
_os_object_retain_internal_n_inline(_os_object_t obj, int n)
{
	int ref_cnt = _os_object_refcnt_add(obj, n);
	if (unlikely(ref_cnt <= 0)) {
		_OS_OBJECT_CLIENT_CRASH("Resurrection of an object");
	}
	return obj;
}

DISPATCH_ALWAYS_INLINE
static inline void
_os_object_release_internal_n_no_dispose_inline(_os_object_t obj, int n)
{
	int ref_cnt = _os_object_refcnt_sub(obj, n);
	if (likely(ref_cnt >= 0)) {
		return;
	}
	_OS_OBJECT_CLIENT_CRASH("Over-release of an object");
}

DISPATCH_ALWAYS_INLINE
static inline void
_os_object_release_internal_n_inline(_os_object_t obj, int n)
{
	int ref_cnt = _os_object_refcnt_sub(obj, n);
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
	(void)_os_object_retain_internal_n_inline(dou._os_obj, 1);
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_retain_2(dispatch_object_t dou)
{
	(void)_os_object_retain_internal_n_inline(dou._os_obj, 2);
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_retain_n(dispatch_object_t dou, int n)
{
	(void)_os_object_retain_internal_n_inline(dou._os_obj, n);
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_release(dispatch_object_t dou)
{
	_os_object_release_internal_n_inline(dou._os_obj, 1);
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_release_2(dispatch_object_t dou)
{
	_os_object_release_internal_n_inline(dou._os_obj, 2);
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_release_n(dispatch_object_t dou, int n)
{
	_os_object_release_internal_n_inline(dou._os_obj, n);
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_release_no_dispose(dispatch_object_t dou)
{
	_os_object_release_internal_n_no_dispose_inline(dou._os_obj, 1);
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_release_2_no_dispose(dispatch_object_t dou)
{
	_os_object_release_internal_n_no_dispose_inline(dou._os_obj, 2);
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_release_tailcall(dispatch_object_t dou)
{
	_os_object_release_internal(dou._os_obj);
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_release_2_tailcall(dispatch_object_t dou)
{
	_os_object_release_internal_n(dou._os_obj, 2);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_retain_storage(dispatch_queue_t dq)
{
	int ref_cnt = os_atomic_inc2o(dq, dq_sref_cnt, relaxed);
	if (unlikely(ref_cnt <= 0)) {
		_OS_OBJECT_CLIENT_CRASH("Resurrection of an object");
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_release_storage(dispatch_queue_t dq)
{
	// this refcount only delays the _dispatch_object_dealloc() and there's no
	// need for visibility wrt to the allocation, the internal refcount already
	// gives us that, and the object becomes immutable after the last internal
	// refcount release.
	int ref_cnt = os_atomic_dec2o(dq, dq_sref_cnt, relaxed);
	if (unlikely(ref_cnt >= 0)) {
		return;
	}
	if (unlikely(ref_cnt < -1)) {
		_OS_OBJECT_CLIENT_CRASH("Over-release of an object");
	}
	dq->dq_state = 0xdead000000000000;
	_dispatch_object_dealloc(dq);
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
		dispatch_queue_t tq = dq->do_targetq;
		if (tq) {
			// redirections, dispatch_sync and dispatch_trysync_f may skip
			// frames, so we need to simulate seeing the missing links
			it->dtfi_queue = tq;
			if (dq == dtf->dtf_queue) {
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
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_frame_push_and_rebase(dispatch_thread_frame_t dtf,
		dispatch_queue_t dq, dispatch_thread_frame_t new_base)
{
	_dispatch_thread_frame_save_state(dtf);
	_dispatch_thread_setspecific_pair(dispatch_queue_key, dq,
			dispatch_frame_key, new_base);
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
		dispatch_qos_t qos, mach_port_t *ulock_addr)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	if (!_dispatch_set_qos_class_enabled) return 0;
	return _pthread_workqueue_override_start_direct_check_owner(thread,
			_dispatch_qos_to_pp(qos), ulock_addr);
#else
	(void)thread; (void)qos; (void)ulock_addr;
	return 0;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_wqthread_override_start(mach_port_t thread, dispatch_qos_t qos)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	if (!_dispatch_set_qos_class_enabled) return;
	(void)_pthread_workqueue_override_start_direct(thread,
			_dispatch_qos_to_pp(qos));
#else
	(void)thread; (void)qos;
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
_dispatch_queue_is_legacy(dispatch_queue_t dq)
{
	return _dispatch_queue_atomic_flags(dq) & DQF_LEGACY;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_wlh_retain(dispatch_wlh_t wlh)
{
	if (wlh && wlh != DISPATCH_WLH_ANON) {
		_dispatch_queue_retain_storage((dispatch_queue_t)wlh);
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_wlh_release(dispatch_wlh_t wlh)
{
	if (wlh && wlh != DISPATCH_WLH_ANON) {
		_dispatch_queue_release_storage((dispatch_queue_t)wlh);
	}
}

#define DISPATCH_WLH_STORAGE_REF 1ul

DISPATCH_ALWAYS_INLINE DISPATCH_PURE
static inline dispatch_wlh_t
_dispatch_get_wlh(void)
{
	return _dispatch_thread_getspecific(dispatch_wlh_key);
}

DISPATCH_ALWAYS_INLINE DISPATCH_PURE
static inline dispatch_wlh_t
_dispatch_get_wlh_reference(void)
{
	dispatch_wlh_t wlh = _dispatch_thread_getspecific(dispatch_wlh_key);
	if (wlh != DISPATCH_WLH_ANON) {
		wlh = (dispatch_wlh_t)((uintptr_t)wlh & ~DISPATCH_WLH_STORAGE_REF);
	}
	return wlh;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_adopt_wlh_anon_recurse(void)
{
	dispatch_wlh_t cur_wlh = _dispatch_get_wlh_reference();
	if (cur_wlh == DISPATCH_WLH_ANON) return false;
	_dispatch_debug("wlh[anon]: set current (releasing %p)", cur_wlh);
	_dispatch_wlh_release(cur_wlh);
	_dispatch_thread_setspecific(dispatch_wlh_key, (void *)DISPATCH_WLH_ANON);
	return true;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_adopt_wlh_anon(void)
{
	if (unlikely(!_dispatch_adopt_wlh_anon_recurse())) {
		DISPATCH_INTERNAL_CRASH(0, "Lingering DISPATCH_WLH_ANON");
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_adopt_wlh(dispatch_wlh_t wlh)
{
	dispatch_wlh_t cur_wlh = _dispatch_get_wlh_reference();
	_dispatch_debug("wlh[%p]: adopt current (releasing %p)", wlh, cur_wlh);
	if (cur_wlh == DISPATCH_WLH_ANON) {
		DISPATCH_INTERNAL_CRASH(0, "Lingering DISPATCH_WLH_ANON");
	}
	if (cur_wlh != wlh) {
		dispatch_assert(wlh);
		_dispatch_wlh_release(cur_wlh);
		_dispatch_wlh_retain(wlh);
	}
	_dispatch_thread_setspecific(dispatch_wlh_key, (void *)wlh);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_preserve_wlh_storage_reference(dispatch_wlh_t wlh)
{
	dispatch_assert(wlh != DISPATCH_WLH_ANON);
	dispatch_assert(wlh == _dispatch_get_wlh());
	_dispatch_thread_setspecific(dispatch_wlh_key,
			(void *)((uintptr_t)wlh | DISPATCH_WLH_STORAGE_REF));
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_reset_wlh(void)
{
	dispatch_assert(_dispatch_get_wlh() == DISPATCH_WLH_ANON);
	_dispatch_debug("wlh[anon]: clear current");
	_dispatch_thread_setspecific(dispatch_wlh_key, NULL);
	_dispatch_clear_return_to_kernel();
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_wlh_should_poll_unote(dispatch_unote_t du)
{
	if (likely(_dispatch_needs_to_return_to_kernel())) {
		dispatch_wlh_t wlh = _dispatch_get_wlh();
		return wlh != DISPATCH_WLH_ANON && du._du->du_wlh == wlh;
	}
	return false;
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
static inline int32_t
_dq_state_extract_width_bits(uint64_t dq_state)
{
	dq_state &= DISPATCH_QUEUE_WIDTH_MASK;
	return (int32_t)(dq_state >> DISPATCH_QUEUE_WIDTH_SHIFT);
}

DISPATCH_ALWAYS_INLINE
static inline int32_t
_dq_state_available_width(uint64_t dq_state)
{
	int32_t full = DISPATCH_QUEUE_WIDTH_FULL;
	if (likely(!(dq_state & DISPATCH_QUEUE_WIDTH_FULL_BIT))) {
		return full - _dq_state_extract_width_bits(dq_state);
	}
	return 0;
}

DISPATCH_ALWAYS_INLINE
static inline int32_t
_dq_state_used_width(uint64_t dq_state, uint16_t dq_width)
{
	int32_t full = DISPATCH_QUEUE_WIDTH_FULL;
	int32_t width = _dq_state_extract_width_bits(dq_state);

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
#define DISPATCH_QUEUE_IS_SUSPENDED(x) \
		_dq_state_is_suspended(os_atomic_load2o(x, dq_state, relaxed))

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
_dq_state_is_base_wlh(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_ROLE_BASE_WLH;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_is_base_anon(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_ROLE_BASE_ANON;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_is_inner_queue(uint64_t dq_state)
{
	return (dq_state & DISPATCH_QUEUE_ROLE_MASK) == DISPATCH_QUEUE_ROLE_INNER;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_is_enqueued(uint64_t dq_state)
{
	return dq_state & (DISPATCH_QUEUE_ENQUEUED|DISPATCH_QUEUE_ENQUEUED_ON_MGR);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_is_enqueued_on_target(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_ENQUEUED;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_is_enqueued_on_manager(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_ENQUEUED_ON_MGR;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_in_sync_transfer(uint64_t dq_state)
{
	return dq_state & DISPATCH_QUEUE_SYNC_TRANSFER;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_received_override(uint64_t dq_state)
{
	return _dq_state_is_base_anon(dq_state) &&
			(dq_state & DISPATCH_QUEUE_RECEIVED_OVERRIDE);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_received_sync_wait(uint64_t dq_state)
{
	return _dq_state_is_base_wlh(dq_state) &&
			(dq_state & DISPATCH_QUEUE_RECEIVED_SYNC_WAIT);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dq_state_max_qos(uint64_t dq_state)
{
	dq_state &= DISPATCH_QUEUE_MAX_QOS_MASK;
	return (dispatch_qos_t)(dq_state >> DISPATCH_QUEUE_MAX_QOS_SHIFT);
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dq_state_from_qos(dispatch_qos_t qos)
{
	return (uint64_t)(qos) << DISPATCH_QUEUE_MAX_QOS_SHIFT;
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dq_state_merge_qos(uint64_t dq_state, dispatch_qos_t qos)
{
	uint64_t qos_bits = _dq_state_from_qos(qos);
	if ((dq_state & DISPATCH_QUEUE_MAX_QOS_MASK) < qos_bits) {
		dq_state &= ~DISPATCH_QUEUE_MAX_QOS_MASK;
		dq_state |= qos_bits;
		if (unlikely(_dq_state_is_base_anon(dq_state))) {
			dq_state |= DISPATCH_QUEUE_RECEIVED_OVERRIDE;
		}
	}
	return dq_state;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_tid
_dq_state_drain_owner(uint64_t dq_state)
{
	return _dispatch_lock_owner((dispatch_lock)dq_state);
}
#define DISPATCH_QUEUE_DRAIN_OWNER(dq) \
	_dq_state_drain_owner(os_atomic_load2o(dq, dq_state, relaxed))

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_drain_locked_by(uint64_t dq_state, dispatch_tid tid)
{
	return _dispatch_lock_is_locked_by((dispatch_lock)dq_state, tid);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_drain_locked_by_self(uint64_t dq_state)
{
	return _dispatch_lock_is_locked_by_self((dispatch_lock)dq_state);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_drain_locked(uint64_t dq_state)
{
	return _dispatch_lock_is_locked((dispatch_lock)dq_state);
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
_dq_state_should_override(uint64_t dq_state)
{
	if (_dq_state_is_suspended(dq_state) ||
			_dq_state_is_enqueued_on_manager(dq_state)) {
		return false;
	}
	if (_dq_state_is_enqueued_on_target(dq_state)) {
		return true;
	}
	if (_dq_state_is_base_wlh(dq_state)) {
		return false;
	}
	return _dq_state_drain_locked(dq_state);
}


#endif // __cplusplus
#pragma mark -
#pragma mark dispatch_queue_t state machine

static inline pthread_priority_t _dispatch_get_priority(void);
static inline dispatch_priority_t _dispatch_get_basepri(void);
static inline dispatch_qos_t _dispatch_get_basepri_override_qos_floor(void);
static inline void _dispatch_set_basepri_override_qos(dispatch_qos_t qos);
static inline void _dispatch_reset_basepri(dispatch_priority_t dbp);
static inline dispatch_priority_t _dispatch_set_basepri(dispatch_priority_t dbp);
static inline bool _dispatch_queue_need_override_retain(
		dispatch_queue_class_t dqu, dispatch_qos_t qos);

#if DISPATCH_PURE_C

// Note to later developers: ensure that any initialization changes are
// made for statically allocated queues (i.e. _dispatch_main_q).
static inline void
_dispatch_queue_init(dispatch_queue_t dq, dispatch_queue_flags_t dqf,
		uint16_t width, uint64_t initial_state_bits)
{
	uint64_t dq_state = DISPATCH_QUEUE_STATE_INIT_VALUE(width);

	dispatch_assert((initial_state_bits & ~(DISPATCH_QUEUE_ROLE_MASK |
			DISPATCH_QUEUE_INACTIVE)) == 0);

	if (initial_state_bits & DISPATCH_QUEUE_INACTIVE) {
		dq_state |= DISPATCH_QUEUE_INACTIVE + DISPATCH_QUEUE_NEEDS_ACTIVATION;
		dq->do_ref_cnt += 2; // rdar://8181908 see _dispatch_queue_resume
	}

	dq_state |= (initial_state_bits & DISPATCH_QUEUE_ROLE_MASK);
	dq->do_next = (struct dispatch_queue_s *)DISPATCH_OBJECT_LISTLESS;
	dqf |= DQF_WIDTH(width);
	os_atomic_store2o(dq, dq_atomic_flags, dqf, relaxed);
	dq->dq_state = dq_state;
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
	uint64_t old_state, new_state;

	(void)os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, relaxed, {
		if (unlikely(!_dq_state_is_inactive(old_state))) {
			os_atomic_rmw_loop_give_up(return false);
		}
		new_state = old_state + DISPATCH_QUEUE_SUSPEND_INTERVAL;
	});
	if (unlikely(!_dq_state_is_suspended(old_state) ||
			_dq_state_has_side_suspend_cnt(old_state))) {
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

DISPATCH_ALWAYS_INLINE
static inline bool
_dq_state_needs_lock_override(uint64_t dq_state, dispatch_qos_t qos)
{
	return _dq_state_is_base_anon(dq_state) &&
			qos < _dq_state_max_qos(dq_state);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dispatch_queue_override_self(uint64_t dq_state)
{
	dispatch_qos_t qos = _dq_state_max_qos(dq_state);
	_dispatch_wqthread_override_start(_dispatch_tid_self(), qos);
	// ensure that the root queue sees
	// that this thread was overridden.
	_dispatch_set_basepri_override_qos(qos);
	return qos;
}

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline uint64_t
_dispatch_queue_drain_try_lock(dispatch_queue_t dq,
		dispatch_invoke_flags_t flags)
{
	uint64_t pending_barrier_width =
			(dq->dq_width - 1) * DISPATCH_QUEUE_WIDTH_INTERVAL;
	uint64_t set_owner_and_set_full_width =
			_dispatch_lock_value_for_self() | DISPATCH_QUEUE_WIDTH_FULL_BIT;
	uint64_t lock_fail_mask, old_state, new_state, dequeue_mask;

	// same as !_dq_state_is_runnable()
	lock_fail_mask  = ~(DISPATCH_QUEUE_WIDTH_FULL_BIT - 1);
	// same as _dq_state_drain_locked()
	lock_fail_mask |= DISPATCH_QUEUE_DRAIN_OWNER_MASK;

	if (flags & DISPATCH_INVOKE_STEALING) {
		lock_fail_mask |= DISPATCH_QUEUE_ENQUEUED_ON_MGR;
		dequeue_mask = 0;
	} else if (flags & DISPATCH_INVOKE_MANAGER_DRAIN) {
		dequeue_mask = DISPATCH_QUEUE_ENQUEUED_ON_MGR;
	} else {
		lock_fail_mask |= DISPATCH_QUEUE_ENQUEUED_ON_MGR;
		dequeue_mask = DISPATCH_QUEUE_ENQUEUED;
	}
	dispatch_assert(!(flags & DISPATCH_INVOKE_WLH));

	dispatch_qos_t oq_floor = _dispatch_get_basepri_override_qos_floor();
retry:
	os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, acquire, {
		new_state = old_state;
		if (likely(!(old_state & lock_fail_mask))) {
			if (unlikely(_dq_state_needs_lock_override(old_state, oq_floor))) {
				os_atomic_rmw_loop_give_up({
					oq_floor = _dispatch_queue_override_self(old_state);
					goto retry;
				});
			}
			//
			// Only keep the HAS_WAITER, MAX_QOS and ENQUEUED bits
			// In particular acquiring the drain lock clears the DIRTY and
			// RECEIVED_OVERRIDE bits.
			//
			new_state &= DISPATCH_QUEUE_DRAIN_PRESERVED_BITS_MASK;
			new_state |= set_owner_and_set_full_width;
			if (_dq_state_has_pending_barrier(old_state) ||
					old_state + pending_barrier_width <
					DISPATCH_QUEUE_WIDTH_FULL_BIT) {
				new_state |= DISPATCH_QUEUE_IN_BARRIER;
			}
		} else if (dequeue_mask) {
			// dequeue_mask is in a register, xor yields better assembly
			new_state ^= dequeue_mask;
		} else {
			os_atomic_rmw_loop_give_up(break);
		}
	});

	dispatch_assert((old_state & dequeue_mask) == dequeue_mask);
	if (likely(!(old_state & lock_fail_mask))) {
		new_state &= DISPATCH_QUEUE_IN_BARRIER | DISPATCH_QUEUE_WIDTH_FULL_BIT |
				dequeue_mask;
		old_state &= DISPATCH_QUEUE_WIDTH_MASK;
		return new_state - old_state;
	}
	return 0;
}

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline bool
_dispatch_queue_drain_try_lock_wlh(dispatch_queue_t dq, uint64_t *dq_state)
{
	uint64_t old_state, new_state;
	uint64_t lock_bits = _dispatch_lock_value_for_self() |
			DISPATCH_QUEUE_WIDTH_FULL_BIT | DISPATCH_QUEUE_IN_BARRIER;

	os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, acquire, {
		new_state = old_state;
		if (unlikely(_dq_state_is_suspended(old_state))) {
			new_state &= ~DISPATCH_QUEUE_ENQUEUED;
		} else if (unlikely(_dq_state_drain_locked(old_state))) {
			os_atomic_rmw_loop_give_up(break);
		} else {
			new_state &= DISPATCH_QUEUE_DRAIN_PRESERVED_BITS_MASK;
			new_state |= lock_bits;
		}
	});
	if (unlikely(!_dq_state_is_base_wlh(old_state) ||
			!_dq_state_is_enqueued_on_target(old_state) ||
			_dq_state_is_enqueued_on_manager(old_state))) {
#if !__LP64__
		old_state >>= 32;
#endif
		DISPATCH_INTERNAL_CRASH(old_state, "Invalid wlh state");
	}

	if (dq_state) *dq_state = new_state;
	return !_dq_state_is_suspended(old_state) &&
			!_dq_state_drain_locked(old_state);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_mgr_lock(dispatch_queue_t dq)
{
	uint64_t old_state, new_state, set_owner_and_set_full_width =
			_dispatch_lock_value_for_self() | DISPATCH_QUEUE_SERIAL_DRAIN_OWNED;

	os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, acquire, {
		new_state = old_state;
		if (unlikely(!_dq_state_is_runnable(old_state) ||
				_dq_state_drain_locked(old_state))) {
			DISPATCH_INTERNAL_CRASH((uintptr_t)old_state,
					"Locking the manager should not fail");
		}
		new_state &= DISPATCH_QUEUE_DRAIN_PRESERVED_BITS_MASK;
		new_state |= set_owner_and_set_full_width;
	});
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_mgr_unlock(dispatch_queue_t dq)
{
	uint64_t old_state, new_state;
	os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, release, {
		new_state = old_state - DISPATCH_QUEUE_SERIAL_DRAIN_OWNED;
		new_state &= ~DISPATCH_QUEUE_DRAIN_UNLOCK_MASK;
		new_state &= ~DISPATCH_QUEUE_MAX_QOS_MASK;
	});
	return _dq_state_is_dirty(old_state);
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
_dispatch_queue_try_acquire_barrier_sync_and_suspend(dispatch_queue_t dq,
		uint32_t tid, uint64_t suspend_count)
{
	uint64_t init  = DISPATCH_QUEUE_STATE_INIT_VALUE(dq->dq_width);
	uint64_t value = DISPATCH_QUEUE_WIDTH_FULL_BIT | DISPATCH_QUEUE_IN_BARRIER |
			_dispatch_lock_value_from_tid(tid) |
			(suspend_count * DISPATCH_QUEUE_SUSPEND_INTERVAL);
	uint64_t old_state, new_state;

	return os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, acquire, {
		uint64_t role = old_state & DISPATCH_QUEUE_ROLE_MASK;
		if (old_state != (init | role)) {
			os_atomic_rmw_loop_give_up(break);
		}
		new_state = value | role;
	});
}

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline bool
_dispatch_queue_try_acquire_barrier_sync(dispatch_queue_t dq, uint32_t tid)
{
	return _dispatch_queue_try_acquire_barrier_sync_and_suspend(dq, tid, 0);
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
	uint64_t old_state, new_state;

	// <rdar://problem/24738102&24743140> reserving non barrier width
	// doesn't fail if only the ENQUEUED bit is set (unlike its barrier width
	// equivalent), so we have to check that this thread hasn't enqueued
	// anything ahead of this call or we can break ordering
	if (unlikely(dq->dq_items_tail)) {
		return false;
	}

	return os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, relaxed, {
		if (unlikely(!_dq_state_is_sync_runnable(old_state)) ||
				_dq_state_is_dirty(old_state) ||
				_dq_state_has_pending_barrier(old_state)) {
			os_atomic_rmw_loop_give_up(return false);
		}
		new_state = old_state + DISPATCH_QUEUE_WIDTH_INTERVAL;
	});
}

/* Used by _dispatch_apply_redirect
 *
 * Try to acquire at most da_width and returns what could be acquired,
 * possibly 0
 */
DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline int32_t
_dispatch_queue_try_reserve_apply_width(dispatch_queue_t dq, int32_t da_width)
{
	uint64_t old_state, new_state;
	int32_t width;

	(void)os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, relaxed, {
		width = (int32_t)_dq_state_available_width(old_state);
		if (unlikely(!width)) {
			os_atomic_rmw_loop_give_up(return 0);
		}
		if (width > da_width) {
			width = da_width;
		}
		new_state = old_state + (uint64_t)width * DISPATCH_QUEUE_WIDTH_INTERVAL;
	});
	return width;
}

/* Used by _dispatch_apply_redirect
 *
 * Release width acquired by _dispatch_queue_try_acquire_width
 */
DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_relinquish_width(dispatch_queue_t dq, int32_t da_width)
{
	(void)os_atomic_sub2o(dq, dq_state,
			(uint64_t)da_width * DISPATCH_QUEUE_WIDTH_INTERVAL, relaxed);
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
	uint64_t old_state, new_state;

	return os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, acquire, {
		if (unlikely(!_dq_state_is_runnable(old_state) ||
				_dq_state_is_dirty(old_state) ||
				_dq_state_has_pending_barrier(old_state))) {
			os_atomic_rmw_loop_give_up(return false);
		}
		new_state = old_state + DISPATCH_QUEUE_WIDTH_INTERVAL;
	});
}

/* Used by concurrent drain
 *
 * Either acquires the full barrier width, in which case the Final state is:
 *   { ib:1 qf:1 pb:0 d:0 }
 * Or if there isn't enough width prepare the queue with the PENDING_BARRIER bit
 *   { ib:0 pb:1 d:0}
 *
 * This always clears the dirty bit as we know for sure we shouldn't reevaluate
 * the state machine here
 */
DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline bool
_dispatch_queue_try_upgrade_full_width(dispatch_queue_t dq, uint64_t owned)
{
	uint64_t old_state, new_state;
	uint64_t pending_barrier_width = DISPATCH_QUEUE_PENDING_BARRIER +
			(dq->dq_width - 1) * DISPATCH_QUEUE_WIDTH_INTERVAL;

	os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, acquire, {
		new_state = old_state - owned;
		if (likely(!_dq_state_has_pending_barrier(old_state))) {
			new_state += pending_barrier_width;
		}
		if (likely(_dq_state_is_runnable(new_state))) {
			new_state += DISPATCH_QUEUE_WIDTH_INTERVAL;
			new_state += DISPATCH_QUEUE_IN_BARRIER;
			new_state -= DISPATCH_QUEUE_PENDING_BARRIER;
		}
		new_state &= ~DISPATCH_QUEUE_DIRTY;
	});
	return new_state & DISPATCH_QUEUE_IN_BARRIER;
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

	if (unlikely(dq->dq_width > 1)) {
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
 * Successful unlock forces { dl:0, d:!done, qo:0 } and gives back `owned`
 */
DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline bool
_dispatch_queue_drain_try_unlock(dispatch_queue_t dq, uint64_t owned, bool done)
{
	uint64_t old_state, new_state;

	os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, release, {
		new_state  = old_state - owned;
		new_state &= ~DISPATCH_QUEUE_DRAIN_UNLOCK_MASK;
		if (unlikely(_dq_state_is_suspended(old_state))) {
			// nothing to do
		} else if (unlikely(_dq_state_is_dirty(old_state))) {
			os_atomic_rmw_loop_give_up({
				// just renew the drain lock with an acquire barrier, to see
				// what the enqueuer that set DIRTY has done.
				// the xor generates better assembly as DISPATCH_QUEUE_DIRTY
				// is already in a register
				os_atomic_xor2o(dq, dq_state, DISPATCH_QUEUE_DIRTY, acquire);
				return false;
			});
		} else if (likely(done)) {
			new_state &= ~DISPATCH_QUEUE_MAX_QOS_MASK;
		} else {
			new_state |= DISPATCH_QUEUE_DIRTY;
		}
	});

	if (_dq_state_received_override(old_state)) {
		// Ensure that the root queue sees that this thread was overridden.
		_dispatch_set_basepri_override_qos(_dq_state_max_qos(old_state));
	}
	return true;
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
		if (likely(_prev)) { \
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

#define os_mpsc_get_head(q, _ns) \
		_dispatch_wait_until(os_atomic_load2o(q, _ns##_head, dependency))

#define os_mpsc_get_next(_n, _o_next) \
		_dispatch_wait_until(os_atomic_load2o(_n, _o_next, dependency))

#define os_mpsc_pop_head(q, _ns, head, _o_next)  ({ \
		typeof(q) _q = (q); \
		os_mpsc_node_type(_q, _ns) _head = (head), _n; \
		_n = os_atomic_load2o(_head, _o_next, dependency); \
		os_atomic_store2o(_q, _ns##_head, _n, relaxed); \
		/* 22708742: set tail to NULL with release, so that NULL write */ \
		/* to head above doesn't clobber head from concurrent enqueuer */ \
		if (unlikely(!_n && \
				!os_atomic_cmpxchg2o(_q, _ns##_tail, _head, NULL, release))) { \
			_n = os_mpsc_get_next(_head, _o_next); \
			os_atomic_store2o(_q, _ns##_head, _n, relaxed); \
		} \
		_n; \
	})

#define os_mpsc_undo_pop_head(q, _ns, head, next, _o_next)  ({ \
		typeof(q) _q = (q); \
		os_mpsc_node_type(_q, _ns) _head = (head), _n = (next); \
		if (unlikely(!_n && \
				!os_atomic_cmpxchg2o(_q, _ns##_tail, NULL, _head, relaxed))) { \
			_n = os_mpsc_get_head(q, _ns); \
			os_atomic_store2o(_head, _o_next, _n, relaxed); \
		} \
		os_atomic_store2o(_q, _ns##_head, _head, relaxed); \
	})

#define os_mpsc_capture_snapshot(q, _ns, tail)  ({ \
		typeof(q) _q = (q); \
		os_mpsc_node_type(_q, _ns) _head = os_mpsc_get_head(q, _ns); \
		os_atomic_store2o(_q, _ns##_head, NULL, relaxed); \
		/* 22708742: set tail to NULL with release, so that NULL write */ \
		/* to head above doesn't clobber head from concurrent enqueuer */ \
		*(tail) = os_atomic_xchg2o(_q, _ns##_tail, NULL, release); \
		_head; \
	})

#define os_mpsc_pop_snapshot_head(head, tail, _o_next) ({ \
		os_unqualified_pointer_type(head) _head = (head), _n = NULL; \
		if (_head != (tail)) { \
			_n = os_mpsc_get_next(_head, _o_next); \
		}; \
		_n; })

#define os_mpsc_prepend(q, _ns, head, tail, _o_next)  ({ \
		typeof(q) _q = (q); \
		os_mpsc_node_type(_q, _ns) _head = (head), _tail = (tail), _n; \
		os_atomic_store2o(_tail, _o_next, NULL, relaxed); \
		if (unlikely(!os_atomic_cmpxchg2o(_q, _ns##_tail, NULL, _tail, release))) { \
			_n = os_mpsc_get_head(q, _ns); \
			os_atomic_store2o(_tail, _o_next, _n, relaxed); \
		} \
		os_atomic_store2o(_q, _ns##_head, _head, relaxed); \
	})

#pragma mark -
#pragma mark dispatch_queue_t tq lock

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_sidelock_trylock(dispatch_queue_t dq, dispatch_qos_t qos)
{
	dispatch_tid owner;
	if (_dispatch_unfair_lock_trylock(&dq->dq_sidelock, &owner)) {
		return true;
	}
	_dispatch_wqthread_override_start_check_owner(owner, qos,
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
	// Since we don't know which override QoS was used, use MAINTENANCE
	// as a marker for _dispatch_reset_basepri_override()
	_dispatch_set_basepri_override_qos(DISPATCH_QOS_MAINTENANCE);
	return false;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_sidelock_unlock(dispatch_queue_t dq)
{
	if (_dispatch_unfair_lock_unlock_had_failed_trylock(&dq->dq_sidelock)) {
		// Ensure that the root queue sees that this thread was overridden.
		// Since we don't know which override QoS was used, use MAINTENANCE
		// as a marker for _dispatch_reset_basepri_override()
		_dispatch_set_basepri_override_qos(DISPATCH_QOS_MAINTENANCE);
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
		struct dispatch_object_s *head)
{
	os_mpsc_push_update_head(dq, dq_items, head);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_root_queue_push_inline(dispatch_queue_t dq, dispatch_object_t _head,
		dispatch_object_t _tail, int n)
{
	struct dispatch_object_s *head = _head._do, *tail = _tail._do;
	if (unlikely(_dispatch_queue_push_update_tail_list(dq, head, tail))) {
		_dispatch_queue_push_update_head(dq, head);
		return _dispatch_global_queue_poke(dq, n, 0);
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_push_inline(dispatch_queue_t dq, dispatch_object_t _tail,
		dispatch_qos_t qos)
{
	struct dispatch_object_s *tail = _tail._do;
	dispatch_wakeup_flags_t flags = 0;
	// If we are going to call dx_wakeup(), the queue must be retained before
	// the item we're pushing can be dequeued, which means:
	// - before we exchange the tail if we may have to override
	// - before we set the head if we made the queue non empty.
	// Otherwise, if preempted between one of these and the call to dx_wakeup()
	// the blocks submitted to the queue may release the last reference to the
	// queue when invoked by _dispatch_queue_drain. <rdar://problem/6932776>
	bool overriding = _dispatch_queue_need_override_retain(dq, qos);
	if (unlikely(_dispatch_queue_push_update_tail(dq, tail))) {
		if (!overriding) _dispatch_retain_2(dq->_as_os_obj);
		_dispatch_queue_push_update_head(dq, tail);
		flags = DISPATCH_WAKEUP_CONSUME_2 | DISPATCH_WAKEUP_MAKE_DIRTY;
	} else if (overriding) {
		flags = DISPATCH_WAKEUP_CONSUME_2;
	} else {
		return;
	}
	return dx_wakeup(dq, qos, flags);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_push_queue(dispatch_queue_t tq, dispatch_queue_t dq,
		uint64_t dq_state)
{
	return dx_push(tq, dq, _dq_state_max_qos(dq_state));
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_priority_t
_dispatch_root_queue_identity_assume(dispatch_queue_t assumed_rq)
{
	dispatch_priority_t old_dbp = _dispatch_get_basepri();
	dispatch_assert(dx_hastypeflag(assumed_rq, QUEUE_ROOT));
	_dispatch_reset_basepri(assumed_rq->dq_priority);
	_dispatch_queue_set_current(assumed_rq);
	return old_dbp;
}

typedef dispatch_queue_wakeup_target_t
_dispatch_queue_class_invoke_handler_t(dispatch_object_t,
		dispatch_invoke_context_t dic, dispatch_invoke_flags_t,
		uint64_t *owned);

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_class_invoke(dispatch_object_t dou,
		dispatch_invoke_context_t dic, dispatch_invoke_flags_t flags,
		dispatch_invoke_flags_t const_restrict_flags,
		_dispatch_queue_class_invoke_handler_t invoke)
{
	dispatch_queue_t dq = dou._dq;
	dispatch_queue_wakeup_target_t tq = DISPATCH_QUEUE_WAKEUP_NONE;
	bool owning = !(flags & DISPATCH_INVOKE_STEALING);
	uint64_t owned = 0;

	// When called from a plain _dispatch_queue_drain:
	//   overriding = false
	//   owning = true
	//
	// When called from an override continuation:
	//   overriding = true
	//   owning depends on whether the override embedded the queue or steals

	if (!(flags & (DISPATCH_INVOKE_STEALING | DISPATCH_INVOKE_WLH))) {
		dq->do_next = DISPATCH_OBJECT_LISTLESS;
	}
	flags |= const_restrict_flags;
	if (likely(flags & DISPATCH_INVOKE_WLH)) {
		owned = DISPATCH_QUEUE_SERIAL_DRAIN_OWNED | DISPATCH_QUEUE_ENQUEUED;
	} else {
		owned = _dispatch_queue_drain_try_lock(dq, flags);
	}
	if (likely(owned)) {
		dispatch_priority_t old_dbp;
		if (!(flags & DISPATCH_INVOKE_MANAGER_DRAIN)) {
			old_dbp = _dispatch_set_basepri(dq->dq_priority);
		} else {
			old_dbp = 0;
		}

		flags = _dispatch_queue_merge_autorelease_frequency(dq, flags);
attempt_running_slow_head:
#if DISPATCH_COCOA_COMPAT
		if ((flags & DISPATCH_INVOKE_WLH) &&
				!(flags & DISPATCH_INVOKE_AUTORELEASE_ALWAYS)) {
			_dispatch_last_resort_autorelease_pool_push(dic);
		}
#endif // DISPATCH_COCOA_COMPAT
		tq = invoke(dq, dic, flags, &owned);
#if DISPATCH_COCOA_COMPAT
		if ((flags & DISPATCH_INVOKE_WLH) &&
				!(flags & DISPATCH_INVOKE_AUTORELEASE_ALWAYS)) {
			dispatch_thread_frame_s dtf;
			_dispatch_thread_frame_push(&dtf, dq);
			_dispatch_last_resort_autorelease_pool_pop(dic);
			_dispatch_thread_frame_pop(&dtf);
		}
#endif // DISPATCH_COCOA_COMPAT
		dispatch_assert(tq != DISPATCH_QUEUE_WAKEUP_TARGET);
		if (unlikely(tq != DISPATCH_QUEUE_WAKEUP_NONE &&
				tq != DISPATCH_QUEUE_WAKEUP_WAIT_FOR_EVENT)) {
			// Either dc is set, which is a deferred invoke case
			//
			// or only tq is and it means a reenqueue is required, because of:
			// a retarget, a suspension, or a width change.
			//
			// In both cases, we want to bypass the check for DIRTY.
			// That may cause us to leave DIRTY in place but all drain lock
			// acquirers clear it
		} else if (!_dispatch_queue_drain_try_unlock(dq, owned,
				tq == DISPATCH_QUEUE_WAKEUP_NONE)) {
			tq = _dispatch_queue_get_current();
			if (dx_hastypeflag(tq, QUEUE_ROOT) || !owning) {
				goto attempt_running_slow_head;
			}
			DISPATCH_COMPILER_CAN_ASSUME(tq != DISPATCH_QUEUE_WAKEUP_NONE);
		} else {
			owned = 0;
			tq = NULL;
		}
		if (!(flags & DISPATCH_INVOKE_MANAGER_DRAIN)) {
			_dispatch_reset_basepri(old_dbp);
		}
	}
	if (likely(owning)) {
		_dispatch_introspection_queue_item_complete(dq);
	}

	if (tq) {
		if (const_restrict_flags & DISPATCH_INVOKE_DISALLOW_SYNC_WAITERS) {
			dispatch_assert(dic->dic_deferred == NULL);
		} else if (dic->dic_deferred) {
			return _dispatch_queue_drain_sync_waiter(dq, dic,
					flags, owned);
		}

		uint64_t old_state, new_state, enqueued = DISPATCH_QUEUE_ENQUEUED;
		if (tq == DISPATCH_QUEUE_WAKEUP_MGR) {
			enqueued = DISPATCH_QUEUE_ENQUEUED_ON_MGR;
		}
		os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, release, {
			new_state  = old_state - owned;
			new_state &= ~DISPATCH_QUEUE_DRAIN_UNLOCK_MASK;
			new_state |= DISPATCH_QUEUE_DIRTY;
			if (_dq_state_is_runnable(new_state) &&
					!_dq_state_is_enqueued(new_state)) {
				// drain was not interupted for suspension
				// we will reenqueue right away, just put ENQUEUED back
				new_state |= enqueued;
			}
		});
		old_state -= owned;
		if (_dq_state_received_override(old_state)) {
			// Ensure that the root queue sees that this thread was overridden.
			_dispatch_set_basepri_override_qos(_dq_state_max_qos(new_state));
		}
		if ((old_state ^ new_state) & enqueued) {
			dispatch_assert(_dq_state_is_enqueued(new_state));
			return _dispatch_queue_push_queue(tq, dq, new_state);
		}
	}

	_dispatch_release_2_tailcall(dq);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_class_probe(dispatch_queue_class_t dqu)
{
	struct dispatch_object_s *tail;
	// seq_cst wrt atomic store to dq_state <rdar://problem/14637483>
	// seq_cst wrt atomic store to dq_flags <rdar://problem/22623242>
	tail = os_atomic_load2o(dqu._oq, oq_items_tail, ordered);
	return unlikely(tail != NULL);
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
_dispatch_get_root_queue(dispatch_qos_t qos, bool overcommit)
{
	if (unlikely(qos == DISPATCH_QOS_UNSPECIFIED || qos > DISPATCH_QOS_MAX)) {
		DISPATCH_CLIENT_CRASH(qos, "Corrupted priority");
	}
	return &_dispatch_root_queues[2 * (qos - 1) + overcommit];
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_set_bound_thread(dispatch_queue_t dq)
{
	// Tag thread-bound queues with the owning thread
	dispatch_assert(_dispatch_queue_is_thread_bound(dq));
	uint64_t old_state, new_state;
	os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, relaxed, {
		new_state = old_state;
		new_state &= ~DISPATCH_QUEUE_DRAIN_OWNER_MASK;
		new_state |= _dispatch_lock_value_for_self();
	});
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_clear_bound_thread(dispatch_queue_t dq)
{
	dispatch_assert(_dispatch_queue_is_thread_bound(dq));
	_dispatch_queue_atomic_flags_clear(dq, DQF_THREAD_BOUND|DQF_CANNOT_TRYSYNC);
	os_atomic_and2o(dq, dq_state, ~DISPATCH_QUEUE_DRAIN_OWNER_MASK, relaxed);
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
static inline dispatch_priority_t
_dispatch_get_basepri(void)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	return (dispatch_priority_t)(uintptr_t)_dispatch_thread_getspecific(
			dispatch_basepri_key);
#else
	return 0;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_reset_basepri(dispatch_priority_t dbp)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	dispatch_priority_t old_dbp = _dispatch_get_basepri();
	// If an inner-loop or'd in the override flag to the per-thread priority,
	// it needs to be propagated up the chain.
	dbp &= ~DISPATCH_PRIORITY_OVERRIDE_MASK;
	dbp |= (old_dbp & DISPATCH_PRIORITY_OVERRIDE_MASK);
	_dispatch_thread_setspecific(dispatch_basepri_key, (void*)(uintptr_t)dbp);
#else
	(void)dbp;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dispatch_get_basepri_override_qos_floor(void)
{
	dispatch_priority_t dbp = _dispatch_get_basepri();
	dispatch_qos_t qos = _dispatch_priority_qos(dbp);
	dispatch_qos_t oqos = _dispatch_priority_override_qos(dbp);
	dispatch_qos_t qos_floor = MAX(qos, oqos);
	return qos_floor ? qos_floor : DISPATCH_QOS_SATURATED;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_set_basepri_override_qos(dispatch_qos_t qos)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	dispatch_priority_t dbp = _dispatch_get_basepri();
	if (_dispatch_priority_override_qos(dbp) >= qos) return;
	dbp &= ~DISPATCH_PRIORITY_OVERRIDE_MASK;
	dbp |= qos << DISPATCH_PRIORITY_OVERRIDE_SHIFT;
	_dispatch_thread_setspecific(dispatch_basepri_key, (void*)(uintptr_t)dbp);
#else
	(void)qos;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_reset_basepri_override(void)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	dispatch_priority_t dbp = _dispatch_get_basepri();
	dispatch_qos_t oqos = _dispatch_priority_override_qos(dbp);
	if (oqos) {
		dbp &= ~DISPATCH_PRIORITY_OVERRIDE_MASK;
		_dispatch_thread_setspecific(dispatch_basepri_key, (void*)(uintptr_t)dbp);
		return oqos != DISPATCH_QOS_SATURATED;
	}
#endif
	return false;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_priority_t
_dispatch_set_basepri(dispatch_priority_t dbp)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	const dispatch_priority_t preserved_mask =
			DISPATCH_PRIORITY_OVERRIDE_MASK | DISPATCH_PRIORITY_FLAG_OVERCOMMIT;
	dispatch_priority_t old_dbp = _dispatch_get_basepri();
	if (old_dbp) {
		dispatch_priority_t flags, defaultqueue, basepri;
		flags = (dbp & DISPATCH_PRIORITY_FLAG_DEFAULTQUEUE);
		defaultqueue = (old_dbp & DISPATCH_PRIORITY_FLAG_DEFAULTQUEUE);
		basepri = old_dbp & DISPATCH_PRIORITY_REQUESTED_MASK;
		dbp &= DISPATCH_PRIORITY_REQUESTED_MASK;
		if (!dbp) {
			flags = DISPATCH_PRIORITY_FLAG_INHERIT | defaultqueue;
			dbp = basepri;
		} else if (dbp < basepri && !defaultqueue) { // rdar://16349734
			dbp = basepri;
		}
		dbp |= flags | (old_dbp & preserved_mask);
	} else {
		dbp &= ~DISPATCH_PRIORITY_OVERRIDE_MASK;
	}
	_dispatch_thread_setspecific(dispatch_basepri_key, (void*)(uintptr_t)dbp);
	return old_dbp;
#else
	(void)dbp;
	return 0;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_priority_t
_dispatch_set_basepri_wlh(dispatch_priority_t dbp)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	dispatch_assert(!_dispatch_get_basepri());
	// _dispatch_set_basepri_override_qos(DISPATCH_QOS_SATURATED)
	dbp |= DISPATCH_QOS_SATURATED << DISPATCH_PRIORITY_OVERRIDE_SHIFT;
	_dispatch_thread_setspecific(dispatch_basepri_key, (void*)(uintptr_t)dbp);
#else
	(void)dbp;
#endif
	return 0;
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_priority_adopt(pthread_priority_t pp, unsigned long flags)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	dispatch_priority_t inherited, defaultqueue, dbp = _dispatch_get_basepri();
	pthread_priority_t basepp = _dispatch_priority_to_pp_strip_flags(dbp);
	bool enforce = (flags & DISPATCH_PRIORITY_ENFORCE) ||
			(pp & _PTHREAD_PRIORITY_ENFORCE_FLAG);
	inherited = (dbp & DISPATCH_PRIORITY_FLAG_INHERIT);
	defaultqueue = (dbp & DISPATCH_PRIORITY_FLAG_DEFAULTQUEUE);
	pp &= ~_PTHREAD_PRIORITY_FLAGS_MASK;

	if (!pp) {
		return basepp;
	} else if (defaultqueue) { // rdar://16349734
		return pp;
	} else if (pp < basepp) {
		return basepp;
	} else if (enforce || inherited) {
		return pp;
	} else {
		return basepp;
	}
#else
	(void)pp; (void)flags;
	return 0;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_priority_inherit_from_target(dispatch_queue_t dq,
		dispatch_queue_t tq)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	const dispatch_priority_t rootqueue_flag = DISPATCH_PRIORITY_FLAG_ROOTQUEUE;
	const dispatch_priority_t inherited_flag = DISPATCH_PRIORITY_FLAG_INHERIT;
	const dispatch_priority_t defaultqueue_flag =
            DISPATCH_PRIORITY_FLAG_DEFAULTQUEUE;
	dispatch_priority_t pri = dq->dq_priority, tpri = tq->dq_priority;

	if ((!_dispatch_priority_qos(pri) || (pri & inherited_flag)) &&
			(tpri & rootqueue_flag)) {
		if (_dispatch_priority_override_qos(pri) == DISPATCH_QOS_SATURATED) {
			pri &= DISPATCH_PRIORITY_OVERRIDE_MASK;
		} else {
			pri = 0;
		}
		if (tpri & defaultqueue_flag) {
			// <rdar://problem/32921639> base queues need to know they target
			// the default root queue so that _dispatch_queue_override_qos()
			// in _dispatch_queue_class_wakeup() can fallback to QOS_DEFAULT
			// if no other priority was provided.
			pri |= defaultqueue_flag;
		} else {
			pri |= (tpri & ~rootqueue_flag) | inherited_flag;
		}
		dq->dq_priority = pri;
	} else if (pri & defaultqueue_flag) {
		// the DEFAULTQUEUE flag is only set on queues due to the code above,
		// and must never be kept if we don't target a global root queue.
		dq->dq_priority = (pri & ~defaultqueue_flag);
	}
#else
	(void)dq; (void)tq;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_priority_t
_dispatch_priority_inherit_from_root_queue(dispatch_priority_t pri,
		dispatch_queue_t rq)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	dispatch_priority_t p = pri & DISPATCH_PRIORITY_REQUESTED_MASK;
	dispatch_priority_t rqp = rq->dq_priority & DISPATCH_PRIORITY_REQUESTED_MASK;
	dispatch_priority_t defaultqueue =
			rq->dq_priority & DISPATCH_PRIORITY_FLAG_DEFAULTQUEUE;

	if (!p || (!defaultqueue && p < rqp)) {
		p = rqp | defaultqueue;
	}
	return p | (rq->dq_priority & DISPATCH_PRIORITY_FLAG_OVERCOMMIT);
#else
	(void)rq; (void)pri;
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
		// the slow path in _dispatch_set_priority_and_voucher_slow() will
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
		voucher_t v, dispatch_thread_set_self_t flags)
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
		voucher_t v, dispatch_thread_set_self_t flags)
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
_dispatch_reset_voucher(voucher_t v, dispatch_thread_set_self_t flags)
{
	flags |= DISPATCH_VOUCHER_CONSUME | DISPATCH_VOUCHER_REPLACE;
	(void)_dispatch_set_priority_and_voucher(0, v, flags);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_need_override(dispatch_queue_class_t dqu, dispatch_qos_t qos)
{
	uint64_t dq_state = os_atomic_load2o(dqu._dq, dq_state, relaxed);
	// dq_priority "override qos" contains the priority at which the queue
	// is already running for thread-bound queues.
	// For non thread-bound queues, the qos of the queue may not be observed
	// when the first work item is dispatched synchronously.
	return _dq_state_max_qos(dq_state) < qos &&
			_dispatch_priority_override_qos(dqu._dq->dq_priority) < qos;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_need_override_retain(dispatch_queue_class_t dqu,
		dispatch_qos_t qos)
{
	if (_dispatch_queue_need_override(dqu, qos)) {
		_os_object_retain_internal_n_inline(dqu._oq->_as_os_obj, 2);
		return true;
	}
	return false;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dispatch_queue_override_qos(dispatch_queue_class_t dqu, dispatch_qos_t qos)
{
	if (dqu._oq->oq_priority & DISPATCH_PRIORITY_FLAG_DEFAULTQUEUE) {
		// queues targeting the default root queue use any asynchronous
		// workitem priority available and fallback to QOS_DEFAULT otherwise.
		return qos ? qos : DISPATCH_QOS_DEFAULT;
	}
	// for asynchronous workitems, queue priority is the floor for overrides
	return MAX(qos, _dispatch_priority_qos(dqu._oq->oq_priority));
}

#define DISPATCH_PRIORITY_PROPAGATE_CURRENT 0x1
#define DISPATCH_PRIORITY_PROPAGATE_FOR_SYNC_IPC 0x2

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_priority_compute_propagated(pthread_priority_t pp,
		unsigned int flags)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	if (flags & DISPATCH_PRIORITY_PROPAGATE_CURRENT) {
		pp = _dispatch_get_priority();
	}
	pp &= ~_PTHREAD_PRIORITY_FLAGS_MASK;
	if (!(flags & DISPATCH_PRIORITY_PROPAGATE_FOR_SYNC_IPC) &&
			pp > _dispatch_qos_to_pp(DISPATCH_QOS_USER_INITIATED)) {
		// Cap QOS for propagation at user-initiated <rdar://16681262&16998036>
		return _dispatch_qos_to_pp(DISPATCH_QOS_USER_INITIATED);
	}
	return pp;
#else
	(void)pp; (void)flags;
	return 0;
#endif
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_priority_propagate(void)
{
	return _dispatch_priority_compute_propagated(0,
			DISPATCH_PRIORITY_PROPAGATE_CURRENT);
}

// including maintenance
DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_is_background_thread(void)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pthread_priority_t pp = _dispatch_get_priority();
	return _dispatch_qos_is_background(_dispatch_qos_from_pp(pp));
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

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline pthread_priority_t
_dispatch_block_invoke_should_set_priority(dispatch_block_flags_t flags,
        pthread_priority_t new_pri)
{
	pthread_priority_t old_pri, p = 0;  // 0 means do not change priority.
	if ((flags & DISPATCH_BLOCK_HAS_PRIORITY)
			&& ((flags & DISPATCH_BLOCK_ENFORCE_QOS_CLASS) ||
			!(flags & DISPATCH_BLOCK_INHERIT_QOS_CLASS))) {
		old_pri = _dispatch_get_priority();
		new_pri &= ~_PTHREAD_PRIORITY_FLAGS_MASK;
		p = old_pri & ~_PTHREAD_PRIORITY_FLAGS_MASK;
		if (!p || p >= new_pri) p = 0;
	}
	return p;
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
	_dispatch_perfmon_workitem_inc();
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_continuation_pop_inline(dispatch_object_t dou,
		dispatch_invoke_context_t dic, dispatch_invoke_flags_t flags,
		dispatch_queue_t dq)
{
	dispatch_pthread_root_queue_observer_hooks_t observer_hooks =
			_dispatch_get_pthread_root_queue_observer_hooks();
	if (observer_hooks) observer_hooks->queue_will_execute(dq);
	_dispatch_trace_continuation_pop(dq, dou);
	flags &= _DISPATCH_INVOKE_PROPAGATE_MASK;
	if (_dispatch_object_has_vtable(dou)) {
		dx_invoke(dou._do, dic, flags);
	} else {
		_dispatch_continuation_invoke_inline(dou, DISPATCH_NO_VOUCHER, flags);
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
static inline dispatch_qos_t
_dispatch_continuation_override_qos(dispatch_queue_t dq,
		dispatch_continuation_t dc)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	dispatch_qos_t dc_qos = _dispatch_qos_from_pp(dc->dc_priority);
	bool enforce = dc->dc_priority & _PTHREAD_PRIORITY_ENFORCE_FLAG;
	dispatch_qos_t dq_qos = _dispatch_priority_qos(dq->dq_priority);
	bool defaultqueue = dq->dq_priority & DISPATCH_PRIORITY_FLAG_DEFAULTQUEUE;

	dispatch_assert(dc->dc_priority != DISPATCH_NO_PRIORITY);
	if (dc_qos && (enforce || !dq_qos || defaultqueue)) {
		return dc_qos;
	}
	return dq_qos;
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

#if HAVE_MACH
#pragma mark dispatch_mach_reply_refs_t

// assumes low bit of mach port names is always set
#define DISPATCH_MACH_REPLY_PORT_UNOWNED 0x1u

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_mach_reply_mark_reply_port_owned(dispatch_mach_reply_refs_t dmr)
{
	dmr->du_ident &= ~DISPATCH_MACH_REPLY_PORT_UNOWNED;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_mach_reply_is_reply_port_owned(dispatch_mach_reply_refs_t dmr)
{
	mach_port_t reply_port = (mach_port_t)dmr->du_ident;
	return reply_port ? !(reply_port & DISPATCH_MACH_REPLY_PORT_UNOWNED) :false;
}

DISPATCH_ALWAYS_INLINE
static inline mach_port_t
_dispatch_mach_reply_get_reply_port(mach_port_t reply_port)
{
	return reply_port ? (reply_port | DISPATCH_MACH_REPLY_PORT_UNOWNED) : 0;
}

#endif // HAVE_MACH

#endif // DISPATCH_PURE_C

#endif /* __DISPATCH_INLINE_INTERNAL__ */
