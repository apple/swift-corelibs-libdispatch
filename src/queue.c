/*
 * Copyright (c) 2008-2011 Apple Inc. All rights reserved.
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
#endif

#if (!HAVE_PTHREAD_WORKQUEUES || DISPATCH_DEBUG) && \
		!defined(DISPATCH_ENABLE_THREAD_POOL)
#define DISPATCH_ENABLE_THREAD_POOL 1
#endif
#if DISPATCH_ENABLE_THREAD_POOL && !DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
#define pthread_workqueue_t void*
#endif

static void _dispatch_cache_cleanup(void *value);
static void _dispatch_async_f_redirect(dispatch_queue_t dq,
		dispatch_continuation_t dc);
static void _dispatch_queue_cleanup(void *ctxt);
static inline void _dispatch_queue_wakeup_global2(dispatch_queue_t dq,
		unsigned int n);
static inline void _dispatch_queue_wakeup_global(dispatch_queue_t dq);
static _dispatch_thread_semaphore_t _dispatch_queue_drain(dispatch_queue_t dq);
static inline _dispatch_thread_semaphore_t
		_dispatch_queue_drain_one_barrier_sync(dispatch_queue_t dq);
#if DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
static void _dispatch_worker_thread3(void *context);
#endif
#if HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
static void _dispatch_worker_thread2(int priority, int options, void *context);
#endif
#if DISPATCH_ENABLE_THREAD_POOL
static void *_dispatch_worker_thread(void *context);
static int _dispatch_pthread_sigmask(int how, sigset_t *set, sigset_t *oset);
#endif

#if DISPATCH_COCOA_COMPAT
static unsigned int _dispatch_worker_threads;
static dispatch_once_t _dispatch_main_q_port_pred;
static mach_port_t main_q_port;

static void _dispatch_main_q_port_init(void *ctxt);
static dispatch_queue_t _dispatch_queue_wakeup_main(void);
static void _dispatch_main_queue_drain(void);
#endif

#pragma mark -
#pragma mark dispatch_root_queue

#if DISPATCH_ENABLE_THREAD_POOL
static struct dispatch_semaphore_s _dispatch_thread_mediator[] = {
	[DISPATCH_ROOT_QUEUE_IDX_LOW_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(semaphore),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
	[DISPATCH_ROOT_QUEUE_IDX_LOW_OVERCOMMIT_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(semaphore),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
	[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(semaphore),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
	[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_OVERCOMMIT_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(semaphore),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
	[DISPATCH_ROOT_QUEUE_IDX_HIGH_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(semaphore),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
	[DISPATCH_ROOT_QUEUE_IDX_HIGH_OVERCOMMIT_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(semaphore),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
	[DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(semaphore),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
	[DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_OVERCOMMIT_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(semaphore),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	},
};
#endif

#define MAX_THREAD_COUNT 255

struct dispatch_root_queue_context_s {
	union {
		struct {
			unsigned int volatile dgq_pending;
#if HAVE_PTHREAD_WORKQUEUES
			int dgq_wq_priority, dgq_wq_options;
#if DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK || DISPATCH_ENABLE_THREAD_POOL
			pthread_workqueue_t dgq_kworkqueue;
#endif
#endif // HAVE_PTHREAD_WORKQUEUES
#if DISPATCH_ENABLE_THREAD_POOL
			dispatch_semaphore_t dgq_thread_mediator;
			uint32_t dgq_thread_pool_size;
#endif
		};
		char _dgq_pad[DISPATCH_CACHELINE_SIZE];
	};
};

DISPATCH_CACHELINE_ALIGN
static struct dispatch_root_queue_context_s _dispatch_root_queue_contexts[] = {
	[DISPATCH_ROOT_QUEUE_IDX_LOW_PRIORITY] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_wq_priority = WORKQ_LOW_PRIOQUEUE,
		.dgq_wq_options = 0,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_thread_mediator = &_dispatch_thread_mediator[
				DISPATCH_ROOT_QUEUE_IDX_LOW_PRIORITY],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_LOW_OVERCOMMIT_PRIORITY] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_wq_priority = WORKQ_LOW_PRIOQUEUE,
		.dgq_wq_options = WORKQ_ADDTHREADS_OPTION_OVERCOMMIT,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_thread_mediator = &_dispatch_thread_mediator[
				DISPATCH_ROOT_QUEUE_IDX_LOW_OVERCOMMIT_PRIORITY],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_PRIORITY] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_wq_priority = WORKQ_DEFAULT_PRIOQUEUE,
		.dgq_wq_options = 0,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_thread_mediator = &_dispatch_thread_mediator[
				DISPATCH_ROOT_QUEUE_IDX_DEFAULT_PRIORITY],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_OVERCOMMIT_PRIORITY] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_wq_priority = WORKQ_DEFAULT_PRIOQUEUE,
		.dgq_wq_options = WORKQ_ADDTHREADS_OPTION_OVERCOMMIT,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_thread_mediator = &_dispatch_thread_mediator[
				DISPATCH_ROOT_QUEUE_IDX_DEFAULT_OVERCOMMIT_PRIORITY],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_HIGH_PRIORITY] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_wq_priority = WORKQ_HIGH_PRIOQUEUE,
		.dgq_wq_options = 0,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_thread_mediator = &_dispatch_thread_mediator[
				DISPATCH_ROOT_QUEUE_IDX_HIGH_PRIORITY],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_HIGH_OVERCOMMIT_PRIORITY] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_wq_priority = WORKQ_HIGH_PRIOQUEUE,
		.dgq_wq_options = WORKQ_ADDTHREADS_OPTION_OVERCOMMIT,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_thread_mediator = &_dispatch_thread_mediator[
				DISPATCH_ROOT_QUEUE_IDX_HIGH_OVERCOMMIT_PRIORITY],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_PRIORITY] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_wq_priority = WORKQ_BG_PRIOQUEUE,
		.dgq_wq_options = 0,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_thread_mediator = &_dispatch_thread_mediator[
				DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_PRIORITY],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_OVERCOMMIT_PRIORITY] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_wq_priority = WORKQ_BG_PRIOQUEUE,
		.dgq_wq_options = WORKQ_ADDTHREADS_OPTION_OVERCOMMIT,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_thread_mediator = &_dispatch_thread_mediator[
				DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_OVERCOMMIT_PRIORITY],
		.dgq_thread_pool_size = MAX_THREAD_COUNT,
#endif
	}}},
};

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
// dq_running is set to 2 so that barrier operations go through the slow path
DISPATCH_CACHELINE_ALIGN
struct dispatch_queue_s _dispatch_root_queues[] = {
	[DISPATCH_ROOT_QUEUE_IDX_LOW_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(queue_root),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_LOW_PRIORITY],
		.dq_label = "com.apple.root.low-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 4,
	},
	[DISPATCH_ROOT_QUEUE_IDX_LOW_OVERCOMMIT_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(queue_root),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_LOW_OVERCOMMIT_PRIORITY],
		.dq_label = "com.apple.root.low-overcommit-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 5,
	},
	[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(queue_root),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_DEFAULT_PRIORITY],
		.dq_label = "com.apple.root.default-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 6,
	},
	[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_OVERCOMMIT_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(queue_root),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_DEFAULT_OVERCOMMIT_PRIORITY],
		.dq_label = "com.apple.root.default-overcommit-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 7,
	},
	[DISPATCH_ROOT_QUEUE_IDX_HIGH_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(queue_root),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_HIGH_PRIORITY],
		.dq_label = "com.apple.root.high-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 8,
	},
	[DISPATCH_ROOT_QUEUE_IDX_HIGH_OVERCOMMIT_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(queue_root),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_HIGH_OVERCOMMIT_PRIORITY],
		.dq_label = "com.apple.root.high-overcommit-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 9,
	},
	[DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(queue_root),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_PRIORITY],
		.dq_label = "com.apple.root.background-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 10,
	},
	[DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_OVERCOMMIT_PRIORITY] = {
		.do_vtable = DISPATCH_VTABLE(queue_root),
		.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
		.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
		.do_ctxt = &_dispatch_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_OVERCOMMIT_PRIORITY],
		.dq_label = "com.apple.root.background-overcommit-priority",
		.dq_running = 2,
		.dq_width = UINT32_MAX,
		.dq_serialnum = 11,
	},
};

#if HAVE_PTHREAD_WORKQUEUES
static const dispatch_queue_t _dispatch_wq2root_queues[][2] = {
	[WORKQ_LOW_PRIOQUEUE][0] = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_LOW_PRIORITY],
	[WORKQ_LOW_PRIOQUEUE][WORKQ_ADDTHREADS_OPTION_OVERCOMMIT] =
			&_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_LOW_OVERCOMMIT_PRIORITY],
	[WORKQ_DEFAULT_PRIOQUEUE][0] = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_DEFAULT_PRIORITY],
	[WORKQ_DEFAULT_PRIOQUEUE][WORKQ_ADDTHREADS_OPTION_OVERCOMMIT] =
			&_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_DEFAULT_OVERCOMMIT_PRIORITY],
	[WORKQ_HIGH_PRIOQUEUE][0] = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_HIGH_PRIORITY],
	[WORKQ_HIGH_PRIOQUEUE][WORKQ_ADDTHREADS_OPTION_OVERCOMMIT] =
			&_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_HIGH_OVERCOMMIT_PRIORITY],
	[WORKQ_BG_PRIOQUEUE][0] = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_PRIORITY],
	[WORKQ_BG_PRIOQUEUE][WORKQ_ADDTHREADS_OPTION_OVERCOMMIT] =
			&_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_OVERCOMMIT_PRIORITY],
};
#endif // HAVE_PTHREAD_WORKQUEUES

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
DISPATCH_CACHELINE_ALIGN
struct dispatch_queue_s _dispatch_mgr_q = {
	.do_vtable = DISPATCH_VTABLE(queue_mgr),
	.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
	.do_targetq = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_HIGH_OVERCOMMIT_PRIORITY],
	.dq_label = "com.apple.libdispatch-manager",
	.dq_width = 1,
	.dq_serialnum = 2,
};

dispatch_queue_t
dispatch_get_global_queue(long priority, unsigned long flags)
{
	if (flags & ~DISPATCH_QUEUE_OVERCOMMIT) {
		return NULL;
	}
	return _dispatch_get_root_queue(priority,
			flags & DISPATCH_QUEUE_OVERCOMMIT);
}

dispatch_queue_t
dispatch_get_current_queue(void)
{
	return _dispatch_queue_get_current() ?: _dispatch_get_root_queue(0, true);
}

#pragma mark -
#pragma mark dispatch_init

static void
_dispatch_hw_config_init(void)
{
	_dispatch_hw_config.cc_max_active = _dispatch_get_activecpu();
	_dispatch_hw_config.cc_max_logical = _dispatch_get_logicalcpu_max();
	_dispatch_hw_config.cc_max_physical = _dispatch_get_physicalcpu_max();
}

static inline bool
_dispatch_root_queues_init_workq(void)
{
	bool result = false;
#if HAVE_PTHREAD_WORKQUEUES
	bool disable_wq = false;
#if DISPATCH_ENABLE_THREAD_POOL
	disable_wq = slowpath(getenv("LIBDISPATCH_DISABLE_KWQ"));
#endif
	int r;
#if HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
	if (!disable_wq) {
		r = pthread_workqueue_setdispatch_np(_dispatch_worker_thread2);
#if !DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
		(void)dispatch_assume_zero(r);
#endif
		result = !r;
	}
#endif // HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
#if DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK || DISPATCH_ENABLE_THREAD_POOL
	if (!result) {
#if DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
		pthread_workqueue_attr_t pwq_attr;
		if (!disable_wq) {
			r = pthread_workqueue_attr_init_np(&pwq_attr);
			(void)dispatch_assume_zero(r);
		}
#endif
		int i;
		for (i = 0; i < DISPATCH_ROOT_QUEUE_COUNT; i++) {
			pthread_workqueue_t pwq = NULL;
			struct dispatch_root_queue_context_s *qc =
					&_dispatch_root_queue_contexts[i];
#if DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
			if (!disable_wq
#if DISPATCH_NO_BG_PRIORITY
					&& (qc->dgq_wq_priority != WORKQ_BG_PRIOQUEUE)
#endif
			) {
				r = pthread_workqueue_attr_setqueuepriority_np(&pwq_attr,
						qc->dgq_wq_priority);
				(void)dispatch_assume_zero(r);
				r = pthread_workqueue_attr_setovercommit_np(&pwq_attr,
						qc->dgq_wq_options & WORKQ_ADDTHREADS_OPTION_OVERCOMMIT);
				(void)dispatch_assume_zero(r);
				r = pthread_workqueue_create_np(&pwq, &pwq_attr);
				(void)dispatch_assume_zero(r);
				result = result || dispatch_assume(pwq);
			}
#endif
			qc->dgq_kworkqueue = pwq ? pwq : (void*)(~0ul);
		}
#if DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
		if (!disable_wq) {
			r = pthread_workqueue_attr_destroy_np(&pwq_attr);
			(void)dispatch_assume_zero(r);
		}
#endif
	}
#endif // DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK || DISPATCH_ENABLE_THREAD_POOL
#endif // HAVE_PTHREAD_WORKQUEUES
	return result;
}

static inline void
_dispatch_root_queues_init_thread_pool(void)
{
#if DISPATCH_ENABLE_THREAD_POOL
	int i;
	for (i = 0; i < DISPATCH_ROOT_QUEUE_COUNT; i++) {
#if TARGET_OS_EMBEDDED
		// some software hangs if the non-overcommitting queues do not
		// overcommit when threads block. Someday, this behavior should apply
		// to all platforms
		if (!(i & 1)) {
			_dispatch_root_queue_contexts[i].dgq_thread_pool_size =
					_dispatch_hw_config.cc_max_active;
		}
#endif
#if USE_MACH_SEM
		// override the default FIFO behavior for the pool semaphores
		kern_return_t kr = semaphore_create(mach_task_self(),
				&_dispatch_thread_mediator[i].dsema_port, SYNC_POLICY_LIFO, 0);
		DISPATCH_VERIFY_MIG(kr);
		(void)dispatch_assume_zero(kr);
		(void)dispatch_assume(_dispatch_thread_mediator[i].dsema_port);
#elif USE_POSIX_SEM
		/* XXXRW: POSIX semaphores don't support LIFO? */
		int ret = sem_init(&_dispatch_thread_mediator[i].dsema_sem, 0, 0);
		(void)dispatch_assume_zero(ret);
