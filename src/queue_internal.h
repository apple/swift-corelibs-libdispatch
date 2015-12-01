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

#ifndef __DISPATCH_QUEUE_INTERNAL__
#define __DISPATCH_QUEUE_INTERNAL__

#ifndef __DISPATCH_INDIRECT__
#error "Please #include <dispatch/dispatch.h> instead of this file directly."
#include <dispatch/base.h> // for HeaderDoc
#endif

#if defined(__BLOCKS__) && !defined(DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES)
#define DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES 1 // <rdar://problem/10719357>
#endif

/* x86 & cortex-a8 have a 64 byte cacheline */
#define DISPATCH_CACHELINE_SIZE 64u
#define DISPATCH_CONTINUATION_SIZE DISPATCH_CACHELINE_SIZE
#define ROUND_UP_TO_CACHELINE_SIZE(x) \
		(((x) + (DISPATCH_CACHELINE_SIZE - 1u)) & \
		~(DISPATCH_CACHELINE_SIZE - 1u))
#define ROUND_UP_TO_CONTINUATION_SIZE(x) \
		(((x) + (DISPATCH_CONTINUATION_SIZE - 1u)) & \
		~(DISPATCH_CONTINUATION_SIZE - 1u))
#define ROUND_UP_TO_VECTOR_SIZE(x) \
		(((x) + 15u) & ~15u)
#define DISPATCH_CACHELINE_ALIGN \
		__attribute__((__aligned__(DISPATCH_CACHELINE_SIZE)))


#define DISPATCH_QUEUE_CACHELINE_PADDING \
		char _dq_pad[DISPATCH_QUEUE_CACHELINE_PAD]
#ifdef __LP64__
#define DISPATCH_QUEUE_CACHELINE_PAD (( \
		(3*sizeof(void*) - DISPATCH_INTROSPECTION_QUEUE_LIST_SIZE) \
		+ DISPATCH_CACHELINE_SIZE) % DISPATCH_CACHELINE_SIZE)
#else
#define DISPATCH_QUEUE_CACHELINE_PAD (( \
		(0*sizeof(void*) - DISPATCH_INTROSPECTION_QUEUE_LIST_SIZE) \
		+ DISPATCH_CACHELINE_SIZE) % DISPATCH_CACHELINE_SIZE)
#if !DISPATCH_INTROSPECTION
// No padding, DISPATCH_QUEUE_CACHELINE_PAD == 0
#undef DISPATCH_QUEUE_CACHELINE_PADDING
#define DISPATCH_QUEUE_CACHELINE_PADDING
#endif
#endif

// If dc_vtable is less than 127, then the object is a continuation.
// Otherwise, the object has a private layout and memory management rules. The
// layout until after 'do_next' must align with normal objects.
#define DISPATCH_CONTINUATION_HEADER(x) \
	_OS_OBJECT_HEADER( \
	const void *do_vtable, \
	do_ref_cnt, \
	do_xref_cnt); \
	struct dispatch_##x##_s *volatile do_next; \
	dispatch_function_t dc_func; \
	void *dc_ctxt; \
	void *dc_data; \
	void *dc_other;

#define DISPATCH_OBJ_ASYNC_BIT		0x1
#define DISPATCH_OBJ_BARRIER_BIT	0x2
#define DISPATCH_OBJ_GROUP_BIT		0x4
#define DISPATCH_OBJ_SYNC_SLOW_BIT	0x8
// vtables are pointers far away from the low page in memory
#define DISPATCH_OBJ_IS_VTABLE(x) ((unsigned long)(x)->do_vtable > 127ul)

struct dispatch_continuation_s {
	DISPATCH_CONTINUATION_HEADER(continuation);
};

typedef struct dispatch_continuation_s *dispatch_continuation_t;

struct dispatch_apply_s {
	size_t volatile da_index, da_todo;
	size_t da_iterations, da_nested;
	dispatch_continuation_t da_dc;
	_dispatch_thread_semaphore_t da_sema;
	uint32_t da_thr_cnt;
};

typedef struct dispatch_apply_s *dispatch_apply_t;

DISPATCH_CLASS_DECL(queue_attr);
struct dispatch_queue_attr_s {
	DISPATCH_STRUCT_HEADER(queue_attr);
};

