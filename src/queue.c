/*
 * Copyright (c) 2008-2009 Apple Inc. All rights reserved.
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
#include "protocol.h"

void
dummy_function(void)
{
}

long
dummy_function_r0(void)
{
	return 0;
}

static bool _dispatch_select_workaround;
static fd_set _dispatch_rfds;
static fd_set _dispatch_wfds;
static void *_dispatch_rfd_ptrs[FD_SETSIZE];
static void *_dispatch_wfd_ptrs[FD_SETSIZE];


static struct dispatch_semaphore_s _dispatch_thread_mediator[] = {
	{
		.do_vtable = &_dispatch_semaphore_vtable,
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
	{
		.do_vtable = &_dispatch_semaphore_vtable,
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
	{
		.do_vtable = &_dispatch_semaphore_vtable,
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
	{
		.do_vtable = &_dispatch_semaphore_vtable,
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
	{
		.do_vtable = &_dispatch_semaphore_vtable,
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
	{
		.do_vtable = &_dispatch_semaphore_vtable,
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
};

static struct dispatch_queue_s _dispatch_root_queues[];

static inline dispatch_queue_t
_dispatch_get_root_queue(long priority, bool overcommit)
{
	if (overcommit) switch (priority) {
	case DISPATCH_QUEUE_PRIORITY_LOW:
		return &_dispatch_root_queues[1];
	case DISPATCH_QUEUE_PRIORITY_DEFAULT:
		return &_dispatch_root_queues[3];
	case DISPATCH_QUEUE_PRIORITY_HIGH:
		return &_dispatch_root_queues[5];
	}
	switch (priority) {
	case DISPATCH_QUEUE_PRIORITY_LOW:
		return &_dispatch_root_queues[0];
	case DISPATCH_QUEUE_PRIORITY_DEFAULT:
		return &_dispatch_root_queues[2];
	case DISPATCH_QUEUE_PRIORITY_HIGH:
		return &_dispatch_root_queues[4];
	default:
		return NULL;
	}
}

#ifdef __BLOCKS__
dispatch_block_t
_dispatch_Block_copy(dispatch_block_t db)
{
	dispatch_block_t rval;

	while (!(rval = Block_copy(db))) {
		sleep(1);
	}

	return rval;
}
#define _dispatch_Block_copy(x) ((typeof(x))_dispatch_Block_copy(x))

void
_dispatch_call_block_and_release(void *block)
{
	void (^b)(void) = block;
	b();
	Block_release(b);
}

void
_dispatch_call_block_and_release2(void *block, void *ctxt)
{
	void (^b)(void*) = block;
	b(ctxt);
	Block_release(b);
}

#endif /* __BLOCKS__ */

struct dispatch_queue_attr_vtable_s {
	DISPATCH_VTABLE_HEADER(dispatch_queue_attr_s);
};

struct dispatch_queue_attr_s {
	DISPATCH_STRUCT_HEADER(dispatch_queue_attr_s, dispatch_queue_attr_vtable_s);

	// Public:
	int qa_priority;
	void* finalizer_ctxt;
	dispatch_queue_finalizer_function_t finalizer_func;

	// Private:
	unsigned long qa_flags;
};

static int _dispatch_pthread_sigmask(int how, sigset_t *set, sigset_t *oset);

#define _dispatch_queue_trylock(dq) dispatch_atomic_cmpxchg(&(dq)->dq_running, 0, 1)
static inline void _dispatch_queue_unlock(dispatch_queue_t dq);
static void _dispatch_queue_invoke(dispatch_queue_t dq);
static void _dispatch_queue_serial_drain_till_empty(dispatch_queue_t dq);
static bool _dispatch_queue_wakeup_global(dispatch_queue_t dq);
static struct dispatch_object_s *_dispatch_queue_concurrent_drain_one(dispatch_queue_t dq);

static bool _dispatch_program_is_probably_callback_driven;

#if DISPATCH_COCOA_COMPAT
void (*dispatch_begin_thread_4GC)(void) = dummy_function;
void (*dispatch_end_thread_4GC)(void) = dummy_function;
void *(*_dispatch_begin_NSAutoReleasePool)(void) = (void *)dummy_function;
void (*_dispatch_end_NSAutoReleasePool)(void *) = (void *)dummy_function;
static void _dispatch_queue_wakeup_main(void);

static dispatch_once_t _dispatch_main_q_port_pred;
static bool main_q_is_draining;
static mach_port_t main_q_port;
#endif

static void _dispatch_cache_cleanup2(void *value);
static void _dispatch_force_cache_cleanup(void);

static const struct dispatch_queue_vtable_s _dispatch_queue_vtable = {
	.do_type = DISPATCH_QUEUE_TYPE,
	.do_kind = "queue",
	.do_dispose = _dispatch_queue_dispose,
	.do_invoke = (void *)dummy_function_r0,
	.do_probe = (void *)dummy_function_r0,
	.do_debug = dispatch_queue_debug,
};

static const struct dispatch_queue_vtable_s _dispatch_queue_root_vtable = {
	.do_type = DISPATCH_QUEUE_GLOBAL_TYPE,
	.do_kind = "global-queue",
	.do_debug = dispatch_queue_debug,
	.do_probe = _dispatch_queue_wakeup_global,
};

#define MAX_THREAD_COUNT 255

struct dispatch_root_queue_context_s {
	pthread_workqueue_t dgq_kworkqueue;
	uint32_t dgq_pending;
	uint32_t dgq_thread_pool_size;
	dispatch_semaphore_t dgq_thread_mediator;
};

#define DISPATCH_ROOT_QUEUE_COUNT (DISPATCH_QUEUE_PRIORITY_COUNT * 2)
static struct dispatch_root_queue_context_s _dispatch_root_queue_contexts[] = {
	{
		.dgq_thread_mediator = &_dispatch_thread_mediator[0],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
	},
	{
		.dgq_thread_mediator = &_dispatch_thread_mediator[1],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
	},
	{
		.dgq_thread_mediator = &_dispatch_thread_mediator[2],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
	},
	{
		.dgq_thread_mediator = &_dispatch_thread_mediator[3],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
	},
	{
		.dgq_thread_mediator = &_dispatch_thread_mediator[4],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
	},
	{
		.dgq_thread_mediator = &_dispatch_thread_mediator[5],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
	},
};

// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
// dq_running is set to 2 so that barrier operations go through the slow path
static struct dispatch_queue_s _dispatch_root_queues[] = {
	{
		.do_vtable = &_dispatch_queue_root_vtable,
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[0],

		.dq_label = "com.apple.root.low-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 4,
	},
	{
		.do_vtable = &_dispatch_queue_root_vtable,
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[1],

		.dq_label = "com.apple.root.low-overcommit-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 5,
	},
	{
		.do_vtable = &_dispatch_queue_root_vtable,
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[2],

		.dq_label = "com.apple.root.default-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 6,
	},
	{
		.do_vtable = &_dispatch_queue_root_vtable,
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[3],

		.dq_label = "com.apple.root.default-overcommit-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 7,
	},
	{
		.do_vtable = &_dispatch_queue_root_vtable,
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[4],

		.dq_label = "com.apple.root.high-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 8,
	},
	{
		.do_vtable = &_dispatch_queue_root_vtable,
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[5],

		.dq_label = "com.apple.root.high-overcommit-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 9,
	},
};

// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
struct dispatch_queue_s _dispatch_main_q = {
	.do_vtable = &_dispatch_queue_vtable,
	.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
	.do_targetq = &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_COUNT / 2],

	.dq_label = "com.apple.main-thread",
	.dq_running = 1,
	.dq_width = 1,
	.dq_serialnum = 1,
};

#if DISPATCH_PERF_MON
static OSSpinLock _dispatch_stats_lock;
static size_t _dispatch_bad_ratio;
static struct {
	uint64_t time_total;
	uint64_t count_total;
	uint64_t thread_total;
} _dispatch_stats[65]; // ffs*/fls*() returns zero when no bits are set
static void _dispatch_queue_merge_stats(uint64_t start);
#endif

static void *_dispatch_worker_thread(void *context);
static void _dispatch_worker_thread2(void *context);

malloc_zone_t *_dispatch_ccache_zone;