#endif
	}
#else
	DISPATCH_CRASH("Thread pool creation failed");
#endif // DISPATCH_ENABLE_THREAD_POOL
}

static void
_dispatch_root_queues_init(void *context DISPATCH_UNUSED)
{
	_dispatch_safe_fork = false;
	if (!_dispatch_root_queues_init_workq()) {
		_dispatch_root_queues_init_thread_pool();
	}

}

#define countof(x) (sizeof(x) / sizeof(x[0]))

DISPATCH_EXPORT DISPATCH_NOTHROW
void
libdispatch_init(void)
{
	dispatch_assert(DISPATCH_QUEUE_PRIORITY_COUNT == 4);
	dispatch_assert(DISPATCH_ROOT_QUEUE_COUNT == 8);

	dispatch_assert(DISPATCH_QUEUE_PRIORITY_LOW ==
			-DISPATCH_QUEUE_PRIORITY_HIGH);
	dispatch_assert(countof(_dispatch_root_queues) ==
			DISPATCH_ROOT_QUEUE_COUNT);
	dispatch_assert(countof(_dispatch_root_queue_contexts) ==
			DISPATCH_ROOT_QUEUE_COUNT);
#if HAVE_PTHREAD_WORKQUEUES
	dispatch_assert(sizeof(_dispatch_wq2root_queues) /
			sizeof(_dispatch_wq2root_queues[0][0]) ==
			DISPATCH_ROOT_QUEUE_COUNT);
#endif
#if DISPATCH_ENABLE_THREAD_POOL
	dispatch_assert(countof(_dispatch_thread_mediator) ==
			DISPATCH_ROOT_QUEUE_COUNT);
#endif

	dispatch_assert(sizeof(struct dispatch_apply_s) <=
			ROUND_UP_TO_CACHELINE_SIZE(sizeof(
			struct dispatch_continuation_s)));
	dispatch_assert(sizeof(struct dispatch_source_s) ==
			sizeof(struct dispatch_queue_s) - DISPATCH_QUEUE_CACHELINE_PAD);
	dispatch_assert(sizeof(struct dispatch_queue_s) % DISPATCH_CACHELINE_SIZE
			== 0);
	dispatch_assert(sizeof(struct dispatch_root_queue_context_s) %
			DISPATCH_CACHELINE_SIZE == 0);

	_dispatch_thread_key_create(&dispatch_queue_key, _dispatch_queue_cleanup);
	_dispatch_thread_key_create(&dispatch_sema4_key,
			(void (*)(void *))_dispatch_thread_semaphore_dispose);
	_dispatch_thread_key_create(&dispatch_cache_key, _dispatch_cache_cleanup);
	_dispatch_thread_key_create(&dispatch_io_key, NULL);
	_dispatch_thread_key_create(&dispatch_apply_key, NULL);
#if DISPATCH_PERF_MON
	_dispatch_thread_key_create(&dispatch_bcounter_key, NULL);
#endif

#if DISPATCH_USE_RESOLVERS // rdar://problem/8541707
	_dispatch_main_q.do_targetq = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_DEFAULT_OVERCOMMIT_PRIORITY];
#endif

	_dispatch_thread_setspecific(dispatch_queue_key, &_dispatch_main_q);

#if DISPATCH_USE_PTHREAD_ATFORK
	(void)dispatch_assume_zero(pthread_atfork(dispatch_atfork_prepare,
			dispatch_atfork_parent, dispatch_atfork_child));
#endif

	_dispatch_hw_config_init();
	_dispatch_vtable_init();
	_os_object_init();
}

DISPATCH_EXPORT DISPATCH_NOTHROW
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

#pragma mark -
#pragma mark dispatch_queue_t

// skip zero
// 1 - main_q
// 2 - mgr_q
// 3 - _unused_
// 4,5,6,7,8,9,10,11 - global queues
// we use 'xadd' on Intel, so the initial value == next assigned
unsigned long _dispatch_queue_serial_numbers = 12;

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
	dq = _dispatch_alloc(DISPATCH_VTABLE(queue),
			sizeof(struct dispatch_queue_s) - DISPATCH_QUEUE_MIN_LABEL_SIZE -
			DISPATCH_QUEUE_CACHELINE_PAD + label_len + 1);

	_dispatch_queue_init(dq);
	strcpy(dq->dq_label, label);

	if (fastpath(!attr)) {
		return dq;
	}
	if (fastpath(attr == DISPATCH_QUEUE_CONCURRENT)) {
		dq->dq_width = UINT32_MAX;
		dq->do_targetq = _dispatch_get_root_queue(0, false);
	} else {
		dispatch_debug_assert(!attr, "Invalid attribute");
	}
	return dq;
}

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
void
_dispatch_queue_dispose(dispatch_queue_t dq)
{
	if (slowpath(dq == _dispatch_queue_get_current())) {
		DISPATCH_CRASH("Release of a queue by itself");
	}
	if (slowpath(dq->dq_items_tail)) {
		DISPATCH_CRASH("Release of a queue while items are enqueued");
	}

	// trash the tail queue so that use after free will crash
	dq->dq_items_tail = (void *)0x200;

	dispatch_queue_t dqsq = dispatch_atomic_xchg2o(dq, dq_specific_q,
			(void *)0x200);
	if (dqsq) {
		_dispatch_release(dqsq);
	}
}

const char *
dispatch_queue_get_label(dispatch_queue_t dq)
{
	return dq->dq_label;
}

static void
_dispatch_queue_set_width2(void *ctxt)
{
	int w = (int)(intptr_t)ctxt; // intentional truncation
	uint32_t tmp;
	dispatch_queue_t dq = _dispatch_queue_get_current();

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
	// multiply by two since the running count is inc/dec by two
	// (the low bit == barrier)
	dq->dq_width = tmp * 2;
}