#define DISPATCH_QUEUE_HEADER \
	uint32_t volatile dq_running; \
	struct dispatch_object_s *volatile dq_items_head; \
	/* LP64 global queue cacheline boundary */ \
	struct dispatch_object_s *volatile dq_items_tail; \
	dispatch_queue_t dq_specific_q; \
	uint32_t dq_width; \
	unsigned int dq_is_thread_bound:1; \
	unsigned long dq_serialnum; \
	const char *dq_label; \
	DISPATCH_INTROSPECTION_QUEUE_LIST;

DISPATCH_CLASS_DECL(queue);
struct dispatch_queue_s {
	DISPATCH_STRUCT_HEADER(queue);
	DISPATCH_QUEUE_HEADER;
	DISPATCH_QUEUE_CACHELINE_PADDING; // for static queues only
};

DISPATCH_INTERNAL_SUBCLASS_DECL(queue_root, queue);
DISPATCH_INTERNAL_SUBCLASS_DECL(queue_runloop, queue);
DISPATCH_INTERNAL_SUBCLASS_DECL(queue_mgr, queue);

DISPATCH_DECL_INTERNAL_SUBCLASS(dispatch_queue_specific_queue, dispatch_queue);
DISPATCH_CLASS_DECL(queue_specific_queue);

extern struct dispatch_queue_s _dispatch_mgr_q;

void _dispatch_queue_destroy(dispatch_object_t dou);
void _dispatch_queue_dispose(dispatch_queue_t dq);
void _dispatch_queue_invoke(dispatch_queue_t dq);
void _dispatch_queue_push_list_slow(dispatch_queue_t dq,
		struct dispatch_object_s *obj, unsigned int n);
void _dispatch_queue_push_slow(dispatch_queue_t dq,
		struct dispatch_object_s *obj);
unsigned long _dispatch_queue_probe(dispatch_queue_t dq);
dispatch_queue_t _dispatch_wakeup(dispatch_object_t dou);
_dispatch_thread_semaphore_t _dispatch_queue_drain(dispatch_object_t dou);
void _dispatch_queue_specific_queue_dispose(dispatch_queue_specific_queue_t
		dqsq);
unsigned long _dispatch_root_queue_probe(dispatch_queue_t dq);
void _dispatch_pthread_root_queue_dispose(dispatch_queue_t dq);
unsigned long _dispatch_runloop_queue_probe(dispatch_queue_t dq);
void _dispatch_runloop_queue_xref_dispose(dispatch_queue_t dq);
void _dispatch_runloop_queue_dispose(dispatch_queue_t dq);
void _dispatch_mgr_queue_drain(void);
unsigned long _dispatch_mgr_queue_probe(dispatch_queue_t dq);
#if DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES
void _dispatch_mgr_priority_init(void);
#else
static inline void _dispatch_mgr_priority_init(void) {}
#endif
void _dispatch_after_timer_callback(void *ctxt);
void _dispatch_async_redirect_invoke(void *ctxt);
void _dispatch_sync_recurse_invoke(void *ctxt);
void _dispatch_apply_invoke(void *ctxt);
void _dispatch_apply_redirect_invoke(void *ctxt);
void _dispatch_barrier_trysync_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func);

#if DISPATCH_DEBUG
void dispatch_debug_queue(dispatch_queue_t dq, const char* str);
#else
static inline void dispatch_debug_queue(dispatch_queue_t dq DISPATCH_UNUSED,
		const char* str DISPATCH_UNUSED) {}
#endif

size_t dispatch_queue_debug(dispatch_queue_t dq, char* buf, size_t bufsiz);
size_t _dispatch_queue_debug_attr(dispatch_queue_t dq, char* buf,
		size_t bufsiz);

#define DISPATCH_QUEUE_PRIORITY_COUNT 4
#define DISPATCH_ROOT_QUEUE_COUNT (DISPATCH_QUEUE_PRIORITY_COUNT * 2)

// overcommit priority index values need bit 1 set
enum {
	DISPATCH_ROOT_QUEUE_IDX_LOW_PRIORITY = 0,
	DISPATCH_ROOT_QUEUE_IDX_LOW_OVERCOMMIT_PRIORITY,
	DISPATCH_ROOT_QUEUE_IDX_DEFAULT_PRIORITY,
	DISPATCH_ROOT_QUEUE_IDX_DEFAULT_OVERCOMMIT_PRIORITY,
	DISPATCH_ROOT_QUEUE_IDX_HIGH_PRIORITY,
	DISPATCH_ROOT_QUEUE_IDX_HIGH_OVERCOMMIT_PRIORITY,
	DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_PRIORITY,
	DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_OVERCOMMIT_PRIORITY,
};