static inline void
_dispatch_continuation_free(dispatch_continuation_t dc)
{
	dispatch_continuation_t prev_dc = _dispatch_thread_getspecific(dispatch_cache_key);
	dc->do_next = prev_dc;
	_dispatch_thread_setspecific(dispatch_cache_key, dc);
}

static inline void
_dispatch_continuation_pop(dispatch_object_t dou)
{
	dispatch_continuation_t dc = dou._dc;
	dispatch_group_t dg;

	if (DISPATCH_OBJ_IS_VTABLE(dou._do)) {
		return _dispatch_queue_invoke(dou._dq);
	}

	// Add the item back to the cache before calling the function. This
	// allows the 'hot' continuation to be used for a quick callback.
	//
	// The ccache version is per-thread.
	// Therefore, the object has not been reused yet.
	// This generates better assembly.
	if ((long)dou._do->do_vtable & DISPATCH_OBJ_ASYNC_BIT) {
		_dispatch_continuation_free(dc);
	}
	if ((long)dou._do->do_vtable & DISPATCH_OBJ_GROUP_BIT) {
		dg = dc->dc_group;
	} else {
		dg = NULL;
	}
	dc->dc_func(dc->dc_ctxt);
	if (dg) {
		dispatch_group_leave(dg);
		_dispatch_release(dg);
	}
}

struct dispatch_object_s *
_dispatch_queue_concurrent_drain_one(dispatch_queue_t dq)
{
	struct dispatch_object_s *head, *next, *const mediator = (void *)~0ul;

	// The mediator value acts both as a "lock" and a signal
	head = dispatch_atomic_xchg(&dq->dq_items_head, mediator);

	if (slowpath(head == NULL)) {
		// The first xchg on the tail will tell the enqueueing thread that it
		// is safe to blindly write out to the head pointer. A cmpxchg honors
		// the algorithm.
		dispatch_atomic_cmpxchg(&dq->dq_items_head, mediator, NULL);
		_dispatch_debug("no work on global work queue");
		return NULL;
	}

	if (slowpath(head == mediator)) {
		// This thread lost the race for ownership of the queue.
		//
		// The ratio of work to libdispatch overhead must be bad. This
		// scenario implies that there are too many threads in the pool.
		// Create a new pending thread and then exit this thread.
		// The kernel will grant a new thread when the load subsides.
		_dispatch_debug("Contention on queue: %p", dq);
		_dispatch_queue_wakeup_global(dq);
#if DISPATCH_PERF_MON
		dispatch_atomic_inc(&_dispatch_bad_ratio);
#endif
		return NULL;
	}

	// Restore the head pointer to a sane value before returning.
	// If 'next' is NULL, then this item _might_ be the last item.
	next = fastpath(head->do_next);

	if (slowpath(!next)) {
		dq->dq_items_head = NULL;
		
		if (dispatch_atomic_cmpxchg(&dq->dq_items_tail, head, NULL)) {
			// both head and tail are NULL now
			goto out;
		}

		// There must be a next item now. This thread won't wait long.
		while (!(next = head->do_next)) {
			_dispatch_hardware_pause();
		}
	}

	dq->dq_items_head = next;
	_dispatch_queue_wakeup_global(dq);
out:
	return head;
}

dispatch_queue_t
dispatch_get_current_queue(void)
{
	return _dispatch_queue_get_current() ?: _dispatch_get_root_queue(0, true);
}

#undef dispatch_get_main_queue
__OSX_AVAILABLE_STARTING(__MAC_10_6,__IPHONE_NA)
dispatch_queue_t dispatch_get_main_queue(void);

dispatch_queue_t
dispatch_get_main_queue(void)
{
	return &_dispatch_main_q;
}
#define dispatch_get_main_queue() (&_dispatch_main_q)

struct _dispatch_hw_config_s _dispatch_hw_config;

static void
_dispatch_queue_set_width_init(void)
{
	size_t valsz = sizeof(uint32_t);

	errno = 0;
	sysctlbyname("hw.activecpu", &_dispatch_hw_config.cc_max_active, &valsz, NULL, 0);
	dispatch_assume_zero(errno);
	dispatch_assume(valsz == sizeof(uint32_t));

	errno = 0;
	sysctlbyname("hw.logicalcpu_max", &_dispatch_hw_config.cc_max_logical, &valsz, NULL, 0);
	dispatch_assume_zero(errno);
	dispatch_assume(valsz == sizeof(uint32_t));

	errno = 0;
	sysctlbyname("hw.physicalcpu_max", &_dispatch_hw_config.cc_max_physical, &valsz, NULL, 0);
	dispatch_assume_zero(errno);
	dispatch_assume(valsz == sizeof(uint32_t));
}