void
dispatch_queue_set_width(dispatch_queue_t dq, long width)
{
	if (slowpath(dq->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		return;
	}
	dispatch_barrier_async_f(dq, (void*)(intptr_t)width,
			_dispatch_queue_set_width2);
}

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
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
	dispatch_queue_t prev_dq;
	unsigned long type;

	if (slowpath(dou._do->do_xref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		return;
	}
	type = dx_type(dou._do) & _DISPATCH_META_TYPE_MASK;
	if (slowpath(!dq)) {
		bool is_concurrent_q = (type == _DISPATCH_QUEUE_TYPE &&
				slowpath(dou._dq->dq_width > 1));
		dq = _dispatch_get_root_queue(0, !is_concurrent_q);
	}
	// TODO: put into the vtable
	switch(type) {
	case _DISPATCH_QUEUE_TYPE:
	case _DISPATCH_SOURCE_TYPE:
		_dispatch_retain(dq);
		return dispatch_barrier_async_f(dou._dq, dq,
				_dispatch_set_target_queue2);
	case _DISPATCH_IO_TYPE:
		return _dispatch_io_set_target_queue(dou._dchannel, dq);
	default:
		_dispatch_retain(dq);
		dispatch_atomic_store_barrier();
		prev_dq = dispatch_atomic_xchg2o(dou._do, do_targetq, dq);
		if (prev_dq) _dispatch_release(prev_dq);
		return;
	}
}

void
dispatch_set_current_target_queue(dispatch_queue_t dq)
{
	dispatch_queue_t queue = _dispatch_queue_get_current();

	if (slowpath(!queue)) {
		DISPATCH_CLIENT_CRASH("SPI not called from a queue");
	}
	if (slowpath(queue->do_xref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		DISPATCH_CLIENT_CRASH("SPI not supported on this queue");
	}
	if (slowpath(queue->dq_width != 1)) {
		DISPATCH_CLIENT_CRASH("SPI not called from a serial queue");
	}
	if (slowpath(!dq)) {
		dq = _dispatch_get_root_queue(0, true);
	}
	_dispatch_retain(dq);
	_dispatch_set_target_queue2(dq);
}

#pragma mark -
#pragma mark dispatch_queue_specific

struct dispatch_queue_specific_queue_s {
	DISPATCH_STRUCT_HEADER(queue_specific_queue);
	DISPATCH_QUEUE_HEADER;
	union {
		char _dqsq_pad[DISPATCH_QUEUE_MIN_LABEL_SIZE];
		struct {
			char dq_label[16];
			TAILQ_HEAD(dispatch_queue_specific_head_s,
					dispatch_queue_specific_s) dqsq_contexts;
		};
	};
};

struct dispatch_queue_specific_s {
	const void *dqs_key;
	void *dqs_ctxt;
	dispatch_function_t dqs_destructor;
	TAILQ_ENTRY(dispatch_queue_specific_s) dqs_list;
};
DISPATCH_DECL(dispatch_queue_specific);

void
_dispatch_queue_specific_queue_dispose(dispatch_queue_specific_queue_t dqsq)
{
	dispatch_queue_specific_t dqs, tmp;

	TAILQ_FOREACH_SAFE(dqs, &dqsq->dqsq_contexts, dqs_list, tmp) {
		if (dqs->dqs_destructor) {
			dispatch_async_f(_dispatch_get_root_queue(
					DISPATCH_QUEUE_PRIORITY_DEFAULT, false), dqs->dqs_ctxt,
					dqs->dqs_destructor);
		}
		free(dqs);
	}
	_dispatch_queue_dispose((dispatch_queue_t)dqsq);
}

static void
_dispatch_queue_init_specific(dispatch_queue_t dq)
{
	dispatch_queue_specific_queue_t dqsq;

	dqsq = _dispatch_alloc(DISPATCH_VTABLE(queue_specific_queue),
			sizeof(struct dispatch_queue_specific_queue_s));
	_dispatch_queue_init((dispatch_queue_t)dqsq);
	dqsq->do_xref_cnt = -1;
	dqsq->do_targetq = _dispatch_get_root_queue(DISPATCH_QUEUE_PRIORITY_HIGH,
			true);
	dqsq->dq_width = UINT32_MAX;
	strlcpy(dqsq->dq_label, "queue-specific", sizeof(dqsq->dq_label));
	TAILQ_INIT(&dqsq->dqsq_contexts);
	dispatch_atomic_store_barrier();
	if (slowpath(!dispatch_atomic_cmpxchg2o(dq, dq_specific_q, NULL,
			(dispatch_queue_t)dqsq))) {
		_dispatch_release((dispatch_queue_t)dqsq);
	}
}

static void
_dispatch_queue_set_specific(void *ctxt)
{
	dispatch_queue_specific_t dqs, dqsn = ctxt;
	dispatch_queue_specific_queue_t dqsq =
			(dispatch_queue_specific_queue_t)_dispatch_queue_get_current();

	TAILQ_FOREACH(dqs, &dqsq->dqsq_contexts, dqs_list) {
		if (dqs->dqs_key == dqsn->dqs_key) {
			// Destroy previous context for existing key
			if (dqs->dqs_destructor) {
				dispatch_async_f(_dispatch_get_root_queue(
						DISPATCH_QUEUE_PRIORITY_DEFAULT, false), dqs->dqs_ctxt,
						dqs->dqs_destructor);
			}
			if (dqsn->dqs_ctxt) {
				// Copy new context for existing key
				dqs->dqs_ctxt = dqsn->dqs_ctxt;
				dqs->dqs_destructor = dqsn->dqs_destructor;
			} else {
				// Remove context storage for existing key
				TAILQ_REMOVE(&dqsq->dqsq_contexts, dqs, dqs_list);
				free(dqs);
			}
			return free(dqsn);
		}
	}
	// Insert context storage for new key
	TAILQ_INSERT_TAIL(&dqsq->dqsq_contexts, dqsn, dqs_list);
}

DISPATCH_NOINLINE
void
dispatch_queue_set_specific(dispatch_queue_t dq, const void *key,
	void *ctxt, dispatch_function_t destructor)
{
	if (slowpath(!key)) {
		return;
	}
	dispatch_queue_specific_t dqs;

	dqs = calloc(1, sizeof(struct dispatch_queue_specific_s));
	dqs->dqs_key = key;
	dqs->dqs_ctxt = ctxt;
	dqs->dqs_destructor = destructor;
	if (slowpath(!dq->dq_specific_q)) {
		_dispatch_queue_init_specific(dq);
	}
	dispatch_barrier_async_f(dq->dq_specific_q, dqs,
			_dispatch_queue_set_specific);
}

static void
_dispatch_queue_get_specific(void *ctxt)
{
	void **ctxtp = ctxt;
	void *key = *ctxtp;
	dispatch_queue_specific_queue_t dqsq =
			(dispatch_queue_specific_queue_t)_dispatch_queue_get_current();
	dispatch_queue_specific_t dqs;

	TAILQ_FOREACH(dqs, &dqsq->dqsq_contexts, dqs_list) {
		if (dqs->dqs_key == key) {
			*ctxtp = dqs->dqs_ctxt;
			return;
		}
	}
	*ctxtp = NULL;
}

DISPATCH_NOINLINE
void *
dispatch_queue_get_specific(dispatch_queue_t dq, const void *key)
{
	if (slowpath(!key)) {
		return NULL;
	}
	void *ctxt = NULL;

	if (fastpath(dq->dq_specific_q)) {
		ctxt = (void *)key;
		dispatch_sync_f(dq->dq_specific_q, &ctxt, _dispatch_queue_get_specific);
	}
	return ctxt;
}

DISPATCH_NOINLINE
void *
dispatch_get_specific(const void *key)
{
	if (slowpath(!key)) {
		return NULL;
	}
	void *ctxt = NULL;
	dispatch_queue_t dq = _dispatch_queue_get_current();

	while (slowpath(dq)) {
		if (slowpath(dq->dq_specific_q)) {
			ctxt = (void *)key;
			dispatch_sync_f(dq->dq_specific_q, &ctxt,
					_dispatch_queue_get_specific);
			if (ctxt) break;
		}
		dq = dq->do_targetq;
	}
	return ctxt;
}

#pragma mark -
#pragma mark dispatch_queue_debug

size_t
_dispatch_queue_debug_attr(dispatch_queue_t dq, char* buf, size_t bufsiz)
{
	dispatch_queue_t target = dq->do_targetq;
	return snprintf(buf, bufsiz, "target = %s[%p], width = 0x%x, "
			"running = 0x%x, barrier = %d ", target ? target->dq_label : "",
			target, dq->dq_width / 2, dq->dq_running / 2, dq->dq_running & 1);
}

size_t
dispatch_queue_debug(dispatch_queue_t dq, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	offset += snprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			dq->dq_label, dq);
	offset += _dispatch_object_debug_attr(dq, &buf[offset], bufsiz - offset);
	offset += _dispatch_queue_debug_attr(dq, &buf[offset], bufsiz - offset);
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

#if DISPATCH_PERF_MON
static OSSpinLock _dispatch_stats_lock;
static size_t _dispatch_bad_ratio;
static struct {
	uint64_t time_total;
	uint64_t count_total;
	uint64_t thread_total;
} _dispatch_stats[65]; // ffs*/fls*() returns zero when no bits are set

static void
_dispatch_queue_merge_stats(uint64_t start)
{
	uint64_t avg, delta = _dispatch_absolute_time() - start;
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

#pragma mark -
#pragma mark dispatch_continuation_t

static malloc_zone_t *_dispatch_ccache_zone;

static void
_dispatch_ccache_init(void *context DISPATCH_UNUSED)
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

	// This is also used for allocating struct dispatch_apply_s. If the
	// ROUND_UP behavior is changed, adjust the assert in libdispatch_init
	while (!(dc = fastpath(malloc_zone_calloc(_dispatch_ccache_zone, 1,
			ROUND_UP_TO_CACHELINE_SIZE(sizeof(*dc)))))) {
		sleep(1);
	}

	return dc;
}

static void
_dispatch_force_cache_cleanup(void)
{
	dispatch_continuation_t dc;
	dc = _dispatch_thread_getspecific(dispatch_cache_key);
	if (dc) {
		_dispatch_thread_setspecific(dispatch_cache_key, NULL);
		_dispatch_cache_cleanup(dc);
	}
}

// rdar://problem/11500155
void
dispatch_flush_continuation_cache(void)
{
	_dispatch_force_cache_cleanup();
}

DISPATCH_NOINLINE
static void
_dispatch_cache_cleanup(void *value)
{
	dispatch_continuation_t dc, next_dc = value;

	while ((dc = next_dc)) {
		next_dc = dc->do_next;
		malloc_zone_free(_dispatch_ccache_zone, dc);
	}
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_continuation_redirect(dispatch_queue_t dq, dispatch_object_t dou)
{
	dispatch_continuation_t dc = dou._dc;

	_dispatch_trace_continuation_pop(dq, dou);
	(void)dispatch_atomic_add2o(dq, dq_running, 2);
	if (!DISPATCH_OBJ_IS_VTABLE(dc) &&
			(long)dc->do_vtable & DISPATCH_OBJ_SYNC_SLOW_BIT) {
		dispatch_atomic_barrier();
		_dispatch_thread_semaphore_signal(
				(_dispatch_thread_semaphore_t)dc->dc_ctxt);
	} else {
		_dispatch_async_f_redirect(dq, dc);
	}
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_continuation_pop(dispatch_object_t dou)
{
	dispatch_continuation_t dc = dou._dc;
	dispatch_group_t dg;

	_dispatch_trace_continuation_pop(_dispatch_queue_get_current(), dou);
	if (DISPATCH_OBJ_IS_VTABLE(dou._do)) {
		return _dispatch_queue_invoke(dou._dq);
	}

	// Add the item back to the cache before calling the function. This
	// allows the 'hot' continuation to be used for a quick callback.
	//
	// The ccache version is per-thread.
	// Therefore, the object has not been reused yet.
	// This generates better assembly.
	if ((long)dc->do_vtable & DISPATCH_OBJ_ASYNC_BIT) {
		_dispatch_continuation_free(dc);
	}
	if ((long)dc->do_vtable & DISPATCH_OBJ_GROUP_BIT) {
		dg = dc->dc_data;
	} else {
		dg = NULL;
	}
	_dispatch_client_callout(dc->dc_ctxt, dc->dc_func);
	if (dg) {
		dispatch_group_leave(dg);
		_dispatch_release(dg);
	}
}

#pragma mark -
#pragma mark dispatch_barrier_async

DISPATCH_NOINLINE
static void
_dispatch_barrier_async_f_slow(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc_from_heap();

	dc->do_vtable = (void *)(DISPATCH_OBJ_ASYNC_BIT | DISPATCH_OBJ_BARRIER_BIT);
	dc->dc_func = func;
	dc->dc_ctxt = ctxt;

	_dispatch_queue_push(dq, dc);
}

DISPATCH_NOINLINE
void
dispatch_barrier_async_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	dispatch_continuation_t dc;

	dc = fastpath(_dispatch_continuation_alloc_cacheonly());
	if (!dc) {
		return _dispatch_barrier_async_f_slow(dq, ctxt, func);
	}

	dc->do_vtable = (void *)(DISPATCH_OBJ_ASYNC_BIT | DISPATCH_OBJ_BARRIER_BIT);
	dc->dc_func = func;
	dc->dc_ctxt = ctxt;

	_dispatch_queue_push(dq, dc);
}

#ifdef __BLOCKS__
void
dispatch_barrier_async(dispatch_queue_t dq, void (^work)(void))
{
	dispatch_barrier_async_f(dq, _dispatch_Block_copy(work),
			_dispatch_call_block_and_release);
}
#endif

#pragma mark -
#pragma mark dispatch_async

static void
_dispatch_async_f_redirect_invoke(void *_ctxt)
{
	struct dispatch_continuation_s *dc = _ctxt;
	struct dispatch_continuation_s *other_dc = dc->dc_other;
	dispatch_queue_t old_dq, dq = dc->dc_data, rq;

	old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	_dispatch_thread_setspecific(dispatch_queue_key, dq);
	_dispatch_continuation_pop(other_dc);
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);

	rq = dq->do_targetq;
	while (slowpath(rq->do_targetq) && rq != old_dq) {
		if (dispatch_atomic_sub2o(rq, dq_running, 2) == 0) {
			_dispatch_wakeup(rq);
		}
		rq = rq->do_targetq;
	}

	if (dispatch_atomic_sub2o(dq, dq_running, 2) == 0) {
		_dispatch_wakeup(dq);
	}
	_dispatch_release(dq);
}

DISPATCH_NOINLINE
static void
_dispatch_async_f2_slow(dispatch_queue_t dq, dispatch_continuation_t dc)
{
	_dispatch_wakeup(dq);
	_dispatch_queue_push(dq, dc);
}

DISPATCH_NOINLINE
static void
_dispatch_async_f_redirect(dispatch_queue_t dq,
		dispatch_continuation_t other_dc)
{
	dispatch_continuation_t dc;
	dispatch_queue_t rq;

	_dispatch_retain(dq);

	dc = _dispatch_continuation_alloc();

	dc->do_vtable = (void *)DISPATCH_OBJ_ASYNC_BIT;
	dc->dc_func = _dispatch_async_f_redirect_invoke;
	dc->dc_ctxt = dc;
	dc->dc_data = dq;
	dc->dc_other = other_dc;

	// Find the queue to redirect to
	rq = dq->do_targetq;
	while (slowpath(rq->do_targetq)) {
		uint32_t running;

		if (slowpath(rq->dq_items_tail) ||
				slowpath(DISPATCH_OBJECT_SUSPENDED(rq)) ||
				slowpath(rq->dq_width == 1)) {
			break;
		}
		running = dispatch_atomic_add2o(rq, dq_running, 2) - 2;
		if (slowpath(running & 1) || slowpath(running + 2 > rq->dq_width)) {
			if (slowpath(dispatch_atomic_sub2o(rq, dq_running, 2) == 0)) {
				return _dispatch_async_f2_slow(rq, dc);
			}
			break;
		}
		rq = rq->do_targetq;
	}
	_dispatch_queue_push(rq, dc);
}

DISPATCH_NOINLINE
static void
_dispatch_async_f2(dispatch_queue_t dq, dispatch_continuation_t dc)
{
	uint32_t running;
	bool locked;

	do {
		if (slowpath(dq->dq_items_tail)
				|| slowpath(DISPATCH_OBJECT_SUSPENDED(dq))) {
			break;
		}
		running = dispatch_atomic_add2o(dq, dq_running, 2);
		if (slowpath(running > dq->dq_width)) {
			if (slowpath(dispatch_atomic_sub2o(dq, dq_running, 2) == 0)) {
				return _dispatch_async_f2_slow(dq, dc);
			}
			break;
		}
		locked = running & 1;
		if (fastpath(!locked)) {
			return _dispatch_async_f_redirect(dq, dc);
		}
		locked = dispatch_atomic_sub2o(dq, dq_running, 2) & 1;
		// We might get lucky and find that the barrier has ended by now
	} while (!locked);

	_dispatch_queue_push(dq, dc);
}

DISPATCH_NOINLINE
static void
_dispatch_async_f_slow(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc_from_heap();

	dc->do_vtable = (void *)DISPATCH_OBJ_ASYNC_BIT;
	dc->dc_func = func;
	dc->dc_ctxt = ctxt;

	// No fastpath/slowpath hint because we simply don't know
	if (dq->do_targetq) {
		return _dispatch_async_f2(dq, dc);
	}

	_dispatch_queue_push(dq, dc);
}

DISPATCH_NOINLINE
void
dispatch_async_f(dispatch_queue_t dq, void *ctxt, dispatch_function_t func)
{
	dispatch_continuation_t dc;

	// No fastpath/slowpath hint because we simply don't know
	if (dq->dq_width == 1) {
		return dispatch_barrier_async_f(dq, ctxt, func);
	}

	dc = fastpath(_dispatch_continuation_alloc_cacheonly());
	if (!dc) {
		return _dispatch_async_f_slow(dq, ctxt, func);
	}

	dc->do_vtable = (void *)DISPATCH_OBJ_ASYNC_BIT;
	dc->dc_func = func;
	dc->dc_ctxt = ctxt;

	// No fastpath/slowpath hint because we simply don't know
	if (dq->do_targetq) {
		return _dispatch_async_f2(dq, dc);
	}

	_dispatch_queue_push(dq, dc);
}

#ifdef __BLOCKS__
void
dispatch_async(dispatch_queue_t dq, void (^work)(void))
{
	dispatch_async_f(dq, _dispatch_Block_copy(work),
			_dispatch_call_block_and_release);
}
#endif

#pragma mark -
#pragma mark dispatch_group_async

DISPATCH_NOINLINE
void
dispatch_group_async_f(dispatch_group_t dg, dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	dispatch_continuation_t dc;

	_dispatch_retain(dg);
	dispatch_group_enter(dg);

	dc = _dispatch_continuation_alloc();

	dc->do_vtable = (void *)(DISPATCH_OBJ_ASYNC_BIT | DISPATCH_OBJ_GROUP_BIT);
	dc->dc_func = func;
	dc->dc_ctxt = ctxt;
	dc->dc_data = dg;

	// No fastpath/slowpath hint because we simply don't know
	if (dq->dq_width != 1 && dq->do_targetq) {
		return _dispatch_async_f2(dq, dc);
	}

	_dispatch_queue_push(dq, dc);
}

#ifdef __BLOCKS__
void
dispatch_group_async(dispatch_group_t dg, dispatch_queue_t dq,
		dispatch_block_t db)
{
	dispatch_group_async_f(dg, dq, _dispatch_Block_copy(db),
			_dispatch_call_block_and_release);
}
#endif

#pragma mark -
#pragma mark dispatch_function_invoke

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_function_invoke(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	dispatch_queue_t old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	_dispatch_thread_setspecific(dispatch_queue_key, dq);
	_dispatch_client_callout(ctxt, func);
	_dispatch_workitem_inc();
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);
}

struct dispatch_function_recurse_s {
	dispatch_queue_t dfr_dq;
	void* dfr_ctxt;
	dispatch_function_t dfr_func;
};

static void
_dispatch_function_recurse_invoke(void *ctxt)
{
	struct dispatch_function_recurse_s *dfr = ctxt;
	_dispatch_function_invoke(dfr->dfr_dq, dfr->dfr_ctxt, dfr->dfr_func);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_function_recurse(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	struct dispatch_function_recurse_s dfr = {
		.dfr_dq = dq,
		.dfr_func = func,
		.dfr_ctxt = ctxt,
	};
	dispatch_sync_f(dq->do_targetq, &dfr, _dispatch_function_recurse_invoke);
}

#pragma mark -
#pragma mark dispatch_barrier_sync

struct dispatch_barrier_sync_slow_s {
	DISPATCH_CONTINUATION_HEADER(barrier_sync_slow);
};

struct dispatch_barrier_sync_slow2_s {
	dispatch_queue_t dbss2_dq;
#if DISPATCH_COCOA_COMPAT
	dispatch_function_t dbss2_func;
	void *dbss2_ctxt;
#endif
	_dispatch_thread_semaphore_t dbss2_sema;
};

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline _dispatch_thread_semaphore_t
_dispatch_barrier_sync_f_pop(dispatch_queue_t dq, dispatch_object_t dou,
		bool lock)
{
	dispatch_continuation_t dc = dou._dc;

	if (DISPATCH_OBJ_IS_VTABLE(dc) || ((long)dc->do_vtable &
			(DISPATCH_OBJ_BARRIER_BIT | DISPATCH_OBJ_SYNC_SLOW_BIT)) !=
			(DISPATCH_OBJ_BARRIER_BIT | DISPATCH_OBJ_SYNC_SLOW_BIT)) {
		return 0;
	}
	_dispatch_trace_continuation_pop(dq, dc);
	_dispatch_workitem_inc();

	struct dispatch_barrier_sync_slow_s *dbssp = (void *)dc;
	struct dispatch_barrier_sync_slow2_s *dbss2 = dbssp->dc_ctxt;
	if (lock) {
		(void)dispatch_atomic_add2o(dbss2->dbss2_dq, do_suspend_cnt,
				DISPATCH_OBJECT_SUSPEND_INTERVAL);
		// rdar://problem/9032024 running lock must be held until sync_f_slow
		// returns
		(void)dispatch_atomic_add2o(dbss2->dbss2_dq, dq_running, 2);
	}
	return dbss2->dbss2_sema ? dbss2->dbss2_sema : MACH_PORT_DEAD;
}

static void
_dispatch_barrier_sync_f_slow_invoke(void *ctxt)
{
	struct dispatch_barrier_sync_slow2_s *dbss2 = ctxt;

	dispatch_assert(dbss2->dbss2_dq == _dispatch_queue_get_current());
#if DISPATCH_COCOA_COMPAT
	// When the main queue is bound to the main thread
	if (dbss2->dbss2_dq == &_dispatch_main_q && pthread_main_np()) {
		dbss2->dbss2_func(dbss2->dbss2_ctxt);
		dbss2->dbss2_func = NULL;
		dispatch_atomic_barrier();
		_dispatch_thread_semaphore_signal(dbss2->dbss2_sema);
		return;
	}
#endif
	(void)dispatch_atomic_add2o(dbss2->dbss2_dq, do_suspend_cnt,
			DISPATCH_OBJECT_SUSPEND_INTERVAL);
	// rdar://9032024 running lock must be held until sync_f_slow returns
	(void)dispatch_atomic_add2o(dbss2->dbss2_dq, dq_running, 2);
	dispatch_atomic_barrier();
	_dispatch_thread_semaphore_signal(dbss2->dbss2_sema);
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_f_slow(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	// It's preferred to execute synchronous blocks on the current thread
	// due to thread-local side effects, garbage collection, etc. However,
	// blocks submitted to the main thread MUST be run on the main thread

	struct dispatch_barrier_sync_slow2_s dbss2 = {
		.dbss2_dq = dq,
#if DISPATCH_COCOA_COMPAT
		.dbss2_func = func,
		.dbss2_ctxt = ctxt,
#endif
		.dbss2_sema = _dispatch_get_thread_semaphore(),
	};
	struct dispatch_barrier_sync_slow_s dbss = {
		.do_vtable = (void *)(DISPATCH_OBJ_BARRIER_BIT |
				DISPATCH_OBJ_SYNC_SLOW_BIT),
		.dc_func = _dispatch_barrier_sync_f_slow_invoke,
		.dc_ctxt = &dbss2,
	};
	_dispatch_queue_push(dq, (void *)&dbss);

	_dispatch_thread_semaphore_wait(dbss2.dbss2_sema);
	_dispatch_put_thread_semaphore(dbss2.dbss2_sema);

#if DISPATCH_COCOA_COMPAT
	// Main queue bound to main thread
	if (dbss2.dbss2_func == NULL) {
		return;
	}
#endif
	dispatch_atomic_acquire_barrier();
	if (slowpath(dq->do_targetq) && slowpath(dq->do_targetq->do_targetq)) {
		_dispatch_function_recurse(dq, ctxt, func);
	} else {
		_dispatch_function_invoke(dq, ctxt, func);
	}
	dispatch_atomic_release_barrier();
	if (fastpath(dq->do_suspend_cnt < 2 * DISPATCH_OBJECT_SUSPEND_INTERVAL) &&
			dq->dq_running == 2) {
		// rdar://problem/8290662 "lock transfer"
		_dispatch_thread_semaphore_t sema;
		sema = _dispatch_queue_drain_one_barrier_sync(dq);
		if (sema) {
			_dispatch_thread_semaphore_signal(sema);
			return;
		}
	}
	(void)dispatch_atomic_sub2o(dq, do_suspend_cnt,
			DISPATCH_OBJECT_SUSPEND_INTERVAL);
	if (slowpath(dispatch_atomic_sub2o(dq, dq_running, 2) == 0)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_f2(dispatch_queue_t dq)
{
	if (!slowpath(DISPATCH_OBJECT_SUSPENDED(dq))) {
		// rdar://problem/8290662 "lock transfer"
		_dispatch_thread_semaphore_t sema;
		sema = _dispatch_queue_drain_one_barrier_sync(dq);
		if (sema) {
			(void)dispatch_atomic_add2o(dq, do_suspend_cnt,
					DISPATCH_OBJECT_SUSPEND_INTERVAL);
			// rdar://9032024 running lock must be held until sync_f_slow
			// returns: increment by 2 and decrement by 1
			(void)dispatch_atomic_inc2o(dq, dq_running);
			_dispatch_thread_semaphore_signal(sema);
			return;
		}
	}
	if (slowpath(dispatch_atomic_dec2o(dq, dq_running) == 0)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_f_invoke(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	dispatch_atomic_acquire_barrier();
	_dispatch_function_invoke(dq, ctxt, func);
	dispatch_atomic_release_barrier();
	if (slowpath(dq->dq_items_tail)) {
		return _dispatch_barrier_sync_f2(dq);
	}
	if (slowpath(dispatch_atomic_dec2o(dq, dq_running) == 0)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_f_recurse(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	dispatch_atomic_acquire_barrier();
	_dispatch_function_recurse(dq, ctxt, func);
	dispatch_atomic_release_barrier();
	if (slowpath(dq->dq_items_tail)) {
		return _dispatch_barrier_sync_f2(dq);
	}
	if (slowpath(dispatch_atomic_dec2o(dq, dq_running) == 0)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_NOINLINE
void
dispatch_barrier_sync_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	// 1) ensure that this thread hasn't enqueued anything ahead of this call
	// 2) the queue is not suspended
	if (slowpath(dq->dq_items_tail) || slowpath(DISPATCH_OBJECT_SUSPENDED(dq))){
		return _dispatch_barrier_sync_f_slow(dq, ctxt, func);
	}
	if (slowpath(!dispatch_atomic_cmpxchg2o(dq, dq_running, 0, 1))) {
		// global queues and main queue bound to main thread always falls into
		// the slow case
		return _dispatch_barrier_sync_f_slow(dq, ctxt, func);
	}
	if (slowpath(dq->do_targetq->do_targetq)) {
		return _dispatch_barrier_sync_f_recurse(dq, ctxt, func);
	}
	_dispatch_barrier_sync_f_invoke(dq, ctxt, func);
}

#ifdef __BLOCKS__
#if DISPATCH_COCOA_COMPAT
DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_slow(dispatch_queue_t dq, void (^work)(void))
{
	// Blocks submitted to the main queue MUST be run on the main thread,
	// therefore under GC we must Block_copy in order to notify the thread-local
	// garbage collector that the objects are transferring to the main thread
	// rdar://problem/7176237&7181849&7458685
	if (dispatch_begin_thread_4GC) {
		dispatch_block_t block = _dispatch_Block_copy(work);
		return dispatch_barrier_sync_f(dq, block,
				_dispatch_call_block_and_release);
	}
	struct Block_basic *bb = (void *)work;
	dispatch_barrier_sync_f(dq, work, (dispatch_function_t)bb->Block_invoke);
}
#endif

void
dispatch_barrier_sync(dispatch_queue_t dq, void (^work)(void))
{
#if DISPATCH_COCOA_COMPAT
	if (slowpath(dq == &_dispatch_main_q)) {
		return _dispatch_barrier_sync_slow(dq, work);
	}
#endif
	struct Block_basic *bb = (void *)work;
	dispatch_barrier_sync_f(dq, work, (dispatch_function_t)bb->Block_invoke);
}
#endif

#pragma mark -
#pragma mark dispatch_sync

DISPATCH_NOINLINE
static void
_dispatch_sync_f_slow(dispatch_queue_t dq, void *ctxt, dispatch_function_t func)
{
	_dispatch_thread_semaphore_t sema = _dispatch_get_thread_semaphore();
	struct dispatch_sync_slow_s {
		DISPATCH_CONTINUATION_HEADER(sync_slow);
	} dss = {
		.do_vtable = (void*)DISPATCH_OBJ_SYNC_SLOW_BIT,
		.dc_ctxt = (void*)sema,
	};
	_dispatch_queue_push(dq, (void *)&dss);

	_dispatch_thread_semaphore_wait(sema);
	_dispatch_put_thread_semaphore(sema);

	if (slowpath(dq->do_targetq->do_targetq)) {
		_dispatch_function_recurse(dq, ctxt, func);
	} else {
		_dispatch_function_invoke(dq, ctxt, func);
	}
	if (slowpath(dispatch_atomic_sub2o(dq, dq_running, 2) == 0)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_sync_f_slow2(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	if (slowpath(dispatch_atomic_sub2o(dq, dq_running, 2) == 0)) {
		_dispatch_wakeup(dq);
	}
	_dispatch_sync_f_slow(dq, ctxt, func);
}

DISPATCH_NOINLINE
static void
_dispatch_sync_f_invoke(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_function_invoke(dq, ctxt, func);
	if (slowpath(dispatch_atomic_sub2o(dq, dq_running, 2) == 0)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_sync_f_recurse(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_function_recurse(dq, ctxt, func);
	if (slowpath(dispatch_atomic_sub2o(dq, dq_running, 2) == 0)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_sync_f2(dispatch_queue_t dq, void *ctxt, dispatch_function_t func)
{
	// 1) ensure that this thread hasn't enqueued anything ahead of this call
	// 2) the queue is not suspended
	if (slowpath(dq->dq_items_tail) || slowpath(DISPATCH_OBJECT_SUSPENDED(dq))){
		return _dispatch_sync_f_slow(dq, ctxt, func);
	}
	if (slowpath(dispatch_atomic_add2o(dq, dq_running, 2) & 1)) {
		return _dispatch_sync_f_slow2(dq, ctxt, func);
	}
	if (slowpath(dq->do_targetq->do_targetq)) {
		return _dispatch_sync_f_recurse(dq, ctxt, func);
	}
	_dispatch_sync_f_invoke(dq, ctxt, func);
}

DISPATCH_NOINLINE
void
dispatch_sync_f(dispatch_queue_t dq, void *ctxt, dispatch_function_t func)
{
	if (fastpath(dq->dq_width == 1)) {
		return dispatch_barrier_sync_f(dq, ctxt, func);
	}
	if (slowpath(!dq->do_targetq)) {
		// the global root queues do not need strict ordering
		(void)dispatch_atomic_add2o(dq, dq_running, 2);
		return _dispatch_sync_f_invoke(dq, ctxt, func);
	}
	_dispatch_sync_f2(dq, ctxt, func);
}

#ifdef __BLOCKS__
#if DISPATCH_COCOA_COMPAT
DISPATCH_NOINLINE
static void
_dispatch_sync_slow(dispatch_queue_t dq, void (^work)(void))
{
	// Blocks submitted to the main queue MUST be run on the main thread,
	// therefore under GC we must Block_copy in order to notify the thread-local
	// garbage collector that the objects are transferring to the main thread
	// rdar://problem/7176237&7181849&7458685
	if (dispatch_begin_thread_4GC) {
		dispatch_block_t block = _dispatch_Block_copy(work);
		return dispatch_sync_f(dq, block, _dispatch_call_block_and_release);
	}
	struct Block_basic *bb = (void *)work;
	dispatch_sync_f(dq, work, (dispatch_function_t)bb->Block_invoke);
}
#endif

void
dispatch_sync(dispatch_queue_t dq, void (^work)(void))
{
#if DISPATCH_COCOA_COMPAT
	if (slowpath(dq == &_dispatch_main_q)) {
		return _dispatch_sync_slow(dq, work);
	}
#endif
	struct Block_basic *bb = (void *)work;
	dispatch_sync_f(dq, work, (dispatch_function_t)bb->Block_invoke);
}
#endif

#pragma mark -
#pragma mark dispatch_after

struct _dispatch_after_time_s {
	void *datc_ctxt;
	void (*datc_func)(void *);
	dispatch_source_t ds;
};

static void
_dispatch_after_timer_callback(void *ctxt)
{
	struct _dispatch_after_time_s *datc = ctxt;

	dispatch_assert(datc->datc_func);
	_dispatch_client_callout(datc->datc_ctxt, datc->datc_func);

	dispatch_source_t ds = datc->ds;
	free(datc);

	dispatch_source_cancel(ds); // Needed until 7287561 gets integrated
	dispatch_release(ds);
}

DISPATCH_NOINLINE
void
dispatch_after_f(dispatch_time_t when, dispatch_queue_t queue, void *ctxt,
		dispatch_function_t func)
{
	uint64_t delta;
	struct _dispatch_after_time_s *datc = NULL;
	dispatch_source_t ds;

	if (when == DISPATCH_TIME_FOREVER) {
#if DISPATCH_DEBUG
		DISPATCH_CLIENT_CRASH(
				"dispatch_after_f() called with 'when' == infinity");
#endif
		return;
	}

	// this function can and should be optimized to not use a dispatch source
	delta = _dispatch_timeout(when);
	if (delta == 0) {
		return dispatch_async_f(queue, ctxt, func);
	}
	// on successful creation, source owns malloc-ed context (which it frees in
	// the event handler)
	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
	dispatch_assert(ds);

	datc = malloc(sizeof(*datc));
	dispatch_assert(datc);
	datc->datc_ctxt = ctxt;
	datc->datc_func = func;
	datc->ds = ds;

	dispatch_set_context(ds, datc);
	dispatch_source_set_event_handler_f(ds, _dispatch_after_timer_callback);
	dispatch_source_set_timer(ds, when, DISPATCH_TIME_FOREVER, 0);
	dispatch_resume(ds);
}

#ifdef __BLOCKS__
void
dispatch_after(dispatch_time_t when, dispatch_queue_t queue,
		dispatch_block_t work)
{
	// test before the copy of the block
	if (when == DISPATCH_TIME_FOREVER) {
#if DISPATCH_DEBUG
		DISPATCH_CLIENT_CRASH(
				"dispatch_after() called with 'when' == infinity");
#endif
		return;
	}
	dispatch_after_f(when, queue, _dispatch_Block_copy(work),
			_dispatch_call_block_and_release);
}
#endif

#pragma mark -
#pragma mark dispatch_wakeup

DISPATCH_NOINLINE
static void
_dispatch_queue_push_list_slow2(dispatch_queue_t dq,
		struct dispatch_object_s *obj)
{
	// The queue must be retained before dq_items_head is written in order
	// to ensure that the reference is still valid when _dispatch_wakeup is
	// called. Otherwise, if preempted between the assignment to
	// dq_items_head and _dispatch_wakeup, the blocks submitted to the
	// queue may release the last reference to the queue when invoked by
	// _dispatch_queue_drain. <rdar://problem/6932776>
	_dispatch_retain(dq);
	dq->dq_items_head = obj;
	_dispatch_wakeup(dq);
	_dispatch_release(dq);
}

DISPATCH_NOINLINE
void
_dispatch_queue_push_list_slow(dispatch_queue_t dq,
		struct dispatch_object_s *obj, unsigned int n)
{
	if (dx_type(dq) == DISPATCH_QUEUE_GLOBAL_TYPE) {
		dq->dq_items_head = obj;
		return _dispatch_queue_wakeup_global2(dq, n);
	}
	_dispatch_queue_push_list_slow2(dq, obj);
}

DISPATCH_NOINLINE
void
_dispatch_queue_push_slow(dispatch_queue_t dq,
		struct dispatch_object_s *obj)
{
	if (dx_type(dq) == DISPATCH_QUEUE_GLOBAL_TYPE) {
		dq->dq_items_head = obj;
		return _dispatch_queue_wakeup_global(dq);
	}
	_dispatch_queue_push_list_slow2(dq, obj);
}

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
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

	// _dispatch_source_invoke() relies on this testing the whole suspend count
	// word, not just the lock bit. In other words, no point taking the lock
	// if the source is suspended or canceled.
	if (!dispatch_atomic_cmpxchg2o(dou._do, do_suspend_cnt, 0,
			DISPATCH_OBJECT_SUSPEND_LOCK)) {
#if DISPATCH_COCOA_COMPAT
		if (dou._dq == &_dispatch_main_q) {
			return _dispatch_queue_wakeup_main();
		}
#endif
		return NULL;
	}
	dispatch_atomic_acquire_barrier();
	_dispatch_retain(dou._do);
	tq = dou._do->do_targetq;
	_dispatch_queue_push(tq, dou._do);
	return tq;	// libdispatch does not need this, but the Instrument DTrace
				// probe does
}

#if DISPATCH_COCOA_COMPAT
DISPATCH_NOINLINE
dispatch_queue_t
_dispatch_queue_wakeup_main(void)
{
	kern_return_t kr;

	dispatch_once_f(&_dispatch_main_q_port_pred, NULL,
			_dispatch_main_q_port_init);
	if (!main_q_port) {
		return NULL;
	}
	kr = _dispatch_send_wakeup_main_thread(main_q_port, 0);

	switch (kr) {
	case MACH_SEND_TIMEOUT:
	case MACH_SEND_TIMED_OUT:
	case MACH_SEND_INVALID_DEST:
		break;
	default:
		(void)dispatch_assume_zero(kr);
		break;
	}
	return NULL;
}
#endif

DISPATCH_NOINLINE
static void
_dispatch_queue_wakeup_global_slow(dispatch_queue_t dq, unsigned int n)
{
	static dispatch_once_t pred;
	struct dispatch_root_queue_context_s *qc = dq->do_ctxt;
	int r;

	dispatch_debug_queue(dq, __func__);
	dispatch_once_f(&pred, NULL, _dispatch_root_queues_init);

#if HAVE_PTHREAD_WORKQUEUES
#if DISPATCH_ENABLE_THREAD_POOL
	if (qc->dgq_kworkqueue != (void*)(~0ul))
#endif
	{
		_dispatch_debug("requesting new worker thread");
#if DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
		if (qc->dgq_kworkqueue) {
			pthread_workitem_handle_t wh;
			unsigned int gen_cnt, i = n;
			do {
				r = pthread_workqueue_additem_np(qc->dgq_kworkqueue,
						_dispatch_worker_thread3, dq, &wh, &gen_cnt);
				(void)dispatch_assume_zero(r);
			} while (--i);
			return;
		}
#endif // DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
#if HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
		r = pthread_workqueue_addthreads_np(qc->dgq_wq_priority,
				qc->dgq_wq_options, n);
		(void)dispatch_assume_zero(r);
#endif
		return;
	}
#endif // HAVE_PTHREAD_WORKQUEUES
#if DISPATCH_ENABLE_THREAD_POOL
	if (dispatch_semaphore_signal(qc->dgq_thread_mediator)) {
		return;
	}

	pthread_t pthr;
	int t_count;
	do {
		t_count = qc->dgq_thread_pool_size;
		if (!t_count) {
			_dispatch_debug("The thread pool is full: %p", dq);
			return;
		}
	} while (!dispatch_atomic_cmpxchg2o(qc, dgq_thread_pool_size, t_count,
			t_count - 1));

	while ((r = pthread_create(&pthr, NULL, _dispatch_worker_thread, dq))) {
		if (r != EAGAIN) {
			(void)dispatch_assume_zero(r);
		}
		sleep(1);
	}
	r = pthread_detach(pthr);
	(void)dispatch_assume_zero(r);
#endif // DISPATCH_ENABLE_THREAD_POOL
}

static inline void
_dispatch_queue_wakeup_global2(dispatch_queue_t dq, unsigned int n)
{
	struct dispatch_root_queue_context_s *qc = dq->do_ctxt;

	if (!dq->dq_items_tail) {
		return;
	}
#if HAVE_PTHREAD_WORKQUEUES
	if (
#if DISPATCH_ENABLE_THREAD_POOL
			(qc->dgq_kworkqueue != (void*)(~0ul)) &&
#endif
			!dispatch_atomic_cmpxchg2o(qc, dgq_pending, 0, n)) {
		_dispatch_debug("work thread request still pending on global queue: "
				"%p", dq);
		return;
	}
#endif // HAVE_PTHREAD_WORKQUEUES
	return 	_dispatch_queue_wakeup_global_slow(dq, n);
}

static inline void
_dispatch_queue_wakeup_global(dispatch_queue_t dq)
{
	return _dispatch_queue_wakeup_global2(dq, 1);
}

bool
_dispatch_queue_probe_root(dispatch_queue_t dq)
{
	_dispatch_queue_wakeup_global2(dq, 1);
	return false;
}

#pragma mark -
#pragma mark dispatch_queue_drain

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
DISPATCH_NOINLINE
void
_dispatch_queue_invoke(dispatch_queue_t dq)
{
	if (!slowpath(DISPATCH_OBJECT_SUSPENDED(dq)) &&
			fastpath(dispatch_atomic_cmpxchg2o(dq, dq_running, 0, 1))) {
		dispatch_atomic_acquire_barrier();
		dispatch_queue_t otq = dq->do_targetq, tq = NULL;
		_dispatch_thread_semaphore_t sema = _dispatch_queue_drain(dq);
		if (dq->do_vtable->do_invoke) {
			// Assume that object invoke checks it is executing on correct queue
			tq = dx_invoke(dq);
		} else if (slowpath(otq != dq->do_targetq)) {
			// An item on the queue changed the target queue
			tq = dq->do_targetq;
		}
		// We do not need to check the result.
		// When the suspend-count lock is dropped, then the check will happen.
		dispatch_atomic_release_barrier();
		(void)dispatch_atomic_dec2o(dq, dq_running);
		if (sema) {
			_dispatch_thread_semaphore_signal(sema);
		} else if (tq) {
			return _dispatch_queue_push(tq, dq);
		}
	}

	dq->do_next = DISPATCH_OBJECT_LISTLESS;
	dispatch_atomic_release_barrier();
	if (!dispatch_atomic_sub2o(dq, do_suspend_cnt,
			DISPATCH_OBJECT_SUSPEND_LOCK)) {
		if (dq->dq_running == 0) {
			_dispatch_wakeup(dq); // verify that the queue is idle
		}
	}
	_dispatch_release(dq); // added when the queue is put on the list
}

static _dispatch_thread_semaphore_t
_dispatch_queue_drain(dispatch_queue_t dq)
{
	dispatch_queue_t orig_tq, old_dq;
	old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	struct dispatch_object_s *dc = NULL, *next_dc = NULL;
	_dispatch_thread_semaphore_t sema = 0;

	// Continue draining sources after target queue change rdar://8928171
	bool check_tq = (dx_type(dq) != DISPATCH_SOURCE_KEVENT_TYPE);

	orig_tq = dq->do_targetq;

	_dispatch_thread_setspecific(dispatch_queue_key, dq);
	//dispatch_debug_queue(dq, __func__);

	while (dq->dq_items_tail) {
		while (!(dc = fastpath(dq->dq_items_head))) {
			_dispatch_hardware_pause();
		}
		dq->dq_items_head = NULL;
		do {
			next_dc = fastpath(dc->do_next);
			if (!next_dc &&
					!dispatch_atomic_cmpxchg2o(dq, dq_items_tail, dc, NULL)) {
				// Enqueue is TIGHTLY controlled, we won't wait long.
				while (!(next_dc = fastpath(dc->do_next))) {
					_dispatch_hardware_pause();
				}
			}
			if (DISPATCH_OBJECT_SUSPENDED(dq)) {
				goto out;
			}
			if (dq->dq_running > dq->dq_width) {
				goto out;
			}
			if (slowpath(orig_tq != dq->do_targetq) && check_tq) {
				goto out;
			}
			if (!fastpath(dq->dq_width == 1)) {
				if (!DISPATCH_OBJ_IS_VTABLE(dc) &&
						(long)dc->do_vtable & DISPATCH_OBJ_BARRIER_BIT) {
					if (dq->dq_running > 1) {
						goto out;
					}
				} else {
					_dispatch_continuation_redirect(dq, dc);
					continue;
				}
			}
			if ((sema = _dispatch_barrier_sync_f_pop(dq, dc, true))) {
				dc = next_dc;
				goto out;
			}
			_dispatch_continuation_pop(dc);
			_dispatch_workitem_inc();
		} while ((dc = next_dc));
	}

out:
	// if this is not a complete drain, we must undo some things
	if (slowpath(dc)) {
		// 'dc' must NOT be "popped"
		// 'dc' might be the last item
		if (!next_dc &&
				!dispatch_atomic_cmpxchg2o(dq, dq_items_tail, NULL, dc)) {
			// wait for enqueue slow path to finish
			while (!(next_dc = fastpath(dq->dq_items_head))) {
				_dispatch_hardware_pause();
			}
			dc->do_next = next_dc;
		}
		dq->dq_items_head = dc;
	}

	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);
	return sema;
}

static void
_dispatch_queue_serial_drain_till_empty(dispatch_queue_t dq)
{
#if DISPATCH_PERF_MON
	uint64_t start = _dispatch_absolute_time();
#endif
	_dispatch_thread_semaphore_t sema = _dispatch_queue_drain(dq);
	if (sema) {
		dispatch_atomic_barrier();
		_dispatch_thread_semaphore_signal(sema);
	}
#if DISPATCH_PERF_MON
	_dispatch_queue_merge_stats(start);
#endif
	_dispatch_force_cache_cleanup();
}

#if DISPATCH_COCOA_COMPAT
void
_dispatch_main_queue_drain(void)
{
	dispatch_queue_t dq = &_dispatch_main_q;
	if (!dq->dq_items_tail) {
		return;
	}
	struct dispatch_main_queue_drain_marker_s {
		DISPATCH_CONTINUATION_HEADER(main_queue_drain_marker);
	} marker = {
		.do_vtable = NULL,
	};
	struct dispatch_object_s *dmarker = (void*)&marker;
	_dispatch_queue_push_notrace(dq, dmarker);

#if DISPATCH_PERF_MON
	uint64_t start = _dispatch_absolute_time();
#endif
	dispatch_queue_t old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	_dispatch_thread_setspecific(dispatch_queue_key, dq);

	struct dispatch_object_s *dc = NULL, *next_dc = NULL;
	while (dq->dq_items_tail) {
		while (!(dc = fastpath(dq->dq_items_head))) {
			_dispatch_hardware_pause();
		}
		dq->dq_items_head = NULL;
		do {
			next_dc = fastpath(dc->do_next);
			if (!next_dc &&
					!dispatch_atomic_cmpxchg2o(dq, dq_items_tail, dc, NULL)) {
				// Enqueue is TIGHTLY controlled, we won't wait long.
				while (!(next_dc = fastpath(dc->do_next))) {
					_dispatch_hardware_pause();
				}
			}
			if (dc == dmarker) {
				if (next_dc) {
					dq->dq_items_head = next_dc;
					_dispatch_queue_wakeup_main();
				}
				goto out;
			}
			_dispatch_continuation_pop(dc);
			_dispatch_workitem_inc();
		} while ((dc = next_dc));
	}
	dispatch_assert(dc); // did not encounter marker

out:
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);
#if DISPATCH_PERF_MON
	_dispatch_queue_merge_stats(start);
#endif
	_dispatch_force_cache_cleanup();
}
#endif

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline _dispatch_thread_semaphore_t
_dispatch_queue_drain_one_barrier_sync(dispatch_queue_t dq)
{
	// rdar://problem/8290662 "lock transfer"
	struct dispatch_object_s *dc, *next_dc;
	_dispatch_thread_semaphore_t sema;

	// queue is locked, or suspended and not being drained
	dc = dq->dq_items_head;
	if (slowpath(!dc) || !(sema = _dispatch_barrier_sync_f_pop(dq, dc, false))){
		return 0;
	}
	// dequeue dc, it is a barrier sync
	next_dc = fastpath(dc->do_next);
	dq->dq_items_head = next_dc;
	if (!next_dc && !dispatch_atomic_cmpxchg2o(dq, dq_items_tail, dc, NULL)) {
		// Enqueue is TIGHTLY controlled, we won't wait long.
		while (!(next_dc = fastpath(dc->do_next))) {
			_dispatch_hardware_pause();
		}
		dq->dq_items_head = next_dc;
	}
	return sema;
}

#ifndef DISPATCH_HEAD_CONTENTION_SPINS
#define DISPATCH_HEAD_CONTENTION_SPINS 10000
#endif

static struct dispatch_object_s *
_dispatch_queue_concurrent_drain_one(dispatch_queue_t dq)
{
	struct dispatch_object_s *head, *next, *const mediator = (void *)~0ul;

start:
	// The mediator value acts both as a "lock" and a signal
	head = dispatch_atomic_xchg2o(dq, dq_items_head, mediator);

	if (slowpath(head == NULL)) {
		// The first xchg on the tail will tell the enqueueing thread that it
		// is safe to blindly write out to the head pointer. A cmpxchg honors
		// the algorithm.
		(void)dispatch_atomic_cmpxchg2o(dq, dq_items_head, mediator, NULL);
		_dispatch_debug("no work on global work queue");
		return NULL;
	}

	if (slowpath(head == mediator)) {
		// This thread lost the race for ownership of the queue.
		// Spin for a short while in case many threads have started draining at
		// once as part of a dispatch_apply
		unsigned int i = DISPATCH_HEAD_CONTENTION_SPINS;
		do {
			_dispatch_hardware_pause();
			if (dq->dq_items_head != mediator) goto start;
		} while (--i);
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

		if (dispatch_atomic_cmpxchg2o(dq, dq_items_tail, head, NULL)) {
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

#pragma mark -
#pragma mark dispatch_worker_thread

static void
_dispatch_worker_thread4(dispatch_queue_t dq)
{
	struct dispatch_object_s *item;


#if DISPATCH_DEBUG
	if (_dispatch_thread_getspecific(dispatch_queue_key)) {
		DISPATCH_CRASH("Premature thread recycling");
	}
#endif
	_dispatch_thread_setspecific(dispatch_queue_key, dq);

#if DISPATCH_COCOA_COMPAT
	(void)dispatch_atomic_inc(&_dispatch_worker_threads);
	// ensure that high-level memory management techniques do not leak/crash
	if (dispatch_begin_thread_4GC) {
		dispatch_begin_thread_4GC();
	}
	void *pool = _dispatch_autorelease_pool_push();
#endif // DISPATCH_COCOA_COMPAT

#if DISPATCH_PERF_MON
	uint64_t start = _dispatch_absolute_time();
#endif
	while ((item = fastpath(_dispatch_queue_concurrent_drain_one(dq)))) {
		_dispatch_continuation_pop(item);
	}
#if DISPATCH_PERF_MON
	_dispatch_queue_merge_stats(start);
#endif

#if DISPATCH_COCOA_COMPAT
	_dispatch_autorelease_pool_pop(pool);
	if (dispatch_end_thread_4GC) {
		dispatch_end_thread_4GC();
	}
	if (!dispatch_atomic_dec(&_dispatch_worker_threads) &&
			dispatch_no_worker_threads_4GC) {
		dispatch_no_worker_threads_4GC();
	}
#endif // DISPATCH_COCOA_COMPAT

	_dispatch_thread_setspecific(dispatch_queue_key, NULL);

	_dispatch_force_cache_cleanup();

}

#if DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
static void
_dispatch_worker_thread3(void *context)
{
	dispatch_queue_t dq = context;
	struct dispatch_root_queue_context_s *qc = dq->do_ctxt;

	(void)dispatch_atomic_dec2o(qc, dgq_pending);
	_dispatch_worker_thread4(dq);
}
#endif

#if HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
static void
_dispatch_worker_thread2(int priority, int options,
		void *context DISPATCH_UNUSED)
{
	dispatch_assert(priority >= 0 && priority < WORKQ_NUM_PRIOQUEUE);
	dispatch_assert(!(options & ~WORKQ_ADDTHREADS_OPTION_OVERCOMMIT));
	dispatch_queue_t dq = _dispatch_wq2root_queues[priority][options];
	struct dispatch_root_queue_context_s *qc = dq->do_ctxt;

	(void)dispatch_atomic_dec2o(qc, dgq_pending);
	_dispatch_worker_thread4(dq);
}
#endif

#if DISPATCH_ENABLE_THREAD_POOL
// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
static void *
_dispatch_worker_thread(void *context)
{
	dispatch_queue_t dq = context;
	struct dispatch_root_queue_context_s *qc = dq->do_ctxt;
	sigset_t mask;
	int r;

	// workaround tweaks the kernel workqueue does for us
	r = sigfillset(&mask);
	(void)dispatch_assume_zero(r);
	r = _dispatch_pthread_sigmask(SIG_BLOCK, &mask, NULL);
	(void)dispatch_assume_zero(r);

	do {
		_dispatch_worker_thread4(dq);
		// we use 65 seconds in case there are any timers that run once a minute
	} while (dispatch_semaphore_wait(qc->dgq_thread_mediator,
			dispatch_time(0, 65ull * NSEC_PER_SEC)) == 0);

	(void)dispatch_atomic_inc2o(qc, dgq_thread_pool_size);
	if (dq->dq_items_tail) {
		_dispatch_queue_wakeup_global(dq);
	}

	return NULL;
}

int
_dispatch_pthread_sigmask(int how, sigset_t *set, sigset_t *oset)
{
	int r;

	/* Workaround: 6269619 Not all signals can be delivered on any thread */

	r = sigdelset(set, SIGILL);
	(void)dispatch_assume_zero(r);
	r = sigdelset(set, SIGTRAP);
	(void)dispatch_assume_zero(r);
#if HAVE_DECL_SIGEMT
	r = sigdelset(set, SIGEMT);
	(void)dispatch_assume_zero(r);
#endif
	r = sigdelset(set, SIGFPE);
	(void)dispatch_assume_zero(r);
	r = sigdelset(set, SIGBUS);
	(void)dispatch_assume_zero(r);
	r = sigdelset(set, SIGSEGV);
	(void)dispatch_assume_zero(r);
	r = sigdelset(set, SIGSYS);
	(void)dispatch_assume_zero(r);
	r = sigdelset(set, SIGPIPE);
	(void)dispatch_assume_zero(r);

	return pthread_sigmask(how, set, oset);
}
#endif

#pragma mark -
#pragma mark dispatch_main_queue

static bool _dispatch_program_is_probably_callback_driven;

#if DISPATCH_COCOA_COMPAT
static void
_dispatch_main_q_port_init(void *ctxt DISPATCH_UNUSED)
{
	kern_return_t kr;

	_dispatch_safe_fork = false;
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			&main_q_port);
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
	kr = mach_port_insert_right(mach_task_self(), main_q_port, main_q_port,
			MACH_MSG_TYPE_MAKE_SEND);
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);

	_dispatch_program_is_probably_callback_driven = true;
}

mach_port_t
_dispatch_get_main_queue_port_4CF(void)
{
	dispatch_once_f(&_dispatch_main_q_port_pred, NULL,
			_dispatch_main_q_port_init);
	return main_q_port;
}

static bool main_q_is_draining;

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
DISPATCH_NOINLINE
static void
_dispatch_queue_set_mainq_drain_state(bool arg)
{
	main_q_is_draining = arg;
}

void
_dispatch_main_queue_callback_4CF(mach_msg_header_t *msg DISPATCH_UNUSED)
{
	if (main_q_is_draining) {
		return;
	}
	_dispatch_queue_set_mainq_drain_state(true);
	_dispatch_main_queue_drain();
	_dispatch_queue_set_mainq_drain_state(false);
}

#endif

void
dispatch_main(void)
{
#if HAVE_PTHREAD_MAIN_NP
	if (pthread_main_np()) {
#endif
		_dispatch_program_is_probably_callback_driven = true;
		pthread_exit(NULL);
		DISPATCH_CRASH("pthread_exit() returned");
#if HAVE_PTHREAD_MAIN_NP
	}
	DISPATCH_CLIENT_CRASH("dispatch_main() must be called on the main thread");
#endif
}

DISPATCH_NOINLINE DISPATCH_NORETURN
static void
_dispatch_sigsuspend(void)
{
	static const sigset_t mask;

#if DISPATCH_COCOA_COMPAT
	// Do not count the signal handling thread as a worker thread
	(void)dispatch_atomic_dec(&_dispatch_worker_threads);
#endif
	for (;;) {
		sigsuspend(&mask);
	}
}

DISPATCH_NORETURN
static void
_dispatch_sig_thread(void *ctxt DISPATCH_UNUSED)
{
	// never returns, so burn bridges behind us
	_dispatch_clear_stack(0);
	_dispatch_sigsuspend();
}

DISPATCH_NOINLINE
static void
_dispatch_queue_cleanup2(void)
{
	(void)dispatch_atomic_dec(&_dispatch_main_q.dq_running);

	dispatch_atomic_release_barrier();
	if (dispatch_atomic_sub2o(&_dispatch_main_q, do_suspend_cnt,
			DISPATCH_OBJECT_SUSPEND_LOCK) == 0) {
		_dispatch_wakeup(&_dispatch_main_q);
	}

	// overload the "probably" variable to mean that dispatch_main() or
	// similar non-POSIX API was called
	// this has to run before the DISPATCH_COCOA_COMPAT below
	if (_dispatch_program_is_probably_callback_driven) {
		dispatch_async_f(_dispatch_get_root_queue(0, true), NULL,
				_dispatch_sig_thread);
		sleep(1); // workaround 6778970
	}

#if DISPATCH_COCOA_COMPAT
	dispatch_once_f(&_dispatch_main_q_port_pred, NULL,
			_dispatch_main_q_port_init);

	mach_port_t mp = main_q_port;
	kern_return_t kr;

	main_q_port = 0;

	if (mp) {
		kr = mach_port_deallocate(mach_task_self(), mp);
		DISPATCH_VERIFY_MIG(kr);
		(void)dispatch_assume_zero(kr);
		kr = mach_port_mod_refs(mach_task_self(), mp, MACH_PORT_RIGHT_RECEIVE,
				-1);
		DISPATCH_VERIFY_MIG(kr);
		(void)dispatch_assume_zero(kr);
	}
#endif
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

#pragma mark -
#pragma mark dispatch_manager_queue

static unsigned int _dispatch_select_workaround;
static fd_set _dispatch_rfds;
static fd_set _dispatch_wfds;
static void **_dispatch_rfd_ptrs;
static void **_dispatch_wfd_ptrs;

static int _dispatch_kq;

static void
_dispatch_get_kq_init(void *context DISPATCH_UNUSED)
{
	static const struct kevent kev = {
		.ident = 1,
		.filter = EVFILT_USER,
		.flags = EV_ADD|EV_CLEAR,
	};

	_dispatch_safe_fork = false;
	_dispatch_kq = kqueue();
	if (_dispatch_kq == -1) {
		DISPATCH_CLIENT_CRASH("kqueue() create failed: "
				"probably out of file descriptors");
	} else if (dispatch_assume(_dispatch_kq < FD_SETSIZE)) {
	// in case we fall back to select()
		FD_SET(_dispatch_kq, &_dispatch_rfds);
	}

	(void)dispatch_assume_zero(kevent(_dispatch_kq, &kev, 1, NULL, 0, NULL));

	_dispatch_queue_push(_dispatch_mgr_q.do_targetq, &_dispatch_mgr_q);
}

static int
_dispatch_get_kq(void)
{
	static dispatch_once_t pred;

	dispatch_once_f(&pred, NULL, _dispatch_get_kq_init);

	return _dispatch_kq;
}

long
_dispatch_update_kq(const struct kevent *kev)
{
	int rval;
	struct kevent kev_copy = *kev;
	// This ensures we don't get a pending kevent back while registering
	// a new kevent
	kev_copy.flags |= EV_RECEIPT;

	if (_dispatch_select_workaround && (kev_copy.flags & EV_DELETE)) {
		// Only executed on manager queue
		switch (kev_copy.filter) {
		case EVFILT_READ:
			if (kev_copy.ident < FD_SETSIZE &&
					FD_ISSET((int)kev_copy.ident, &_dispatch_rfds)) {
				FD_CLR((int)kev_copy.ident, &_dispatch_rfds);
				_dispatch_rfd_ptrs[kev_copy.ident] = 0;
				(void)dispatch_atomic_dec(&_dispatch_select_workaround);
				return 0;
			}
			break;
		case EVFILT_WRITE:
			if (kev_copy.ident < FD_SETSIZE &&
					FD_ISSET((int)kev_copy.ident, &_dispatch_wfds)) {
				FD_CLR((int)kev_copy.ident, &_dispatch_wfds);
				_dispatch_wfd_ptrs[kev_copy.ident] = 0;
				(void)dispatch_atomic_dec(&_dispatch_select_workaround);
				return 0;
			}
			break;
		default:
			break;
		}
	}

retry:
	rval = kevent(_dispatch_get_kq(), &kev_copy, 1, &kev_copy, 1, NULL);
	if (rval == -1) {
		// If we fail to register with kevents, for other reasons aside from
		// changelist elements.
		int err = errno;
		switch (err) {
		case EINTR:
			goto retry;
		case EBADF:
			_dispatch_bug_client("Do not close random Unix descriptors");
			break;
		default:
			(void)dispatch_assume_zero(err);
			break;
		}
		//kev_copy.flags |= EV_ERROR;
		//kev_copy.data = err;
		return err;
	}

	// The following select workaround only applies to adding kevents
	if ((kev->flags & (EV_DISABLE|EV_DELETE)) ||
			!(kev->flags & (EV_ADD|EV_ENABLE))) {
		return 0;
	}

	// Only executed on manager queue
	switch (kev_copy.data) {
	case 0:
		return 0;
	case EBADF:
		break;
	default:
		// If an error occurred while registering with kevent, and it was
		// because of a kevent changelist processing && the kevent involved
		// either doing a read or write, it would indicate we were trying
		// to register a /dev/* port; fall back to select
		switch (kev_copy.filter) {
		case EVFILT_READ:
			if (dispatch_assume(kev_copy.ident < FD_SETSIZE)) {
				if (!_dispatch_rfd_ptrs) {
					_dispatch_rfd_ptrs = calloc(FD_SETSIZE, sizeof(void*));
				}
				_dispatch_rfd_ptrs[kev_copy.ident] = kev_copy.udata;
				FD_SET((int)kev_copy.ident, &_dispatch_rfds);
				(void)dispatch_atomic_inc(&_dispatch_select_workaround);
				_dispatch_debug("select workaround used to read fd %d: 0x%lx",
						(int)kev_copy.ident, (long)kev_copy.data);
				return 0;
			}
			break;
		case EVFILT_WRITE:
			if (dispatch_assume(kev_copy.ident < FD_SETSIZE)) {
				if (!_dispatch_wfd_ptrs) {
					_dispatch_wfd_ptrs = calloc(FD_SETSIZE, sizeof(void*));
				}
				_dispatch_wfd_ptrs[kev_copy.ident] = kev_copy.udata;
				FD_SET((int)kev_copy.ident, &_dispatch_wfds);
				(void)dispatch_atomic_inc(&_dispatch_select_workaround);
				_dispatch_debug("select workaround used to write fd %d: 0x%lx",
						(int)kev_copy.ident, (long)kev_copy.data);
				return 0;
			}
			break;
		default:
			// kevent error, _dispatch_source_merge_kevent() will handle it
			_dispatch_source_drain_kevent(&kev_copy);
			break;
		}
		break;
	}
	return kev_copy.data;
}

bool
_dispatch_mgr_wakeup(dispatch_queue_t dq)
{
	static const struct kevent kev = {
		.ident = 1,
		.filter = EVFILT_USER,
		.fflags = NOTE_TRIGGER,
	};

	_dispatch_debug("waking up the _dispatch_mgr_q: %p", dq);

	_dispatch_update_kq(&kev);

	return false;
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

#if DISPATCH_USE_VM_PRESSURE && DISPATCH_USE_MALLOC_VM_PRESSURE_SOURCE
// VM Pressure source for malloc <rdar://problem/7805121>
static dispatch_source_t _dispatch_malloc_vm_pressure_source;

static void
_dispatch_malloc_vm_pressure_handler(void *context DISPATCH_UNUSED)
{
	malloc_zone_pressure_relief(0,0);
}

static void
_dispatch_malloc_vm_pressure_setup(void)
{
	_dispatch_malloc_vm_pressure_source = dispatch_source_create(
			DISPATCH_SOURCE_TYPE_VM, 0, DISPATCH_VM_PRESSURE,
			_dispatch_get_root_queue(0, true));
	dispatch_source_set_event_handler_f(_dispatch_malloc_vm_pressure_source,
			_dispatch_malloc_vm_pressure_handler);
	dispatch_resume(_dispatch_malloc_vm_pressure_source);
}
#else
#define _dispatch_malloc_vm_pressure_setup()
#endif

DISPATCH_NOINLINE DISPATCH_NORETURN
static void
_dispatch_mgr_invoke(void)
{
	static const struct timespec timeout_immediately = { 0, 0 };
	struct timespec timeout;
	const struct timespec *timeoutp;
	struct timeval sel_timeout, *sel_timeoutp;
	fd_set tmp_rfds, tmp_wfds;
	struct kevent kev[1];
	int k_cnt, err, i, r;

	_dispatch_thread_setspecific(dispatch_queue_key, &_dispatch_mgr_q);
#if DISPATCH_COCOA_COMPAT
	// Do not count the manager thread as a worker thread
	(void)dispatch_atomic_dec(&_dispatch_worker_threads);
#endif
	_dispatch_malloc_vm_pressure_setup();

	for (;;) {
		_dispatch_run_timers();

		timeoutp = _dispatch_get_next_timer_fire(&timeout);

		if (_dispatch_select_workaround) {
			FD_COPY(&_dispatch_rfds, &tmp_rfds);
			FD_COPY(&_dispatch_wfds, &tmp_wfds);
			if (timeoutp) {
				sel_timeout.tv_sec = timeoutp->tv_sec;
				sel_timeout.tv_usec = (typeof(sel_timeout.tv_usec))
						(timeoutp->tv_nsec / 1000u);
				sel_timeoutp = &sel_timeout;
			} else {
				sel_timeoutp = NULL;
			}

			r = select(FD_SETSIZE, &tmp_rfds, &tmp_wfds, NULL, sel_timeoutp);
			if (r == -1) {
				err = errno;
				if (err != EBADF) {
					if (err != EINTR) {
						(void)dispatch_assume_zero(err);
					}
					continue;
				}
				for (i = 0; i < FD_SETSIZE; i++) {
					if (i == _dispatch_kq) {
						continue;
					}
					if (!FD_ISSET(i, &_dispatch_rfds) && !FD_ISSET(i,
							&_dispatch_wfds)) {
						continue;
					}
					r = dup(i);
					if (r != -1) {
						close(r);
					} else {
						if (FD_ISSET(i, &_dispatch_rfds)) {
							FD_CLR(i, &_dispatch_rfds);
							_dispatch_rfd_ptrs[i] = 0;
							(void)dispatch_atomic_dec(
									&_dispatch_select_workaround);
						}
						if (FD_ISSET(i, &_dispatch_wfds)) {
							FD_CLR(i, &_dispatch_wfds);
							_dispatch_wfd_ptrs[i] = 0;
							(void)dispatch_atomic_dec(
									&_dispatch_select_workaround);
						}
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
						FD_CLR(i, &_dispatch_rfds); // emulate EV_DISABLE
						EV_SET(&kev[0], i, EVFILT_READ,
								EV_ADD|EV_ENABLE|EV_DISPATCH, 0, 1,
								_dispatch_rfd_ptrs[i]);
						_dispatch_rfd_ptrs[i] = 0;
						(void)dispatch_atomic_dec(&_dispatch_select_workaround);
						_dispatch_mgr_thread2(kev, 1);
					}
					if (FD_ISSET(i, &tmp_wfds)) {
						FD_CLR(i, &_dispatch_wfds); // emulate EV_DISABLE
						EV_SET(&kev[0], i, EVFILT_WRITE,
								EV_ADD|EV_ENABLE|EV_DISPATCH, 0, 1,
								_dispatch_wfd_ptrs[i]);
						_dispatch_wfd_ptrs[i] = 0;
						(void)dispatch_atomic_dec(&_dispatch_select_workaround);
						_dispatch_mgr_thread2(kev, 1);
					}
				}
			}

			timeoutp = &timeout_immediately;
		}

		k_cnt = kevent(_dispatch_kq, NULL, 0, kev, sizeof(kev) / sizeof(kev[0]),
				timeoutp);
		err = errno;

		switch (k_cnt) {
		case -1:
			if (err == EBADF) {
				DISPATCH_CLIENT_CRASH("Do not close random Unix descriptors");
			}
			if (err != EINTR) {
				(void)dispatch_assume_zero(err);
			}
			continue;
		default:
			_dispatch_mgr_thread2(kev, (size_t)k_cnt);
			// fall through
		case 0:
			_dispatch_force_cache_cleanup();
			continue;
		}
	}
}

DISPATCH_NORETURN
dispatch_queue_t
_dispatch_mgr_thread(dispatch_queue_t dq DISPATCH_UNUSED)
{
	// never returns, so burn bridges behind us & clear stack 2k ahead
	_dispatch_clear_stack(2048);
	_dispatch_mgr_invoke();
}