extern unsigned long volatile _dispatch_queue_serial_numbers;
extern struct dispatch_queue_s _dispatch_root_queues[];

#if !(USE_OBJC && __OBJC2__)

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_push_list2(dispatch_queue_t dq, struct dispatch_object_s *head,
		struct dispatch_object_s *tail)
{
	struct dispatch_object_s *prev;
	tail->do_next = NULL;
	prev = dispatch_atomic_xchg2o(dq, dq_items_tail, tail, release);
	if (fastpath(prev)) {
		// if we crash here with a value less than 0x1000, then we are at a
		// known bug in client code for example, see _dispatch_queue_dispose
		// or _dispatch_atfork_child
		prev->do_next = head;
	}
	return (prev != NULL);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_push_list(dispatch_queue_t dq, dispatch_object_t _head,
		dispatch_object_t _tail, unsigned int n)
{
	struct dispatch_object_s *head = _head._do, *tail = _tail._do;
	if (!fastpath(_dispatch_queue_push_list2(dq, head, tail))) {
		_dispatch_queue_push_list_slow(dq, head, n);
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_push(dispatch_queue_t dq, dispatch_object_t _tail)
{
	struct dispatch_object_s *tail = _tail._do;
	if (!fastpath(_dispatch_queue_push_list2(dq, tail, tail))) {
		_dispatch_queue_push_slow(dq, tail);
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_push_wakeup(dispatch_queue_t dq, dispatch_object_t _tail,
		bool wakeup)
{
	struct dispatch_object_s *tail = _tail._do;
	if (!fastpath(_dispatch_queue_push_list2(dq, tail, tail))) {
		_dispatch_queue_push_slow(dq, tail);
	} else if (slowpath(wakeup)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_class_invoke(dispatch_object_t dou,
		dispatch_queue_t (*invoke)(dispatch_object_t,
		_dispatch_thread_semaphore_t*))
{
	dispatch_queue_t dq = dou._dq;
	if (!slowpath(DISPATCH_OBJECT_SUSPENDED(dq)) &&
			fastpath(dispatch_atomic_cmpxchg2o(dq, dq_running, 0, 1, acquire))){
		dispatch_queue_t tq = NULL;
		_dispatch_thread_semaphore_t sema = 0;
		tq = invoke(dq, &sema);
		// We do not need to check the result.
		// When the suspend-count lock is dropped, then the check will happen.
		(void)dispatch_atomic_dec2o(dq, dq_running, release);
		if (sema) {
			_dispatch_thread_semaphore_signal(sema);
		} else if (tq) {
			return _dispatch_queue_push(tq, dq);
		}
	}
	dq->do_next = DISPATCH_OBJECT_LISTLESS;
	if (!dispatch_atomic_sub2o(dq, do_suspend_cnt,
			DISPATCH_OBJECT_SUSPEND_LOCK, release)) {
		dispatch_atomic_barrier(seq_cst); // <rdar://problem/11915417>
		if (dispatch_atomic_load2o(dq, dq_running, seq_cst) == 0) {
			_dispatch_wakeup(dq); // verify that the queue is idle
		}
	}
	_dispatch_release(dq); // added when the queue is put on the list
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_t
_dispatch_queue_get_current(void)
{
	return (dispatch_queue_t)_dispatch_thread_getspecific(dispatch_queue_key);
}

DISPATCH_ALWAYS_INLINE DISPATCH_CONST
static inline dispatch_queue_t
_dispatch_get_root_queue(long priority, bool overcommit)
{
	if (overcommit) switch (priority) {
	case DISPATCH_QUEUE_PRIORITY_BACKGROUND:
#if !DISPATCH_NO_BG_PRIORITY
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_OVERCOMMIT_PRIORITY];
#endif
	case DISPATCH_QUEUE_PRIORITY_LOW:
	case DISPATCH_QUEUE_PRIORITY_NON_INTERACTIVE:
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_LOW_OVERCOMMIT_PRIORITY];
	case DISPATCH_QUEUE_PRIORITY_DEFAULT:
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_DEFAULT_OVERCOMMIT_PRIORITY];
	case DISPATCH_QUEUE_PRIORITY_HIGH:
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_HIGH_OVERCOMMIT_PRIORITY];
	}
	switch (priority) {
	case DISPATCH_QUEUE_PRIORITY_BACKGROUND:
#if !DISPATCH_NO_BG_PRIORITY
		return &_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_PRIORITY];
#endif
	case DISPATCH_QUEUE_PRIORITY_LOW:
	case DISPATCH_QUEUE_PRIORITY_NON_INTERACTIVE:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_LOW_PRIORITY];
	case DISPATCH_QUEUE_PRIORITY_DEFAULT:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_PRIORITY];
	case DISPATCH_QUEUE_PRIORITY_HIGH:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_HIGH_PRIORITY];
	default:
		return NULL;
	}
}