void
dispatch_queue_set_width(dispatch_queue_t dq, long width)
{
	int w = (int)width;	// intentional truncation
	uint32_t tmp;

	if (slowpath(dq->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		return;
	}
	if (w == 1 || w == 0) {
		dq->dq_width = 1;
		return;
	}
	if (w > 0) {
		tmp = w;
	} else switch (w) {
	case DISPATCH_QUEUE_WIDTH_MAX_PHYSICAL_CPUS:
		tmp = _dispatch_hw_config.cc_max_physical;
		break;
	case DISPATCH_QUEUE_WIDTH_ACTIVE_CPUS:
		tmp = _dispatch_hw_config.cc_max_active;
		break;
	default:
		// fall through
	case DISPATCH_QUEUE_WIDTH_MAX_LOGICAL_CPUS:
		tmp = _dispatch_hw_config.cc_max_logical;
		break;
	}
	// multiply by two since the running count is inc/dec by two (the low bit == barrier)
	dq->dq_width = tmp * 2;

	// XXX if the queue has items and the width is increased, we should try to wake the queue
}

// skip zero
// 1 - main_q
// 2 - mgr_q
// 3 - _unused_
// 4,5,6,7,8,9 - global queues
// we use 'xadd' on Intel, so the initial value == next assigned
static unsigned long _dispatch_queue_serial_numbers = 10;

// Note to later developers: ensure that any initialization changes are
// made for statically allocated queues (i.e. _dispatch_main_q).
inline void
_dispatch_queue_init(dispatch_queue_t dq)
{
	dq->do_vtable = &_dispatch_queue_vtable;
	dq->do_next = DISPATCH_OBJECT_LISTLESS;
	dq->do_ref_cnt = 1;
	dq->do_xref_cnt = 1;
	dq->do_targetq = _dispatch_get_root_queue(0, true);
	dq->dq_running = 0;
	dq->dq_width = 1;
	dq->dq_serialnum = dispatch_atomic_inc(&_dispatch_queue_serial_numbers) - 1;
}

dispatch_queue_t
dispatch_queue_create(const char *label, dispatch_queue_attr_t attr)
{
	dispatch_queue_t dq;
	size_t label_len;

	if (!label) {
		label = "";
	}

	label_len = strlen(label);
	if (label_len < (DISPATCH_QUEUE_MIN_LABEL_SIZE - 1)) {
		label_len = (DISPATCH_QUEUE_MIN_LABEL_SIZE - 1);
	}

	// XXX switch to malloc()
	dq = calloc(1ul, sizeof(struct dispatch_queue_s) - DISPATCH_QUEUE_MIN_LABEL_SIZE + label_len + 1);
	if (slowpath(!dq)) {
		return dq;
	}

	_dispatch_queue_init(dq);
	strcpy(dq->dq_label, label);

#ifndef DISPATCH_NO_LEGACY
	if (slowpath(attr)) {
		dq->do_targetq = _dispatch_get_root_queue(attr->qa_priority, attr->qa_flags & DISPATCH_QUEUE_OVERCOMMIT);
		dq->dq_finalizer_ctxt = attr->finalizer_ctxt;
		dq->dq_finalizer_func = attr->finalizer_func;
#ifdef __BLOCKS__
		if (attr->finalizer_func == (void*)_dispatch_call_block_and_release2) {
			// if finalizer_ctxt is a Block, retain it.
 			dq->dq_finalizer_ctxt = Block_copy(dq->dq_finalizer_ctxt);
			if (!(dq->dq_finalizer_ctxt)) {
				goto out_bad;
			}
		}
#endif
	}
#endif

	return dq;

out_bad:
	free(dq);
	return NULL;
}

// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
void
_dispatch_queue_dispose(dispatch_queue_t dq)
{
	if (slowpath(dq == _dispatch_queue_get_current())) {
		DISPATCH_CRASH("Release of a queue by itself");
	}
	if (slowpath(dq->dq_items_tail)) {
		DISPATCH_CRASH("Release of a queue while items are enqueued");
	}

#ifndef DISPATCH_NO_LEGACY
	if (dq->dq_finalizer_func) {
		dq->dq_finalizer_func(dq->dq_finalizer_ctxt, dq);
	}
#endif

	// trash the tail queue so that use after free will crash
	dq->dq_items_tail = (void *)0x200;

	_dispatch_dispose(dq);
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_async_f_slow(dispatch_queue_t dq, void *context, dispatch_function_t func)
{
	dispatch_continuation_t dc = fastpath(_dispatch_continuation_alloc_from_heap());

	dc->do_vtable = (void *)(DISPATCH_OBJ_ASYNC_BIT | DISPATCH_OBJ_BARRIER_BIT);
	dc->dc_func = func;
	dc->dc_ctxt = context;

	_dispatch_queue_push(dq, dc);
}

#ifdef __BLOCKS__
void
dispatch_barrier_async(dispatch_queue_t dq, void (^work)(void))
{
	dispatch_barrier_async_f(dq, _dispatch_Block_copy(work), _dispatch_call_block_and_release);
}
#endif

DISPATCH_NOINLINE
void
dispatch_barrier_async_f(dispatch_queue_t dq, void *context, dispatch_function_t func)
{
	dispatch_continuation_t dc = fastpath(_dispatch_continuation_alloc_cacheonly());

	if (!dc) {
		return _dispatch_barrier_async_f_slow(dq, context, func);
	}

	dc->do_vtable = (void *)(DISPATCH_OBJ_ASYNC_BIT | DISPATCH_OBJ_BARRIER_BIT);
	dc->dc_func = func;
	dc->dc_ctxt = context;

	_dispatch_queue_push(dq, dc);
}

DISPATCH_NOINLINE
static void
_dispatch_async_f_slow(dispatch_queue_t dq, void *context, dispatch_function_t func)
{
	dispatch_continuation_t dc = fastpath(_dispatch_continuation_alloc_from_heap());

	dc->do_vtable = (void *)DISPATCH_OBJ_ASYNC_BIT;
	dc->dc_func = func;
	dc->dc_ctxt = context;

	_dispatch_queue_push(dq, dc);
}

#ifdef __BLOCKS__
void
dispatch_async(dispatch_queue_t dq, void (^work)(void))
{
	dispatch_async_f(dq, _dispatch_Block_copy(work), _dispatch_call_block_and_release);
}
#endif

DISPATCH_NOINLINE
void
dispatch_async_f(dispatch_queue_t dq, void *ctxt, dispatch_function_t func)
{
	dispatch_continuation_t dc = fastpath(_dispatch_continuation_alloc_cacheonly());

	// unlike dispatch_sync_f(), we do NOT need to check the queue width,
	// the "drain" function will do this test

	if (!dc) {
		return _dispatch_async_f_slow(dq, ctxt, func);
	}

	dc->do_vtable = (void *)DISPATCH_OBJ_ASYNC_BIT;
	dc->dc_func = func;
	dc->dc_ctxt = ctxt;

	_dispatch_queue_push(dq, dc);
}

struct dispatch_barrier_sync_slow2_s {
	dispatch_function_t dbss2_func;
	dispatch_function_t dbss2_ctxt;
	dispatch_semaphore_t dbss2_sema;
};

static void
_dispatch_barrier_sync_f_slow_invoke(void *ctxt)
{
	struct dispatch_barrier_sync_slow2_s *dbss2 = ctxt;

	dbss2->dbss2_func(dbss2->dbss2_ctxt);
	dispatch_semaphore_signal(dbss2->dbss2_sema);
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_f_slow(dispatch_queue_t dq, void *ctxt, dispatch_function_t func)
{
	struct dispatch_barrier_sync_slow2_s dbss2 = {
		.dbss2_func = func,
		.dbss2_ctxt = ctxt,
		.dbss2_sema = _dispatch_get_thread_semaphore(),
	};
	struct dispatch_barrier_sync_slow_s {
		DISPATCH_CONTINUATION_HEADER(dispatch_barrier_sync_slow_s);
	} dbss = {
		.do_vtable = (void *)DISPATCH_OBJ_BARRIER_BIT,
		.dc_func = _dispatch_barrier_sync_f_slow_invoke,
		.dc_ctxt = &dbss2,
	};

	_dispatch_queue_push(dq, (void *)&dbss);

	while (dispatch_semaphore_wait(dbss2.dbss2_sema, dispatch_time(0, 3ull * NSEC_PER_SEC))) {
		if (DISPATCH_OBJECT_SUSPENDED(dq)) {
			continue;
		}
		if (_dispatch_queue_trylock(dq)) {
			_dispatch_queue_drain(dq);
			_dispatch_queue_unlock(dq);
		}
	}
	_dispatch_put_thread_semaphore(dbss2.dbss2_sema);
}

#ifdef __BLOCKS__
void
dispatch_barrier_sync(dispatch_queue_t dq, void (^work)(void))
{
	struct Block_basic *bb = (void *)work;

	dispatch_barrier_sync_f(dq, work, (dispatch_function_t)bb->Block_invoke);
}
#endif

DISPATCH_NOINLINE
void
dispatch_barrier_sync_f(dispatch_queue_t dq, void *ctxt, dispatch_function_t func)
{
	dispatch_queue_t old_dq = _dispatch_thread_getspecific(dispatch_queue_key);

	// 1) ensure that this thread hasn't enqueued anything ahead of this call
	// 2) the queue is not suspended
	// 3) the queue is not weird
	if (slowpath(dq->dq_items_tail)
			|| slowpath(DISPATCH_OBJECT_SUSPENDED(dq))
			|| slowpath(!_dispatch_queue_trylock(dq))) {
		return _dispatch_barrier_sync_f_slow(dq, ctxt, func);
	}

	_dispatch_thread_setspecific(dispatch_queue_key, dq);
	func(ctxt);
	_dispatch_workitem_inc();
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);
	_dispatch_queue_unlock(dq);
}

static void
_dispatch_sync_f_slow2(void *ctxt)
{
	dispatch_queue_t dq = _dispatch_queue_get_current();
	dispatch_atomic_add(&dq->dq_running, 2);
	dispatch_semaphore_signal(ctxt);
}

DISPATCH_NOINLINE
static void
_dispatch_sync_f_slow(dispatch_queue_t dq)
{
	// the global root queues do not need strict ordering
	if (dq->do_targetq == NULL) {
		dispatch_atomic_add(&dq->dq_running, 2);
		return;
	}

	struct dispatch_sync_slow_s {
		DISPATCH_CONTINUATION_HEADER(dispatch_sync_slow_s);
	} dss = {
		.do_vtable = NULL,
		.dc_func = _dispatch_sync_f_slow2,
		.dc_ctxt = _dispatch_get_thread_semaphore(),
	};

	// XXX FIXME -- concurrent queues can be come serial again
	_dispatch_queue_push(dq, (void *)&dss);

	dispatch_semaphore_wait(dss.dc_ctxt, DISPATCH_TIME_FOREVER);
	_dispatch_put_thread_semaphore(dss.dc_ctxt);
}

#ifdef __BLOCKS__
void
dispatch_sync(dispatch_queue_t dq, void (^work)(void))
{
	struct Block_basic *bb = (void *)work;
	dispatch_sync_f(dq, work, (dispatch_function_t)bb->Block_invoke);
}
#endif

DISPATCH_NOINLINE
void
dispatch_sync_f(dispatch_queue_t dq, void *ctxt, dispatch_function_t func)
{
	typeof(dq->dq_running) prev_cnt;
	dispatch_queue_t old_dq;

	if (dq->dq_width == 1) {
		return dispatch_barrier_sync_f(dq, ctxt, func);
	}

	// 1) ensure that this thread hasn't enqueued anything ahead of this call
	// 2) the queue is not suspended
	if (slowpath(dq->dq_items_tail) || slowpath(DISPATCH_OBJECT_SUSPENDED(dq))) {
		_dispatch_sync_f_slow(dq);
	} else {
		prev_cnt = dispatch_atomic_add(&dq->dq_running, 2) - 2;

		if (slowpath(prev_cnt & 1)) {
			if (dispatch_atomic_sub(&dq->dq_running, 2) == 0) {
				_dispatch_wakeup(dq);
			}
			_dispatch_sync_f_slow(dq);
		}
	}

	old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	_dispatch_thread_setspecific(dispatch_queue_key, dq);
	func(ctxt);
	_dispatch_workitem_inc();
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);

	if (slowpath(dispatch_atomic_sub(&dq->dq_running, 2) == 0)) {
		_dispatch_wakeup(dq);
	}
}

const char *
dispatch_queue_get_label(dispatch_queue_t dq)
{
	return dq->dq_label;
}

#if DISPATCH_COCOA_COMPAT
static void
_dispatch_main_q_port_init(void *ctxt __attribute__((unused)))
{
	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &main_q_port);
	DISPATCH_VERIFY_MIG(kr);
	dispatch_assume_zero(kr);
	kr = mach_port_insert_right(mach_task_self(), main_q_port, main_q_port, MACH_MSG_TYPE_MAKE_SEND);
	DISPATCH_VERIFY_MIG(kr);
	dispatch_assume_zero(kr);

	_dispatch_program_is_probably_callback_driven = true;
	_dispatch_safe_fork = false;
}

// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
DISPATCH_NOINLINE
static void
_dispatch_queue_set_mainq_drain_state(bool arg)
{
	main_q_is_draining = arg;
}
#endif

void
dispatch_main(void)
{
	if (pthread_main_np()) {
		_dispatch_program_is_probably_callback_driven = true;
		pthread_exit(NULL);
		DISPATCH_CRASH("pthread_exit() returned");
	}
	DISPATCH_CLIENT_CRASH("dispatch_main() must be called on the main thread");
}

static void
_dispatch_sigsuspend(void *ctxt __attribute__((unused)))
{
	static const sigset_t mask;

	for (;;) {
		sigsuspend(&mask);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_queue_cleanup2(void)
{
	dispatch_atomic_dec(&_dispatch_main_q.dq_running);

	if (dispatch_atomic_sub(&_dispatch_main_q.do_suspend_cnt, DISPATCH_OBJECT_SUSPEND_LOCK) == 0) {
		_dispatch_wakeup(&_dispatch_main_q);
	}

	// overload the "probably" variable to mean that dispatch_main() or
	// similar non-POSIX API was called
	// this has to run before the DISPATCH_COCOA_COMPAT below
	if (_dispatch_program_is_probably_callback_driven) {
		dispatch_async_f(_dispatch_get_root_queue(0, 0), NULL, _dispatch_sigsuspend);
		sleep(1);	// workaround 6778970
	}

#if DISPATCH_COCOA_COMPAT
	dispatch_once_f(&_dispatch_main_q_port_pred, NULL, _dispatch_main_q_port_init);

	mach_port_t mp = main_q_port;
	kern_return_t kr;

	main_q_port = 0;

	if (mp) {
		kr = mach_port_deallocate(mach_task_self(), mp);
		DISPATCH_VERIFY_MIG(kr);
		dispatch_assume_zero(kr);
		kr = mach_port_mod_refs(mach_task_self(), mp, MACH_PORT_RIGHT_RECEIVE, -1);
		DISPATCH_VERIFY_MIG(kr);
		dispatch_assume_zero(kr);
	}
#endif
}

dispatch_queue_t
dispatch_get_concurrent_queue(long pri)
{
	if (pri > 0) {
		pri = DISPATCH_QUEUE_PRIORITY_HIGH;
	} else if (pri < 0) {
		pri = DISPATCH_QUEUE_PRIORITY_LOW;
	}
	return _dispatch_get_root_queue(pri, false);
}

static void
_dispatch_queue_cleanup(void *ctxt)
{
	if (ctxt == &_dispatch_main_q) {
		return _dispatch_queue_cleanup2();
	}
	// POSIX defines that destructors are only called if 'ctxt' is non-null
	DISPATCH_CRASH("Premature thread exit while a dispatch queue is running");
}

dispatch_queue_t
dispatch_get_global_queue(long priority, unsigned long flags)
{
	if (flags & ~DISPATCH_QUEUE_OVERCOMMIT) {
		return NULL;
	}
	return _dispatch_get_root_queue(priority, flags & DISPATCH_QUEUE_OVERCOMMIT);
}

#define countof(x)	(sizeof(x) / sizeof(x[0]))
void
libdispatch_init(void)
{
	dispatch_assert(DISPATCH_QUEUE_PRIORITY_COUNT == 3);
	dispatch_assert(DISPATCH_ROOT_QUEUE_COUNT == 6);

	dispatch_assert(DISPATCH_QUEUE_PRIORITY_LOW == -DISPATCH_QUEUE_PRIORITY_HIGH);
	dispatch_assert(countof(_dispatch_root_queues) == DISPATCH_ROOT_QUEUE_COUNT);
	dispatch_assert(countof(_dispatch_thread_mediator) == DISPATCH_ROOT_QUEUE_COUNT);
	dispatch_assert(countof(_dispatch_root_queue_contexts) == DISPATCH_ROOT_QUEUE_COUNT);

	_dispatch_thread_key_init_np(dispatch_queue_key, _dispatch_queue_cleanup);
	_dispatch_thread_key_init_np(dispatch_sema4_key, (void (*)(void *))dispatch_release);	// use the extern release
	_dispatch_thread_key_init_np(dispatch_cache_key, _dispatch_cache_cleanup2);
#if DISPATCH_PERF_MON
	_dispatch_thread_key_init_np(dispatch_bcounter_key, NULL);
#endif

	_dispatch_thread_setspecific(dispatch_queue_key, &_dispatch_main_q);

	_dispatch_queue_set_width_init();
}

void
_dispatch_queue_unlock(dispatch_queue_t dq)
{
	if (slowpath(dispatch_atomic_dec(&dq->dq_running))) {
		return;
	}

	_dispatch_wakeup(dq);
}

// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
dispatch_queue_t
_dispatch_wakeup(dispatch_object_t dou)
{
	dispatch_queue_t tq;

	if (slowpath(DISPATCH_OBJECT_SUSPENDED(dou._do))) {
		return NULL;
	}
	if (!dx_probe(dou._do) && !dou._dq->dq_items_tail) {
		return NULL;
	}

	if (!_dispatch_trylock(dou._do)) {
#if DISPATCH_COCOA_COMPAT
		if (dou._dq == &_dispatch_main_q) {
			_dispatch_queue_wakeup_main();
		}
#endif
		return NULL;
	}
	_dispatch_retain(dou._do);
	tq = dou._do->do_targetq;
	_dispatch_queue_push(tq, dou._do);
	return tq;	// libdispatch doesn't need this, but the Instrument DTrace probe does
}

#if DISPATCH_COCOA_COMPAT
DISPATCH_NOINLINE
void
_dispatch_queue_wakeup_main(void)
{
	kern_return_t kr;

	dispatch_once_f(&_dispatch_main_q_port_pred, NULL, _dispatch_main_q_port_init);

	kr = _dispatch_send_wakeup_main_thread(main_q_port, 0);

	switch (kr) {
	case MACH_SEND_TIMEOUT:
	case MACH_SEND_TIMED_OUT:
	case MACH_SEND_INVALID_DEST:
		break;
	default:
		dispatch_assume_zero(kr);
		break;
	}

	_dispatch_safe_fork = false;
}
#endif

static inline int
_dispatch_rootq2wq_pri(long idx)
{
#ifdef WORKQ_DEFAULT_PRIOQUEUE
	switch (idx) {
	case 0:
	case 1:
		return WORKQ_LOW_PRIOQUEUE;
	case 2:
	case 3:
	default:
		return WORKQ_DEFAULT_PRIOQUEUE;
	case 4:
	case 5:
		return WORKQ_HIGH_PRIOQUEUE;
	}
#else
	return pri;
#endif
}

static void
_dispatch_root_queues_init(void *context __attribute__((unused)))
{
	bool disable_wq = getenv("LIBDISPATCH_DISABLE_KWQ");
	pthread_workqueue_attr_t pwq_attr;
	kern_return_t kr;
	int i, r;

	r = pthread_workqueue_attr_init_np(&pwq_attr);
	dispatch_assume_zero(r);

	for (i = 0; i < DISPATCH_ROOT_QUEUE_COUNT; i++) {
		r = pthread_workqueue_attr_setqueuepriority_np(&pwq_attr, _dispatch_rootq2wq_pri(i));
		dispatch_assume_zero(r);
		r = pthread_workqueue_attr_setovercommit_np(&pwq_attr, i & 1);
		dispatch_assume_zero(r);
// some software hangs if the non-overcommitting queues do not overcommit when threads block
#if 0
		if (!(i & 1)) {
			dispatch_root_queue_contexts[i].dgq_thread_pool_size = _dispatch_hw_config.cc_max_active;
		}
#endif

		r = 0;
		if (disable_wq || (r = pthread_workqueue_create_np(&_dispatch_root_queue_contexts[i].dgq_kworkqueue, &pwq_attr))) {
			if (r != ENOTSUP) {
				dispatch_assume_zero(r);
			}
			// override the default FIFO behavior for the pool semaphores
			kr = semaphore_create(mach_task_self(), &_dispatch_thread_mediator[i].dsema_port, SYNC_POLICY_LIFO, 0);
			DISPATCH_VERIFY_MIG(kr);
			dispatch_assume_zero(kr);
			dispatch_assume(_dispatch_thread_mediator[i].dsema_port);
		} else {
			dispatch_assume(_dispatch_root_queue_contexts[i].dgq_kworkqueue);
		}
	}

	r = pthread_workqueue_attr_destroy_np(&pwq_attr);
	dispatch_assume_zero(r);
}

bool
_dispatch_queue_wakeup_global(dispatch_queue_t dq)
{
	static dispatch_once_t pred;
	struct dispatch_root_queue_context_s *qc = dq->do_ctxt;
	pthread_workitem_handle_t wh;
	unsigned int gen_cnt;
	pthread_t pthr;
	int r, t_count;

	if (!dq->dq_items_tail) {
		return false;
	}

	_dispatch_safe_fork = false;

	dispatch_debug_queue(dq, __PRETTY_FUNCTION__);

	dispatch_once_f(&pred, NULL, _dispatch_root_queues_init);

	if (qc->dgq_kworkqueue) {
		if (dispatch_atomic_cmpxchg(&qc->dgq_pending, 0, 1)) {
			_dispatch_debug("requesting new worker thread");

			r = pthread_workqueue_additem_np(qc->dgq_kworkqueue, _dispatch_worker_thread2, dq, &wh, &gen_cnt);
			dispatch_assume_zero(r);
		} else {
			_dispatch_debug("work thread request still pending on global queue: %p", dq);
		}
		goto out;
	}

	if (dispatch_semaphore_signal(qc->dgq_thread_mediator)) {
		goto out;
	}

	do {
		t_count = qc->dgq_thread_pool_size;
		if (!t_count) {
			_dispatch_debug("The thread pool is full: %p", dq);
			goto out;
		}
	} while (!dispatch_atomic_cmpxchg(&qc->dgq_thread_pool_size, t_count, t_count - 1));

	while ((r = pthread_create(&pthr, NULL, _dispatch_worker_thread, dq))) {
		if (r != EAGAIN) {
			dispatch_assume_zero(r);
		}
		sleep(1);
	}
	r = pthread_detach(pthr);
	dispatch_assume_zero(r);

out:
	return false;
}

void
_dispatch_queue_serial_drain_till_empty(dispatch_queue_t dq)
{
#if DISPATCH_PERF_MON
	uint64_t start = mach_absolute_time();
#endif
	_dispatch_queue_drain(dq);
#if DISPATCH_PERF_MON
	_dispatch_queue_merge_stats(start);
#endif
	_dispatch_force_cache_cleanup();
}

// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
DISPATCH_NOINLINE
void
_dispatch_queue_invoke(dispatch_queue_t dq)
{
	dispatch_queue_t tq = dq->do_targetq;

	if (!slowpath(DISPATCH_OBJECT_SUSPENDED(dq)) && fastpath(_dispatch_queue_trylock(dq))) {
		_dispatch_queue_drain(dq);
		if (tq == dq->do_targetq) {
			tq = dx_invoke(dq);
		} else {
			tq = dq->do_targetq;
		}
		// We do not need to check the result.
		// When the suspend-count lock is dropped, then the check will happen.
		dispatch_atomic_dec(&dq->dq_running);
		if (tq) {
			return _dispatch_queue_push(tq, dq);
		}
	}

	dq->do_next = DISPATCH_OBJECT_LISTLESS;
	if (dispatch_atomic_sub(&dq->do_suspend_cnt, DISPATCH_OBJECT_SUSPEND_LOCK) == 0) {
		if (dq->dq_running == 0) {
			_dispatch_wakeup(dq);	// verify that the queue is idle
		}
	}
	_dispatch_release(dq);	// added when the queue is put on the list
}

// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
static void
_dispatch_set_target_queue2(void *ctxt)
{
	dispatch_queue_t prev_dq, dq = _dispatch_queue_get_current();
		  
	prev_dq = dq->do_targetq;
	dq->do_targetq = ctxt;
	_dispatch_release(prev_dq);
}

void
dispatch_set_target_queue(dispatch_object_t dou, dispatch_queue_t dq)
{
	if (slowpath(dou._do->do_xref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		return;
	}
	// NOTE: we test for NULL target queues internally to detect root queues
	// therefore, if the retain crashes due to a bad input, that is OK
	_dispatch_retain(dq);
	dispatch_barrier_async_f(dou._dq, dq, _dispatch_set_target_queue2);
}

static void
_dispatch_async_f_redirect2(void *_ctxt)
{
	struct dispatch_continuation_s *dc = _ctxt;
	struct dispatch_continuation_s *other_dc = dc->dc_data[1];
	dispatch_queue_t old_dq, dq = dc->dc_data[0];

	old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	_dispatch_thread_setspecific(dispatch_queue_key, dq);
	_dispatch_continuation_pop(other_dc);
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);

	if (dispatch_atomic_sub(&dq->dq_running, 2) == 0) {
		_dispatch_wakeup(dq);
	}
	_dispatch_release(dq);
}

static void
_dispatch_async_f_redirect(dispatch_queue_t dq, struct dispatch_object_s *other_dc)
{
	dispatch_continuation_t dc = (void *)other_dc;
	dispatch_queue_t root_dq = dq;

	if (dc->dc_func == _dispatch_sync_f_slow2) {
		return dc->dc_func(dc->dc_ctxt);
	}

	dispatch_atomic_add(&dq->dq_running, 2);
	_dispatch_retain(dq);

	dc = _dispatch_continuation_alloc_cacheonly() ?: _dispatch_continuation_alloc_from_heap();

	dc->do_vtable = (void *)DISPATCH_OBJ_ASYNC_BIT;
	dc->dc_func = _dispatch_async_f_redirect2;
	dc->dc_ctxt = dc;
	dc->dc_data[0] = dq;
	dc->dc_data[1] = other_dc;

	do {
		root_dq = root_dq->do_targetq;
	} while (root_dq->do_targetq);

	_dispatch_queue_push(root_dq, dc);
}


void
_dispatch_queue_drain(dispatch_queue_t dq)
{
	dispatch_queue_t orig_tq, old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	struct dispatch_object_s *dc = NULL, *next_dc = NULL;

	orig_tq = dq->do_targetq;

	_dispatch_thread_setspecific(dispatch_queue_key, dq);

	while (dq->dq_items_tail) {
		while (!fastpath(dq->dq_items_head)) {
			_dispatch_hardware_pause();
		}

		dc = dq->dq_items_head;
		dq->dq_items_head = NULL;

		do {
			// Enqueue is TIGHTLY controlled, we won't wait long.
			do {
				next_dc = fastpath(dc->do_next);
			} while (!next_dc && !dispatch_atomic_cmpxchg(&dq->dq_items_tail, dc, NULL));
			if (DISPATCH_OBJECT_SUSPENDED(dq)) {
				goto out;
			}
			if (dq->dq_running > dq->dq_width) {
				goto out;
			}
			if (orig_tq != dq->do_targetq) {
				goto out;
			}
			if (fastpath(dq->dq_width == 1)) {
				_dispatch_continuation_pop(dc);
				_dispatch_workitem_inc();
			} else if ((long)dc->do_vtable & DISPATCH_OBJ_BARRIER_BIT) {
				if (dq->dq_running > 1) {
					goto out;
				}
				_dispatch_continuation_pop(dc);
				_dispatch_workitem_inc();
			} else {
				_dispatch_async_f_redirect(dq, dc);
			}
		} while ((dc = next_dc));
	}

out:
	// if this is not a complete drain, we must undo some things
	if (slowpath(dc)) {
		// 'dc' must NOT be "popped"
		// 'dc' might be the last item
		if (next_dc || dispatch_atomic_cmpxchg(&dq->dq_items_tail, NULL, dc)) {
			dq->dq_items_head = dc;
		} else {
			while (!(next_dc = dq->dq_items_head)) {
				_dispatch_hardware_pause();
			}
			dq->dq_items_head = dc;
			dc->do_next = next_dc;
		}
	}

	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);
}

// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
void *
_dispatch_worker_thread(void *context)
{
	dispatch_queue_t dq = context;
	struct dispatch_root_queue_context_s *qc = dq->do_ctxt;
	sigset_t mask;
	int r;

	// workaround tweaks the kernel workqueue does for us
	r = sigfillset(&mask);
	dispatch_assume_zero(r);
	r = _dispatch_pthread_sigmask(SIG_BLOCK, &mask, NULL);
	dispatch_assume_zero(r);

	do {
		_dispatch_worker_thread2(context);
		// we use 65 seconds in case there are any timers that run once a minute
	} while (dispatch_semaphore_wait(qc->dgq_thread_mediator, dispatch_time(0, 65ull * NSEC_PER_SEC)) == 0);

	dispatch_atomic_inc(&qc->dgq_thread_pool_size);
	if (dq->dq_items_tail) {
		_dispatch_queue_wakeup_global(dq);
	}

	return NULL;
}

// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
void
_dispatch_worker_thread2(void *context)
{
	struct dispatch_object_s *item;
	dispatch_queue_t dq = context;
	struct dispatch_root_queue_context_s *qc = dq->do_ctxt;

	if (_dispatch_thread_getspecific(dispatch_queue_key)) {
		DISPATCH_CRASH("Premature thread recycling");
	}

	_dispatch_thread_setspecific(dispatch_queue_key, dq);
	qc->dgq_pending = 0;

#if DISPATCH_COCOA_COMPAT
	// ensure that high-level memory management techniques do not leak/crash
	dispatch_begin_thread_4GC();
	void *pool = _dispatch_begin_NSAutoReleasePool();
#endif

#if DISPATCH_PERF_MON
	uint64_t start = mach_absolute_time();
#endif
	while ((item = fastpath(_dispatch_queue_concurrent_drain_one(dq)))) {
		_dispatch_continuation_pop(item);
	}
#if DISPATCH_PERF_MON
	_dispatch_queue_merge_stats(start);
#endif

#if DISPATCH_COCOA_COMPAT
	_dispatch_end_NSAutoReleasePool(pool);
	dispatch_end_thread_4GC();
#endif

	_dispatch_thread_setspecific(dispatch_queue_key, NULL);

	_dispatch_force_cache_cleanup();
}

#if DISPATCH_PERF_MON
void
_dispatch_queue_merge_stats(uint64_t start)
{
	uint64_t avg, delta = mach_absolute_time() - start;
	unsigned long count, bucket;

	count = (size_t)_dispatch_thread_getspecific(dispatch_bcounter_key);
	_dispatch_thread_setspecific(dispatch_bcounter_key, NULL);

	if (count) {
		avg = delta / count;
		bucket = flsll(avg);
	} else {
		bucket = 0;
	}

	// 64-bit counters on 32-bit require a lock or a queue
	OSSpinLockLock(&_dispatch_stats_lock);

	_dispatch_stats[bucket].time_total += delta;
	_dispatch_stats[bucket].count_total += count;
	_dispatch_stats[bucket].thread_total++;

	OSSpinLockUnlock(&_dispatch_stats_lock);
}
#endif

size_t
dispatch_queue_debug_attr(dispatch_queue_t dq, char* buf, size_t bufsiz)
{
	return snprintf(buf, bufsiz, "parent = %p ", dq->do_targetq);
}

size_t
dispatch_queue_debug(dispatch_queue_t dq, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	offset += snprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ", dq->dq_label, dq);
	offset += dispatch_object_debug_attr(dq, &buf[offset], bufsiz - offset);
	offset += dispatch_queue_debug_attr(dq, &buf[offset], bufsiz - offset);
	offset += snprintf(&buf[offset], bufsiz - offset, "}");
	return offset;
}

#if DISPATCH_DEBUG
void
dispatch_debug_queue(dispatch_queue_t dq, const char* str) {
	if (fastpath(dq)) {
		dispatch_debug(dq, "%s", str);
	} else {
		_dispatch_log("queue[NULL]: %s", str);
	}
}
#endif

#if DISPATCH_COCOA_COMPAT
void
_dispatch_main_queue_callback_4CF(mach_msg_header_t *msg __attribute__((unused)))
{
	if (main_q_is_draining) {
		return;
	}
	_dispatch_queue_set_mainq_drain_state(true);
	_dispatch_queue_serial_drain_till_empty(&_dispatch_main_q);
	_dispatch_queue_set_mainq_drain_state(false);
}

mach_port_t
_dispatch_get_main_queue_port_4CF(void)
{
	dispatch_once_f(&_dispatch_main_q_port_pred, NULL, _dispatch_main_q_port_init);
	return main_q_port;
}
#endif

static void
dispatch_queue_attr_dispose(dispatch_queue_attr_t attr)
{
	dispatch_queue_attr_set_finalizer(attr, NULL);
	_dispatch_dispose(attr);
}

static const struct dispatch_queue_attr_vtable_s dispatch_queue_attr_vtable = {
	.do_type = DISPATCH_QUEUE_ATTR_TYPE,
	.do_kind = "queue-attr",
	.do_dispose = dispatch_queue_attr_dispose,
};

dispatch_queue_attr_t
dispatch_queue_attr_create(void)
{
	dispatch_queue_attr_t a = calloc(1, sizeof(struct dispatch_queue_attr_s));

	if (a) {
		a->do_vtable = &dispatch_queue_attr_vtable;
		a->do_next = DISPATCH_OBJECT_LISTLESS;
		a->do_ref_cnt = 1;
		a->do_xref_cnt = 1;
		a->do_targetq = _dispatch_get_root_queue(0, 0);
		a->qa_flags = DISPATCH_QUEUE_OVERCOMMIT;
	}
	return a;
}

void
dispatch_queue_attr_set_flags(dispatch_queue_attr_t attr, uint64_t flags)
{
	dispatch_assert_zero(flags & ~DISPATCH_QUEUE_FLAGS_MASK);
	attr->qa_flags = (unsigned long)flags & DISPATCH_QUEUE_FLAGS_MASK;
}
	
void
dispatch_queue_attr_set_priority(dispatch_queue_attr_t attr, int priority)
{
	dispatch_debug_assert(attr, "NULL pointer");
	dispatch_debug_assert(priority <= DISPATCH_QUEUE_PRIORITY_HIGH && priority >= DISPATCH_QUEUE_PRIORITY_LOW, "Invalid priority");

	if (priority > 0) {
		priority = DISPATCH_QUEUE_PRIORITY_HIGH;
	} else if (priority < 0) {
		priority = DISPATCH_QUEUE_PRIORITY_LOW;
	}

	attr->qa_priority = priority;
}

void
dispatch_queue_attr_set_finalizer_f(dispatch_queue_attr_t attr,
        void *context, dispatch_queue_finalizer_function_t finalizer)
{
#ifdef __BLOCKS__
	if (attr->finalizer_func == (void*)_dispatch_call_block_and_release2) {
		Block_release(attr->finalizer_ctxt);
	}
#endif
	attr->finalizer_ctxt = context;
	attr->finalizer_func = finalizer;
}

#ifdef __BLOCKS__
long
dispatch_queue_attr_set_finalizer(dispatch_queue_attr_t attr,
        dispatch_queue_finalizer_t finalizer)
{
	void *ctxt;
	dispatch_queue_finalizer_function_t func;

	if (finalizer) {
		if (!(ctxt = Block_copy(finalizer))) {
			return 1;
		}
		func = (void *)_dispatch_call_block_and_release2;
	} else {
		ctxt = NULL;
		func = NULL;
	}

	dispatch_queue_attr_set_finalizer_f(attr, ctxt, func);

	return 0;
}
#endif

static void
_dispatch_ccache_init(void *context __attribute__((unused)))
{
	_dispatch_ccache_zone = malloc_create_zone(0, 0);
	dispatch_assert(_dispatch_ccache_zone);
	malloc_set_zone_name(_dispatch_ccache_zone, "DispatchContinuations");
}

dispatch_continuation_t
_dispatch_continuation_alloc_from_heap(void)
{
	static dispatch_once_t pred;
	dispatch_continuation_t dc;

	dispatch_once_f(&pred, NULL, _dispatch_ccache_init);

	while (!(dc = fastpath(malloc_zone_calloc(_dispatch_ccache_zone, 1, ROUND_UP_TO_CACHELINE_SIZE(sizeof(*dc)))))) {
		sleep(1);
	}

	return dc;
}

void
_dispatch_force_cache_cleanup(void)
{
	dispatch_continuation_t dc = _dispatch_thread_getspecific(dispatch_cache_key);
	if (dc) {
		_dispatch_thread_setspecific(dispatch_cache_key, NULL);
		_dispatch_cache_cleanup2(dc);
	}
}

DISPATCH_NOINLINE
void
_dispatch_cache_cleanup2(void *value)
{
	dispatch_continuation_t dc, next_dc = value;

	while ((dc = next_dc)) {
		next_dc = dc->do_next;
		malloc_zone_free(_dispatch_ccache_zone, dc);
	}
}

static char _dispatch_build[16];

static void
_dispatch_bug_init(void *context __attribute__((unused)))
{
	int mib[] = { CTL_KERN, KERN_OSVERSION };
	size_t bufsz = sizeof(_dispatch_build);

	sysctl(mib, 2, _dispatch_build, &bufsz, NULL, 0);
}

void
_dispatch_bug(size_t line, long val)
{
	static dispatch_once_t pred;
	static void *last_seen;
	void *ra = __builtin_return_address(0);

	dispatch_once_f(&pred, NULL, _dispatch_bug_init);
	if (last_seen != ra) {
		last_seen = ra;
		_dispatch_log("BUG in libdispatch: %s - %lu - 0x%lx", _dispatch_build, line, val);
	}
}

void
_dispatch_abort(size_t line, long val)
{
	_dispatch_bug(line, val);
	abort();
}

void
_dispatch_log(const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);

	_dispatch_logv(msg, ap);

	va_end(ap);
}