// Note to later developers: ensure that any initialization changes are
// made for statically allocated queues (i.e. _dispatch_main_q).
static inline void
_dispatch_queue_init(dispatch_queue_t dq)
{
	dq->do_next = (struct dispatch_queue_s *)DISPATCH_OBJECT_LISTLESS;

	dq->dq_running = 0;
	dq->dq_width = 1;
	dq->dq_serialnum = dispatch_atomic_inc_orig(&_dispatch_queue_serial_numbers,
			relaxed);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_set_bound_thread(dispatch_queue_t dq)
{
	//Tag thread-bound queues with the owning thread
	dispatch_assert(dq->dq_is_thread_bound);
	dq->do_finalizer = (void*)_dispatch_thread_self();
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_clear_bound_thread(dispatch_queue_t dq)
{
	dispatch_assert(dq->dq_is_thread_bound);
	dq->do_finalizer = NULL;
}

DISPATCH_ALWAYS_INLINE
static inline pthread_t
_dispatch_queue_get_bound_thread(dispatch_queue_t dq)
{
	dispatch_assert(dq->dq_is_thread_bound);
	return (pthread_t)dq->do_finalizer;
}

#ifndef DISPATCH_CONTINUATION_CACHE_LIMIT
#if TARGET_OS_EMBEDDED
#define DISPATCH_CONTINUATION_CACHE_LIMIT 112 // one 256k heap for 64 threads
#define DISPATCH_CONTINUATION_CACHE_LIMIT_MEMORYSTATUS_PRESSURE_WARN 16
#else
#define DISPATCH_CONTINUATION_CACHE_LIMIT 65536
#define DISPATCH_CONTINUATION_CACHE_LIMIT_MEMORYSTATUS_PRESSURE_WARN 128
#endif
#endif

dispatch_continuation_t _dispatch_continuation_alloc_from_heap(void);
void _dispatch_continuation_free_to_heap(dispatch_continuation_t c);

#if DISPATCH_USE_MEMORYSTATUS_SOURCE
extern int _dispatch_continuation_cache_limit;
void _dispatch_continuation_free_to_cache_limit(dispatch_continuation_t c);
#else
#define _dispatch_continuation_cache_limit DISPATCH_CONTINUATION_CACHE_LIMIT
#define _dispatch_continuation_free_to_cache_limit(c) \
		_dispatch_continuation_free_to_heap(c)
#endif

DISPATCH_ALWAYS_INLINE
static inline dispatch_continuation_t
_dispatch_continuation_alloc_cacheonly(void)
{
	dispatch_continuation_t dc = (dispatch_continuation_t)
			fastpath(_dispatch_thread_getspecific(dispatch_cache_key));
	if (dc) {
		_dispatch_thread_setspecific(dispatch_cache_key, dc->do_next);
	}
	return dc;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_continuation_t
_dispatch_continuation_alloc(void)
{
	dispatch_continuation_t dc =
			fastpath(_dispatch_continuation_alloc_cacheonly());
	if(!dc) {
		return _dispatch_continuation_alloc_from_heap();
	}
	return dc;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_continuation_t
_dispatch_continuation_free_cacheonly(dispatch_continuation_t dc)
{
	dispatch_continuation_t prev_dc = (dispatch_continuation_t)
			fastpath(_dispatch_thread_getspecific(dispatch_cache_key));
	int cnt = prev_dc ? prev_dc->do_ref_cnt + 1 : 1;
	// Cap continuation cache
	if (slowpath(cnt > _dispatch_continuation_cache_limit)) {
		return dc;
	}
	dc->do_next = prev_dc;
	dc->do_ref_cnt = cnt;
	_dispatch_thread_setspecific(dispatch_cache_key, dc);
	return NULL;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_free(dispatch_continuation_t dc)
{
	dc = _dispatch_continuation_free_cacheonly(dc);
	if (slowpath(dc)) {
		_dispatch_continuation_free_to_cache_limit(dc);
	}
}
#endif // !(USE_OBJC && __OBJC2__)

#endif