void
_dispatch_logv(const char *msg, va_list ap)
{
#if DISPATCH_DEBUG
	static FILE *logfile, *tmp;
	char newbuf[strlen(msg) + 2];
	char path[PATH_MAX];

	sprintf(newbuf, "%s\n", msg);

	if (!logfile) {
		snprintf(path, sizeof(path), "/var/tmp/libdispatch.%d.log", getpid());
		tmp = fopen(path, "a");
		assert(tmp);
		if (!dispatch_atomic_cmpxchg(&logfile, NULL, tmp)) {
			fclose(tmp);
		} else {
			struct timeval tv;
			gettimeofday(&tv, NULL);
			fprintf(logfile, "=== log file opened for %s[%u] at %ld.%06u ===\n",
					getprogname() ?: "", getpid(), tv.tv_sec, tv.tv_usec);
		}
	}
	vfprintf(logfile, newbuf, ap);
	fflush(logfile);
#else
	vsyslog(LOG_NOTICE, msg, ap);
#endif
}

int
_dispatch_pthread_sigmask(int how, sigset_t *set, sigset_t *oset)
{
	int r;

	/* Workaround: 6269619 Not all signals can be delivered on any thread */

	r = sigdelset(set, SIGILL);
	dispatch_assume_zero(r);
	r = sigdelset(set, SIGTRAP);
	dispatch_assume_zero(r);
	r = sigdelset(set, SIGEMT);
	dispatch_assume_zero(r);
	r = sigdelset(set, SIGFPE);
	dispatch_assume_zero(r);
	r = sigdelset(set, SIGBUS);
	dispatch_assume_zero(r);
	r = sigdelset(set, SIGSEGV);
	dispatch_assume_zero(r);
	r = sigdelset(set, SIGSYS);
	dispatch_assume_zero(r);
	r = sigdelset(set, SIGPIPE);
	dispatch_assume_zero(r);

	return pthread_sigmask(how, set, oset);
}

bool _dispatch_safe_fork = true;

void
dispatch_atfork_prepare(void)
{
}

void
dispatch_atfork_parent(void)
{
}

void
dispatch_atfork_child(void)
{
	void *crash = (void *)0x100;
	size_t i;

	if (_dispatch_safe_fork) {
		return;
	}

	_dispatch_main_q.dq_items_head = crash;
	_dispatch_main_q.dq_items_tail = crash;

	_dispatch_mgr_q.dq_items_head = crash;
	_dispatch_mgr_q.dq_items_tail = crash;

	for (i = 0; i < DISPATCH_ROOT_QUEUE_COUNT; i++) {
		_dispatch_root_queues[i].dq_items_head = crash;
		_dispatch_root_queues[i].dq_items_tail = crash;
	}
}

void
dispatch_init_pthread(pthread_t pthr __attribute__((unused)))
{
}

static int _dispatch_kq;

static void
_dispatch_get_kq_init(void *context __attribute__((unused)))
{
	static const struct kevent kev = {
		.ident = 1,
		.filter = EVFILT_USER,
		.flags = EV_ADD|EV_CLEAR,
	};

	_dispatch_kq = kqueue();
	_dispatch_safe_fork = false;
	// in case we fall back to select()
	FD_SET(_dispatch_kq, &_dispatch_rfds);

	if (_dispatch_kq == -1) {
		dispatch_assert_zero(errno);
	}

	dispatch_assume_zero(kevent(_dispatch_kq, &kev, 1, NULL, 0, NULL)); 

	_dispatch_queue_push(_dispatch_mgr_q.do_targetq, &_dispatch_mgr_q);
}

static int
_dispatch_get_kq(void)
{
	static dispatch_once_t pred;

	dispatch_once_f(&pred, NULL, _dispatch_get_kq_init);

	return _dispatch_kq;
}

static void
_dispatch_mgr_thread2(struct kevent *kev, size_t cnt)
{
	size_t i;

	for (i = 0; i < cnt; i++) {
		// EVFILT_USER isn't used by sources
		if (kev[i].filter == EVFILT_USER) {
			// If _dispatch_mgr_thread2() ever is changed to return to the
			// caller, then this should become _dispatch_queue_drain()
			_dispatch_queue_serial_drain_till_empty(&_dispatch_mgr_q);
		} else {
			_dispatch_source_drain_kevent(&kev[i]);
		}
    }
}

static dispatch_queue_t
_dispatch_mgr_invoke(dispatch_queue_t dq)
{
	static const struct timespec timeout_immediately = { 0, 0 };
	struct timespec timeout;
	const struct timespec *timeoutp;
	struct timeval sel_timeout, *sel_timeoutp;
	fd_set tmp_rfds, tmp_wfds;
	struct kevent kev[1];
	int k_cnt, k_err, i, r;

	_dispatch_thread_setspecific(dispatch_queue_key, dq);

	for (;;) {
		_dispatch_run_timers();

		timeoutp = _dispatch_get_next_timer_fire(&timeout);
		
		if (_dispatch_select_workaround) {
			FD_COPY(&_dispatch_rfds, &tmp_rfds);
			FD_COPY(&_dispatch_wfds, &tmp_wfds);
			if (timeoutp) {
				sel_timeout.tv_sec = timeoutp->tv_sec;
				sel_timeout.tv_usec = (typeof(sel_timeout.tv_usec))(timeoutp->tv_nsec / 1000u);
				sel_timeoutp = &sel_timeout;
			} else {
				sel_timeoutp = NULL;
			}
			
			r = select(FD_SETSIZE, &tmp_rfds, &tmp_wfds, NULL, sel_timeoutp);
			if (r == -1) {
				if (errno != EBADF) {
					dispatch_assume_zero(errno);
					continue;
				}
				for (i = 0; i < FD_SETSIZE; i++) {
					if (i == _dispatch_kq) {
						continue;
					}
					if (!FD_ISSET(i, &_dispatch_rfds) && !FD_ISSET(i, &_dispatch_wfds)) {
						continue;
					}
					r = dup(i);
					if (r != -1) {
						close(r);
					} else {
						FD_CLR(i, &_dispatch_rfds);
						FD_CLR(i, &_dispatch_wfds);
						_dispatch_rfd_ptrs[i] = 0;
						_dispatch_wfd_ptrs[i] = 0;
					}
				}
				continue;
			}
			
			if (r > 0) {
				for (i = 0; i < FD_SETSIZE; i++) {
					if (i == _dispatch_kq) {
						continue;
					}
					if (FD_ISSET(i, &tmp_rfds)) {
						FD_CLR(i, &_dispatch_rfds);	// emulate EV_DISABLE
						EV_SET(&kev[0], i, EVFILT_READ, EV_ADD|EV_ENABLE|EV_DISPATCH, 0, 1, _dispatch_rfd_ptrs[i]);
						_dispatch_rfd_ptrs[i] = 0;
						_dispatch_mgr_thread2(kev, 1);
					}
					if (FD_ISSET(i, &tmp_wfds)) {
						FD_CLR(i, &_dispatch_wfds);	// emulate EV_DISABLE
						EV_SET(&kev[0], i, EVFILT_WRITE, EV_ADD|EV_ENABLE|EV_DISPATCH, 0, 1, _dispatch_wfd_ptrs[i]);
						_dispatch_wfd_ptrs[i] = 0;
						_dispatch_mgr_thread2(kev, 1);
					}
				}
			}
			
			timeoutp = &timeout_immediately;
		}
		
		k_cnt = kevent(_dispatch_kq, NULL, 0, kev, sizeof(kev) / sizeof(kev[0]), timeoutp);
		k_err = errno;

		switch (k_cnt) {
		case -1:
			if (k_err == EBADF) {
				DISPATCH_CLIENT_CRASH("Do not close random Unix descriptors");
			}
			dispatch_assume_zero(k_err);
			continue;
		default:
			_dispatch_mgr_thread2(kev, (size_t)k_cnt);
			// fall through
		case 0:
			_dispatch_force_cache_cleanup();
			continue;
		}
	}

	return NULL;
}

static bool
_dispatch_mgr_wakeup(dispatch_queue_t dq)
{
	static const struct kevent kev = {
		.ident = 1,
		.filter = EVFILT_USER,
#ifdef EV_TRIGGER
		.flags = EV_TRIGGER,
#endif
#ifdef NOTE_TRIGGER
		.fflags = NOTE_TRIGGER,
#endif
	};

	_dispatch_debug("waking up the _dispatch_mgr_q: %p", dq);

	_dispatch_update_kq(&kev);

	return false;
}

void
_dispatch_update_kq(const struct kevent *kev)
{
	struct kevent kev_copy = *kev;
	kev_copy.flags |= EV_RECEIPT;

	if (kev_copy.flags & EV_DELETE) {
		switch (kev_copy.filter) {
		case EVFILT_READ:
			if (FD_ISSET((int)kev_copy.ident, &_dispatch_rfds)) {
				FD_CLR((int)kev_copy.ident, &_dispatch_rfds);
				_dispatch_rfd_ptrs[kev_copy.ident] = 0;
				return;
			}
		case EVFILT_WRITE:
			if (FD_ISSET((int)kev_copy.ident, &_dispatch_wfds)) {
				FD_CLR((int)kev_copy.ident, &_dispatch_wfds);
				_dispatch_wfd_ptrs[kev_copy.ident] = 0;
				return;
			}
		default:
			break;
		}
	}
	
	int rval = kevent(_dispatch_get_kq(), &kev_copy, 1, &kev_copy, 1, NULL);
	if (rval == -1) { 
		// If we fail to register with kevents, for other reasons aside from
		// changelist elements.
		dispatch_assume_zero(errno);
		//kev_copy.flags |= EV_ERROR;
		//kev_copy.data = error;
		return;
	}

	// The following select workaround only applies to adding kevents
	if (!(kev->flags & EV_ADD)) {
		return;
	}

	switch (kev_copy.data) {
	case 0:
		return;
	case EBADF:
		break;
	default:
		// If an error occurred while registering with kevent, and it was 
		// because of a kevent changelist processing && the kevent involved
		// either doing a read or write, it would indicate we were trying
		// to register a /dev/* port; fall back to select
		switch (kev_copy.filter) {
		case EVFILT_READ:
			_dispatch_select_workaround = true;
			FD_SET((int)kev_copy.ident, &_dispatch_rfds);
			_dispatch_rfd_ptrs[kev_copy.ident] = kev_copy.udata;
			break;
		case EVFILT_WRITE:
			_dispatch_select_workaround = true;
			FD_SET((int)kev_copy.ident, &_dispatch_wfds);
			_dispatch_wfd_ptrs[kev_copy.ident] = kev_copy.udata;
			break;
		default:
			_dispatch_source_drain_kevent(&kev_copy); 
			break;
		}
		break;
	}
}

static const struct dispatch_queue_vtable_s _dispatch_queue_mgr_vtable = {
	.do_type = DISPATCH_QUEUE_MGR_TYPE,
	.do_kind = "mgr-queue",
	.do_invoke = _dispatch_mgr_invoke,
	.do_debug = dispatch_queue_debug,
	.do_probe = _dispatch_mgr_wakeup,
};

// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
struct dispatch_queue_s _dispatch_mgr_q = {
	.do_vtable = &_dispatch_queue_mgr_vtable,
	.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
	.do_targetq = &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_COUNT - 1],

	.dq_label = "com.apple.libdispatch-manager",
	.dq_width = 1,
	.dq_serialnum = 2,
};

const struct dispatch_queue_offsets_s dispatch_queue_offsets = {
	.dqo_version = 3,
	.dqo_label = offsetof(struct dispatch_queue_s, dq_label),
	.dqo_label_size = sizeof(_dispatch_main_q.dq_label),
	.dqo_flags = 0,
	.dqo_flags_size = 0,
	.dqo_width = offsetof(struct dispatch_queue_s, dq_width),
	.dqo_width_size = sizeof(_dispatch_main_q.dq_width),
	.dqo_serialnum = offsetof(struct dispatch_queue_s, dq_serialnum),
	.dqo_serialnum_size = sizeof(_dispatch_main_q.dq_serialnum),
	.dqo_running = offsetof(struct dispatch_queue_s, dq_running),
	.dqo_running_size = sizeof(_dispatch_main_q.dq_running),
};

#ifdef __BLOCKS__
void
dispatch_after(dispatch_time_t when, dispatch_queue_t queue, dispatch_block_t work)
{
	// test before the copy of the block
	if (when == DISPATCH_TIME_FOREVER) {
#if DISPATCH_DEBUG
		DISPATCH_CLIENT_CRASH("dispatch_after() called with 'when' == infinity");
#endif
		return;
	}
	dispatch_after_f(when, queue, _dispatch_Block_copy(work), _dispatch_call_block_and_release);
}
#endif

DISPATCH_NOINLINE
void
dispatch_after_f(dispatch_time_t when, dispatch_queue_t queue, void *ctxt, void (*func)(void *))
{
	uint64_t delta;
	if (when == DISPATCH_TIME_FOREVER) {
#if DISPATCH_DEBUG
		DISPATCH_CLIENT_CRASH("dispatch_after_f() called with 'when' == infinity");
#endif
		return;
	}

	// this function can and should be optimized to not use a dispatch source
again:
	delta = _dispatch_timeout(when);
	if (delta == 0) {
		return dispatch_async_f(queue, ctxt, func);
	}
	if (!dispatch_source_timer_create(DISPATCH_TIMER_INTERVAL, delta, 0, NULL, queue, ^(dispatch_source_t ds) {
		long err_dom, err_val;
		if ((err_dom = dispatch_source_get_error(ds, &err_val))) {
			dispatch_assert(err_dom == DISPATCH_ERROR_DOMAIN_POSIX);
			dispatch_assert(err_val == ECANCELED);
			func(ctxt);
			dispatch_release(ds);	// MUST NOT be _dispatch_release()
		} else {
			dispatch_source_cancel(ds);
		}
	})) {
		goto again;
	}
}
