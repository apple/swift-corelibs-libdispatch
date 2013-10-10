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

#include "internal.h"
#if HAVE_MACH
#include "protocol.h"
#endif

#if (!HAVE_PTHREAD_WORKQUEUES || DISPATCH_DEBUG) && \
		!defined(DISPATCH_ENABLE_THREAD_POOL)
#define DISPATCH_ENABLE_THREAD_POOL 1
#endif
#if DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES || DISPATCH_ENABLE_THREAD_POOL
#define DISPATCH_USE_PTHREAD_POOL 1
#endif
#if HAVE_PTHREAD_WORKQUEUES && !HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP && \
		!defined(DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK)
#define DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK 1
#endif
#if HAVE_PTHREAD_WORKQUEUES && DISPATCH_USE_PTHREAD_POOL && \
		!DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
#define pthread_workqueue_t void*
#endif

static void _dispatch_cache_cleanup(void *value);
static void _dispatch_async_f_redirect(dispatch_queue_t dq,
		dispatch_continuation_t dc);
static void _dispatch_queue_cleanup(void *ctxt);
static inline void _dispatch_queue_wakeup_global2(dispatch_queue_t dq,
		unsigned int n);
static inline void _dispatch_queue_wakeup_global(dispatch_queue_t dq);
static inline _dispatch_thread_semaphore_t
		_dispatch_queue_drain_one_barrier_sync(dispatch_queue_t dq);
#if HAVE_PTHREAD_WORKQUEUES
static void _dispatch_worker_thread3(void *context);
#if HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
static void _dispatch_worker_thread2(int priority, int options, void *context);
#endif
#endif
#if DISPATCH_USE_PTHREAD_POOL
static void *_dispatch_worker_thread(void *context);
static int _dispatch_pthread_sigmask(int how, sigset_t *set, sigset_t *oset);
#endif

#if DISPATCH_COCOA_COMPAT
static dispatch_once_t _dispatch_main_q_port_pred;
static dispatch_queue_t _dispatch_main_queue_wakeup(void);
unsigned long _dispatch_runloop_queue_wakeup(dispatch_queue_t dq);
static void _dispatch_runloop_queue_port_init(void *ctxt);
static void _dispatch_runloop_queue_port_dispose(dispatch_queue_t dq);
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

#define MAX_PTHREAD_COUNT 255

struct dispatch_root_queue_context_s {
	union {
		struct {
			unsigned int volatile dgq_pending;
#if HAVE_PTHREAD_WORKQUEUES
			int dgq_wq_priority, dgq_wq_options;
#if DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK || DISPATCH_USE_PTHREAD_POOL
			pthread_workqueue_t dgq_kworkqueue;
#endif
#endif // HAVE_PTHREAD_WORKQUEUES
#if DISPATCH_USE_PTHREAD_POOL
			void *dgq_ctxt;
			dispatch_semaphore_t dgq_thread_mediator;
			uint32_t volatile dgq_thread_pool_size;
#endif
		};
		char _dgq_pad[DISPATCH_CACHELINE_SIZE];
	};
};
typedef struct dispatch_root_queue_context_s *dispatch_root_queue_context_t;

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

#if DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES
static struct dispatch_queue_s _dispatch_mgr_root_queue;
#else
#define _dispatch_mgr_root_queue \
		_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_HIGH_OVERCOMMIT_PRIORITY]
#endif

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
DISPATCH_CACHELINE_ALIGN
struct dispatch_queue_s _dispatch_mgr_q = {
	.do_vtable = DISPATCH_VTABLE(queue_mgr),
	.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
	.do_targetq = &_dispatch_mgr_root_queue,
	.dq_label = "com.apple.libdispatch-manager",
	.dq_width = 1,
	.dq_is_thread_bound = 1,
	.dq_serialnum = 2,
};

dispatch_queue_t
dispatch_get_global_queue(long priority, unsigned long flags)
{
	if (flags & ~(unsigned long)DISPATCH_QUEUE_OVERCOMMIT) {
		return NULL;
	}
	return _dispatch_get_root_queue(priority,
			flags & DISPATCH_QUEUE_OVERCOMMIT);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_t
_dispatch_get_current_queue(void)
{
	return _dispatch_queue_get_current() ?: _dispatch_get_root_queue(0, true);
}

dispatch_queue_t
dispatch_get_current_queue(void)
{
	return _dispatch_get_current_queue();
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_targets_queue(dispatch_queue_t dq1, dispatch_queue_t dq2)
{
	while (dq1) {
		if (dq1 == dq2) {
			return true;
		}
		dq1 = dq1->do_targetq;
	}
	return false;
}

#define DISPATCH_ASSERT_QUEUE_MESSAGE "BUG in client of libdispatch: " \
		"Assertion failed: Block was run on an unexpected queue"

DISPATCH_NOINLINE
static void
_dispatch_assert_queue_fail(dispatch_queue_t dq, bool expected)
{
	char *msg;
	asprintf(&msg, "%s\n%s queue: 0x%p[%s]", DISPATCH_ASSERT_QUEUE_MESSAGE,
			expected ? "Expected" : "Unexpected", dq, dq->dq_label ?
			dq->dq_label : "");
	_dispatch_log("%s", msg);
	_dispatch_set_crash_log_message(msg);
	_dispatch_hardware_crash();
	free(msg);
}

void
dispatch_assert_queue(dispatch_queue_t dq)
{
	if (slowpath(!dq) || slowpath(!(dx_metatype(dq) == _DISPATCH_QUEUE_TYPE))) {
		DISPATCH_CLIENT_CRASH("invalid queue passed to "
				"dispatch_assert_queue()");
	}
	dispatch_queue_t cq = _dispatch_queue_get_current();
	if (fastpath(cq) && fastpath(_dispatch_queue_targets_queue(cq, dq))) {
		return;
	}
	_dispatch_assert_queue_fail(dq, true);
}

void
dispatch_assert_queue_not(dispatch_queue_t dq)
{
	if (slowpath(!dq) || slowpath(!(dx_metatype(dq) == _DISPATCH_QUEUE_TYPE))) {
		DISPATCH_CLIENT_CRASH("invalid queue passed to "
				"dispatch_assert_queue_not()");
	}
	dispatch_queue_t cq = _dispatch_queue_get_current();
	if (slowpath(cq) && slowpath(_dispatch_queue_targets_queue(cq, dq))) {
		_dispatch_assert_queue_fail(dq, false);
	}
}

#if DISPATCH_DEBUG && DISPATCH_ROOT_QUEUE_DEBUG
#define _dispatch_root_queue_debug(...) _dispatch_debug(__VA_ARGS__)
#define _dispatch_debug_root_queue(...) dispatch_debug_queue(__VA_ARGS__)
#else
#define _dispatch_root_queue_debug(...)
#define _dispatch_debug_root_queue(...)
#endif

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
#if DISPATCH_ENABLE_THREAD_POOL && DISPATCH_DEBUG
	disable_wq = slowpath(getenv("LIBDISPATCH_DISABLE_KWQ"));
#endif
	int r;
#if HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
	if (!disable_wq) {
#if PTHREAD_WORKQUEUE_SPI_VERSION >= 20121218
		pthread_workqueue_setdispatchoffset_np(
				offsetof(struct dispatch_queue_s, dq_serialnum));
#endif
		r = pthread_workqueue_setdispatch_np(_dispatch_worker_thread2);
#if !DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
		(void)dispatch_assume_zero(r);
#endif
		result = !r;
	}
#endif // HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
#if DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK || DISPATCH_USE_PTHREAD_POOL
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
			dispatch_root_queue_context_t qc;
			qc = &_dispatch_root_queue_contexts[i];
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
						qc->dgq_wq_options &
						WORKQ_ADDTHREADS_OPTION_OVERCOMMIT);
				(void)dispatch_assume_zero(r);
				r = pthread_workqueue_create_np(&pwq, &pwq_attr);
				(void)dispatch_assume_zero(r);
				result = result || dispatch_assume(pwq);
			}
#endif // DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
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

#if DISPATCH_USE_PTHREAD_POOL
static inline void
_dispatch_root_queue_init_pthread_pool(dispatch_root_queue_context_t qc,
		bool overcommit)
{
	qc->dgq_thread_pool_size = overcommit ? MAX_PTHREAD_COUNT :
			_dispatch_hw_config.cc_max_active;
#if USE_MACH_SEM
	// override the default FIFO behavior for the pool semaphores
	kern_return_t kr = semaphore_create(mach_task_self(),
			&qc->dgq_thread_mediator->dsema_port, SYNC_POLICY_LIFO, 0);
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
	(void)dispatch_assume(qc->dgq_thread_mediator->dsema_port);
#elif USE_POSIX_SEM
	/* XXXRW: POSIX semaphores don't support LIFO? */
	int ret = sem_init(&qc->dgq_thread_mediator->dsema_sem, 0, 0);
	(void)dispatch_assume_zero(ret);
#endif
}
#endif // DISPATCH_USE_PTHREAD_POOL

static void
_dispatch_root_queues_init(void *context DISPATCH_UNUSED)
{
	_dispatch_safe_fork = false;
	if (!_dispatch_root_queues_init_workq()) {
#if DISPATCH_ENABLE_THREAD_POOL
		int i;
		for (i = 0; i < DISPATCH_ROOT_QUEUE_COUNT; i++) {
			bool overcommit = true;
#if TARGET_OS_EMBEDDED
			// some software hangs if the non-overcommitting queues do not
			// overcommit when threads block. Someday, this behavior should
			// apply to all platforms
			if (!(i & 1)) {
				overcommit = false;
			}
#endif
			_dispatch_root_queue_init_pthread_pool(
					&_dispatch_root_queue_contexts[i], overcommit);
		}
#else
		DISPATCH_CRASH("Root queue initialization failed");
#endif // DISPATCH_ENABLE_THREAD_POOL
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
			DISPATCH_CONTINUATION_SIZE);
	dispatch_assert(sizeof(struct dispatch_queue_s) % DISPATCH_CACHELINE_SIZE
			== 0);
	dispatch_assert(sizeof(struct dispatch_root_queue_context_s) %
			DISPATCH_CACHELINE_SIZE == 0);

	_dispatch_thread_key_create(&dispatch_queue_key, _dispatch_queue_cleanup);
#if !DISPATCH_USE_OS_SEMAPHORE_CACHE
	_dispatch_thread_key_create(&dispatch_sema4_key,
			(void (*)(void *))_dispatch_thread_semaphore_dispose);
#endif
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
	_dispatch_queue_set_bound_thread(&_dispatch_main_q);

#if DISPATCH_USE_PTHREAD_ATFORK
	(void)dispatch_assume_zero(pthread_atfork(dispatch_atfork_prepare,
			dispatch_atfork_parent, dispatch_atfork_child));
#endif

	_dispatch_hw_config_init();
	_dispatch_vtable_init();
	_os_object_init();
	_dispatch_introspection_init();
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
	_dispatch_child_of_unsafe_fork = true;

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
// 3 - mgr_root_q
// 4,5,6,7,8,9,10,11 - global queues
// we use 'xadd' on Intel, so the initial value == next assigned
unsigned long volatile _dispatch_queue_serial_numbers = 12;

dispatch_queue_t
dispatch_queue_create_with_target(const char *label,
		dispatch_queue_attr_t attr, dispatch_queue_t tq)
{
	dispatch_queue_t dq;

	dq = _dispatch_alloc(DISPATCH_VTABLE(queue),
			sizeof(struct dispatch_queue_s) - DISPATCH_QUEUE_CACHELINE_PAD);

	_dispatch_queue_init(dq);
	if (label) {
		dq->dq_label = strdup(label);
	}

	if (attr == DISPATCH_QUEUE_CONCURRENT) {
		dq->dq_width = UINT32_MAX;
		if (!tq) {
			tq = _dispatch_get_root_queue(0, false);
		}
	} else {
		if (!tq) {
			// Default target queue is overcommit!
			tq = _dispatch_get_root_queue(0, true);
		}
		if (slowpath(attr)) {
			dispatch_debug_assert(!attr, "Invalid attribute");
		}
	}
	dq->do_targetq = tq;
	_dispatch_object_debug(dq, "%s", __func__);
	return _dispatch_introspection_queue_create(dq);
}

dispatch_queue_t
dispatch_queue_create(const char *label, dispatch_queue_attr_t attr)
{
	return dispatch_queue_create_with_target(label, attr,
			DISPATCH_TARGET_QUEUE_DEFAULT);
}

void
_dispatch_queue_destroy(dispatch_object_t dou)
{
	dispatch_queue_t dq = dou._dq;
	if (slowpath(dq == _dispatch_queue_get_current())) {
		DISPATCH_CRASH("Release of a queue by itself");
	}
	if (slowpath(dq->dq_items_tail)) {
		DISPATCH_CRASH("Release of a queue while items are enqueued");
	}

	// trash the tail queue so that use after free will crash
	dq->dq_items_tail = (void *)0x200;

	dispatch_queue_t dqsq = dispatch_atomic_xchg2o(dq, dq_specific_q,
			(void *)0x200, relaxed);
	if (dqsq) {
		_dispatch_release(dqsq);
	}
}

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
void
_dispatch_queue_dispose(dispatch_queue_t dq)
{
	_dispatch_object_debug(dq, "%s", __func__);
	_dispatch_introspection_queue_dispose(dq);
	if (dq->dq_label) {
		free((void*)dq->dq_label);
	}
	_dispatch_queue_destroy(dq);
}

const char *
dispatch_queue_get_label(dispatch_queue_t dq)
{
	if (slowpath(dq == DISPATCH_CURRENT_QUEUE_LABEL)) {
		dq = _dispatch_get_current_queue();
	}
	return dq->dq_label ? dq->dq_label : "";
}

static void
_dispatch_queue_set_width2(void *ctxt)
{
	int w = (int)(intptr_t)ctxt; // intentional truncation
	uint32_t tmp;
	dispatch_queue_t dq = _dispatch_queue_get_current();

	if (w == 1 || w == 0) {
		dq->dq_width = 1;
		_dispatch_object_debug(dq, "%s", __func__);
		return;
	}
	if (w > 0) {
		tmp = (unsigned int)w;
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
	_dispatch_object_debug(dq, "%s", __func__);
}

void
dispatch_queue_set_width(dispatch_queue_t dq, long width)
{
	if (slowpath(dq->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT) ||
			slowpath(dx_type(dq) == DISPATCH_QUEUE_ROOT_TYPE)) {
		return;
	}
	_dispatch_barrier_trysync_f(dq, (void*)(intptr_t)width,
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
	_dispatch_object_debug(dq, "%s", __func__);
}

void
dispatch_set_target_queue(dispatch_object_t dou, dispatch_queue_t dq)
{
	DISPATCH_OBJECT_TFB(_dispatch_objc_set_target_queue, dou, dq);
	if (slowpath(dou._do->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT) ||
			slowpath(dx_type(dou._do) == DISPATCH_QUEUE_ROOT_TYPE)) {
		return;
	}
	unsigned long type = dx_metatype(dou._do);
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
		return _dispatch_barrier_trysync_f(dou._dq, dq,
				_dispatch_set_target_queue2);
	case _DISPATCH_IO_TYPE:
		return _dispatch_io_set_target_queue(dou._dchannel, dq);
	default: {
		dispatch_queue_t prev_dq;
		_dispatch_retain(dq);
		prev_dq = dispatch_atomic_xchg2o(dou._do, do_targetq, dq, release);
		if (prev_dq) _dispatch_release(prev_dq);
		_dispatch_object_debug(dou._do, "%s", __func__);
		return;
		}
	}
}

#pragma mark -
#pragma mark dispatch_pthread_root_queue

struct dispatch_pthread_root_queue_context_s {
	pthread_attr_t dpq_thread_attr;
	dispatch_block_t dpq_thread_configure;
	struct dispatch_semaphore_s dpq_thread_mediator;
};
typedef struct dispatch_pthread_root_queue_context_s *
		dispatch_pthread_root_queue_context_t;

#if DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES
static struct dispatch_pthread_root_queue_context_s
		_dispatch_mgr_root_queue_pthread_context;
static struct dispatch_root_queue_context_s
		_dispatch_mgr_root_queue_context = {{{
#if HAVE_PTHREAD_WORKQUEUES
	.dgq_kworkqueue = (void*)(~0ul),
#endif
	.dgq_ctxt = &_dispatch_mgr_root_queue_pthread_context,
	.dgq_thread_pool_size = 1,
}}};
static struct dispatch_queue_s _dispatch_mgr_root_queue = {
	.do_vtable = DISPATCH_VTABLE(queue_root),
	.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
	.do_ctxt = &_dispatch_mgr_root_queue_context,
	.dq_label = "com.apple.root.libdispatch-manager",
	.dq_running = 2,
	.dq_width = UINT32_MAX,
	.dq_serialnum = 3,
};
static struct {
	volatile int prio;
	int policy;
	pthread_t tid;
} _dispatch_mgr_sched;
static dispatch_once_t _dispatch_mgr_sched_pred;

static void
_dispatch_mgr_sched_init(void *ctxt DISPATCH_UNUSED)
{
	struct sched_param param;
	pthread_attr_t *attr;
	attr = &_dispatch_mgr_root_queue_pthread_context.dpq_thread_attr;
	(void)dispatch_assume_zero(pthread_attr_init(attr));
	(void)dispatch_assume_zero(pthread_attr_getschedpolicy(attr,
			&_dispatch_mgr_sched.policy));
	(void)dispatch_assume_zero(pthread_attr_getschedparam(attr, &param));
	 // high-priority workq threads are at priority 2 above default
	_dispatch_mgr_sched.prio = param.sched_priority + 2;
}

DISPATCH_NOINLINE
static pthread_t *
_dispatch_mgr_root_queue_init(void)
{
	dispatch_once_f(&_dispatch_mgr_sched_pred, NULL, _dispatch_mgr_sched_init);
	struct sched_param param;
	pthread_attr_t *attr;
	attr = &_dispatch_mgr_root_queue_pthread_context.dpq_thread_attr;
	(void)dispatch_assume_zero(pthread_attr_setdetachstate(attr,
			PTHREAD_CREATE_DETACHED));
#if !DISPATCH_DEBUG
	(void)dispatch_assume_zero(pthread_attr_setstacksize(attr, 64 * 1024));
#endif
	param.sched_priority = _dispatch_mgr_sched.prio;
	(void)dispatch_assume_zero(pthread_attr_setschedparam(attr, &param));
	return &_dispatch_mgr_sched.tid;
}

static inline void
_dispatch_mgr_priority_apply(void)
{
	struct sched_param param;
	do {
		param.sched_priority = _dispatch_mgr_sched.prio;
		(void)dispatch_assume_zero(pthread_setschedparam(
				_dispatch_mgr_sched.tid, _dispatch_mgr_sched.policy, &param));
	} while (_dispatch_mgr_sched.prio > param.sched_priority);
}

DISPATCH_NOINLINE
void
_dispatch_mgr_priority_init(void)
{
	struct sched_param param;
	pthread_attr_t *attr;
	attr = &_dispatch_mgr_root_queue_pthread_context.dpq_thread_attr;
	(void)dispatch_assume_zero(pthread_attr_getschedparam(attr, &param));
	if (slowpath(_dispatch_mgr_sched.prio > param.sched_priority)) {
		return _dispatch_mgr_priority_apply();
	}
}

DISPATCH_NOINLINE
static void
_dispatch_mgr_priority_raise(const pthread_attr_t *attr)
{
	dispatch_once_f(&_dispatch_mgr_sched_pred, NULL, _dispatch_mgr_sched_init);
	struct sched_param param;
	(void)dispatch_assume_zero(pthread_attr_getschedparam(attr, &param));
	int p = _dispatch_mgr_sched.prio;
	do if (p >= param.sched_priority) {
		return;
	} while (slowpath(!dispatch_atomic_cmpxchgvw2o(&_dispatch_mgr_sched, prio,
			p, param.sched_priority, &p, relaxed)));
	if (_dispatch_mgr_sched.tid) {
		return _dispatch_mgr_priority_apply();
	}
}

dispatch_queue_t
dispatch_pthread_root_queue_create(const char *label, unsigned long flags,
		const pthread_attr_t *attr, dispatch_block_t configure)
{
	dispatch_queue_t dq;
	dispatch_root_queue_context_t qc;
	dispatch_pthread_root_queue_context_t pqc;
	size_t dqs;

	if (slowpath(flags)) {
		return NULL;
	}
	dqs = sizeof(struct dispatch_queue_s) - DISPATCH_QUEUE_CACHELINE_PAD;
	dq = _dispatch_alloc(DISPATCH_VTABLE(queue_root), dqs +
			sizeof(struct dispatch_root_queue_context_s) +
			sizeof(struct dispatch_pthread_root_queue_context_s));
	qc = (void*)dq + dqs;
	pqc = (void*)qc + sizeof(struct dispatch_root_queue_context_s);

	_dispatch_queue_init(dq);
	if (label) {
		dq->dq_label = strdup(label);
	}

	dq->do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK;
	dq->do_ctxt = qc;
	dq->do_targetq = NULL;
	dq->dq_running = 2;
	dq->dq_width = UINT32_MAX;

	pqc->dpq_thread_mediator.do_vtable = DISPATCH_VTABLE(semaphore);
	qc->dgq_thread_mediator = &pqc->dpq_thread_mediator;
	qc->dgq_ctxt = pqc;
#if HAVE_PTHREAD_WORKQUEUES
	qc->dgq_kworkqueue = (void*)(~0ul);
#endif
	_dispatch_root_queue_init_pthread_pool(qc, true); // rdar://11352331

	if (attr) {
		memcpy(&pqc->dpq_thread_attr, attr, sizeof(pthread_attr_t));
		_dispatch_mgr_priority_raise(&pqc->dpq_thread_attr);
	} else {
		(void)dispatch_assume_zero(pthread_attr_init(&pqc->dpq_thread_attr));
	}
	(void)dispatch_assume_zero(pthread_attr_setdetachstate(
			&pqc->dpq_thread_attr, PTHREAD_CREATE_DETACHED));
	if (configure) {
		pqc->dpq_thread_configure = _dispatch_Block_copy(configure);
	}
	_dispatch_object_debug(dq, "%s", __func__);
	return _dispatch_introspection_queue_create(dq);
}
#endif

void
_dispatch_pthread_root_queue_dispose(dispatch_queue_t dq)
{
	if (slowpath(dq->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		DISPATCH_CRASH("Global root queue disposed");
	}
	_dispatch_object_debug(dq, "%s", __func__);
	_dispatch_introspection_queue_dispose(dq);
#if DISPATCH_USE_PTHREAD_POOL
	dispatch_root_queue_context_t qc = dq->do_ctxt;
	dispatch_pthread_root_queue_context_t pqc = qc->dgq_ctxt;

	_dispatch_semaphore_dispose(qc->dgq_thread_mediator);
	if (pqc->dpq_thread_configure) {
		Block_release(pqc->dpq_thread_configure);
	}
	dq->do_targetq = _dispatch_get_root_queue(0, false);
#endif
	if (dq->dq_label) {
		free((void*)dq->dq_label);
	}
	_dispatch_queue_destroy(dq);
}

#pragma mark -
#pragma mark dispatch_queue_specific

struct dispatch_queue_specific_queue_s {
	DISPATCH_STRUCT_HEADER(queue_specific_queue);
	DISPATCH_QUEUE_HEADER;
	TAILQ_HEAD(dispatch_queue_specific_head_s,
			dispatch_queue_specific_s) dqsq_contexts;
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
	_dispatch_queue_destroy((dispatch_queue_t)dqsq);
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
	dqsq->dq_label = "queue-specific";
	TAILQ_INIT(&dqsq->dqsq_contexts);
	if (slowpath(!dispatch_atomic_cmpxchg2o(dq, dq_specific_q, NULL,
			(dispatch_queue_t)dqsq, release))) {
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

	dqs = _dispatch_calloc(1, sizeof(struct dispatch_queue_specific_s));
	dqs->dqs_key = key;
	dqs->dqs_ctxt = ctxt;
	dqs->dqs_destructor = destructor;
	if (slowpath(!dq->dq_specific_q)) {
		_dispatch_queue_init_specific(dq);
	}
	_dispatch_barrier_trysync_f(dq->dq_specific_q, dqs,
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
	size_t offset = 0;
	dispatch_queue_t target = dq->do_targetq;
	offset += dsnprintf(buf, bufsiz, "target = %s[%p], width = 0x%x, "
			"running = 0x%x, barrier = %d ", target && target->dq_label ?
			target->dq_label : "", target, dq->dq_width / 2,
			dq->dq_running / 2, dq->dq_running & 1);
	if (dq->dq_is_thread_bound) {
		offset += dsnprintf(buf, bufsiz, ", thread = %p ",
				_dispatch_queue_get_bound_thread(dq));
	}
	return offset;
}

size_t
dispatch_queue_debug(dispatch_queue_t dq, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			dq->dq_label ? dq->dq_label : dx_kind(dq), dq);
	offset += _dispatch_object_debug_attr(dq, &buf[offset], bufsiz - offset);
	offset += _dispatch_queue_debug_attr(dq, &buf[offset], bufsiz - offset);
	offset += dsnprintf(&buf[offset], bufsiz - offset, "}");
	return offset;
}

#if DISPATCH_DEBUG
void
dispatch_debug_queue(dispatch_queue_t dq, const char* str) {
	if (fastpath(dq)) {
		_dispatch_object_debug(dq, "%s", str);
	} else {
		_dispatch_log("queue[NULL]: %s", str);
	}
}
#endif

#if DISPATCH_PERF_MON
static OSSpinLock _dispatch_stats_lock;
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

DISPATCH_NOINLINE
static void
_dispatch_cache_cleanup(void *value)
{
	dispatch_continuation_t dc, next_dc = value;

	while ((dc = next_dc)) {
		next_dc = dc->do_next;
		_dispatch_continuation_free_to_heap(dc);
	}
}

#if DISPATCH_USE_MEMORYSTATUS_SOURCE
int _dispatch_continuation_cache_limit = DISPATCH_CONTINUATION_CACHE_LIMIT;

DISPATCH_NOINLINE
void
_dispatch_continuation_free_to_cache_limit(dispatch_continuation_t dc)
{
	_dispatch_continuation_free_to_heap(dc);
	dispatch_continuation_t next_dc;
	dc = _dispatch_thread_getspecific(dispatch_cache_key);
	int cnt;
	if (!dc || (cnt = dc->do_ref_cnt-_dispatch_continuation_cache_limit) <= 0) {
		return;
	}
	do {
		next_dc = dc->do_next;
		_dispatch_continuation_free_to_heap(dc);
	} while (--cnt && (dc = next_dc));
	_dispatch_thread_setspecific(dispatch_cache_key, next_dc);
}
#endif

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_continuation_redirect(dispatch_queue_t dq, dispatch_object_t dou)
{
	dispatch_continuation_t dc = dou._dc;

	_dispatch_trace_continuation_pop(dq, dou);
	(void)dispatch_atomic_add2o(dq, dq_running, 2, acquire);
	if (!DISPATCH_OBJ_IS_VTABLE(dc) &&
			(long)dc->do_vtable & DISPATCH_OBJ_SYNC_SLOW_BIT) {
		_dispatch_thread_semaphore_signal(
				(_dispatch_thread_semaphore_t)dc->dc_other);
	} else {
		_dispatch_async_f_redirect(dq, dc);
	}
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline void
_dispatch_continuation_pop(dispatch_object_t dou)
{
	dispatch_continuation_t dc = dou._dc, dc1;
	dispatch_group_t dg;

	_dispatch_trace_continuation_pop(_dispatch_queue_get_current(), dou);
	if (DISPATCH_OBJ_IS_VTABLE(dou._do)) {
		return dx_invoke(dou._do);
	}

	// Add the item back to the cache before calling the function. This
	// allows the 'hot' continuation to be used for a quick callback.
	//
	// The ccache version is per-thread.
	// Therefore, the object has not been reused yet.
	// This generates better assembly.
	if ((long)dc->do_vtable & DISPATCH_OBJ_ASYNC_BIT) {
		dc1 = _dispatch_continuation_free_cacheonly(dc);
	} else {
		dc1 = NULL;
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
	if (slowpath(dc1)) {
		_dispatch_continuation_free_to_cache_limit(dc1);
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

void
_dispatch_async_redirect_invoke(void *ctxt)
{
	struct dispatch_continuation_s *dc = ctxt;
	struct dispatch_continuation_s *other_dc = dc->dc_other;
	dispatch_queue_t old_dq, dq = dc->dc_data, rq;

	old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	_dispatch_thread_setspecific(dispatch_queue_key, dq);
	_dispatch_continuation_pop(other_dc);
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);

	rq = dq->do_targetq;
	while (slowpath(rq->do_targetq) && rq != old_dq) {
		if (dispatch_atomic_sub2o(rq, dq_running, 2, relaxed) == 0) {
			_dispatch_wakeup(rq);
		}
		rq = rq->do_targetq;
	}

	if (dispatch_atomic_sub2o(dq, dq_running, 2, relaxed) == 0) {
		_dispatch_wakeup(dq);
	}
	_dispatch_release(dq);
}

static inline void
_dispatch_async_f_redirect2(dispatch_queue_t dq, dispatch_continuation_t dc)
{
	uint32_t running = 2;

	// Find the queue to redirect to
	do {
		if (slowpath(dq->dq_items_tail) ||
				slowpath(DISPATCH_OBJECT_SUSPENDED(dq)) ||
				slowpath(dq->dq_width == 1)) {
			break;
		}
		running = dispatch_atomic_add2o(dq, dq_running, 2, relaxed);
		if (slowpath(running & 1) || slowpath(running > dq->dq_width)) {
			running = dispatch_atomic_sub2o(dq, dq_running, 2, relaxed);
			break;
		}
		dq = dq->do_targetq;
	} while (slowpath(dq->do_targetq));

	_dispatch_queue_push_wakeup(dq, dc, running == 0);
}

DISPATCH_NOINLINE
static void
_dispatch_async_f_redirect(dispatch_queue_t dq,
		dispatch_continuation_t other_dc)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();

	dc->do_vtable = (void *)DISPATCH_OBJ_ASYNC_BIT;
	dc->dc_func = _dispatch_async_redirect_invoke;
	dc->dc_ctxt = dc;
	dc->dc_data = dq;
	dc->dc_other = other_dc;

	_dispatch_retain(dq);
	dq = dq->do_targetq;
	if (slowpath(dq->do_targetq)) {
		return _dispatch_async_f_redirect2(dq, dc);
	}

	_dispatch_queue_push(dq, dc);
}

DISPATCH_NOINLINE
static void
_dispatch_async_f2(dispatch_queue_t dq, dispatch_continuation_t dc)
{
	uint32_t running = 2;

	do {
		if (slowpath(dq->dq_items_tail)
				|| slowpath(DISPATCH_OBJECT_SUSPENDED(dq))) {
			break;
		}
		running = dispatch_atomic_add2o(dq, dq_running, 2, relaxed);
		if (slowpath(running > dq->dq_width)) {
			running = dispatch_atomic_sub2o(dq, dq_running, 2, relaxed);
			break;
		}
		if (!slowpath(running & 1)) {
			return _dispatch_async_f_redirect(dq, dc);
		}
		running = dispatch_atomic_sub2o(dq, dq_running, 2, relaxed);
		// We might get lucky and find that the barrier has ended by now
	} while (!(running & 1));

	_dispatch_queue_push_wakeup(dq, dc, running == 0);
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
	_dispatch_perfmon_workitem_inc();
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);
}

void
_dispatch_sync_recurse_invoke(void *ctxt)
{
	dispatch_continuation_t dc = ctxt;
	_dispatch_function_invoke(dc->dc_data, dc->dc_ctxt, dc->dc_func);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_function_recurse(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	struct dispatch_continuation_s dc = {
		.dc_data = dq,
		.dc_func = func,
		.dc_ctxt = ctxt,
	};
	dispatch_sync_f(dq->do_targetq, &dc, _dispatch_sync_recurse_invoke);
}

#pragma mark -
#pragma mark dispatch_barrier_sync

static void _dispatch_sync_f_invoke(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func);

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline _dispatch_thread_semaphore_t
_dispatch_barrier_sync_f_pop(dispatch_queue_t dq, dispatch_object_t dou,
		bool lock)
{
	_dispatch_thread_semaphore_t sema;
	dispatch_continuation_t dc = dou._dc;

	if (DISPATCH_OBJ_IS_VTABLE(dc) || ((long)dc->do_vtable &
			(DISPATCH_OBJ_BARRIER_BIT | DISPATCH_OBJ_SYNC_SLOW_BIT)) !=
			(DISPATCH_OBJ_BARRIER_BIT | DISPATCH_OBJ_SYNC_SLOW_BIT)) {
		return 0;
	}
	_dispatch_trace_continuation_pop(dq, dc);
	_dispatch_perfmon_workitem_inc();

	dc = dc->dc_ctxt;
	dq = dc->dc_data;
	sema = (_dispatch_thread_semaphore_t)dc->dc_other;
	if (lock) {
		(void)dispatch_atomic_add2o(dq, do_suspend_cnt,
				DISPATCH_OBJECT_SUSPEND_INTERVAL, relaxed);
		// rdar://problem/9032024 running lock must be held until sync_f_slow
		// returns
		(void)dispatch_atomic_add2o(dq, dq_running, 2, relaxed);
	}
	return sema ? sema : MACH_PORT_DEAD;
}

static void
_dispatch_barrier_sync_f_slow_invoke(void *ctxt)
{
	dispatch_continuation_t dc = ctxt;
	dispatch_queue_t dq = dc->dc_data;
	_dispatch_thread_semaphore_t sema;
	sema = (_dispatch_thread_semaphore_t)dc->dc_other;

	dispatch_assert(dq == _dispatch_queue_get_current());
#if DISPATCH_COCOA_COMPAT
	if (slowpath(dq->dq_is_thread_bound)) {
		// The queue is bound to a non-dispatch thread (e.g. main thread)
		dc->dc_func(dc->dc_ctxt);
		dispatch_atomic_store2o(dc, dc_func, NULL, release);
		_dispatch_thread_semaphore_signal(sema); // release
		return;
	}
#endif
	(void)dispatch_atomic_add2o(dq, do_suspend_cnt,
			DISPATCH_OBJECT_SUSPEND_INTERVAL, relaxed);
	// rdar://9032024 running lock must be held until sync_f_slow returns
	(void)dispatch_atomic_add2o(dq, dq_running, 2, relaxed);
	_dispatch_thread_semaphore_signal(sema); // release
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_f_slow(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	if (slowpath(!dq->do_targetq)) {
		// the global concurrent queues do not need strict ordering
		(void)dispatch_atomic_add2o(dq, dq_running, 2, relaxed);
		return _dispatch_sync_f_invoke(dq, ctxt, func);
	}
	// It's preferred to execute synchronous blocks on the current thread
	// due to thread-local side effects, garbage collection, etc. However,
	// blocks submitted to the main thread MUST be run on the main thread

	_dispatch_thread_semaphore_t sema = _dispatch_get_thread_semaphore();
	struct dispatch_continuation_s dc = {
		.dc_data = dq,
#if DISPATCH_COCOA_COMPAT
		.dc_func = func,
		.dc_ctxt = ctxt,
#endif
		.dc_other = (void*)sema,
	};
	struct dispatch_continuation_s dbss = {
		.do_vtable = (void *)(DISPATCH_OBJ_BARRIER_BIT |
				DISPATCH_OBJ_SYNC_SLOW_BIT),
		.dc_func = _dispatch_barrier_sync_f_slow_invoke,
		.dc_ctxt = &dc,
#if DISPATCH_INTROSPECTION
		.dc_data = (void*)_dispatch_thread_self(),
#endif
	};
	_dispatch_queue_push(dq, &dbss);

	_dispatch_thread_semaphore_wait(sema); // acquire
	_dispatch_put_thread_semaphore(sema);

#if DISPATCH_COCOA_COMPAT
	// Queue bound to a non-dispatch thread
	if (dc.dc_func == NULL) {
		return;
	}
#endif
	if (slowpath(dq->do_targetq->do_targetq)) {
		_dispatch_function_recurse(dq, ctxt, func);
	} else {
		_dispatch_function_invoke(dq, ctxt, func);
	}
	if (fastpath(dq->do_suspend_cnt < 2 * DISPATCH_OBJECT_SUSPEND_INTERVAL) &&
			dq->dq_running == 2) {
		// rdar://problem/8290662 "lock transfer"
		sema = _dispatch_queue_drain_one_barrier_sync(dq);
		if (sema) {
			_dispatch_thread_semaphore_signal(sema); // release
			return;
		}
	}
	(void)dispatch_atomic_sub2o(dq, do_suspend_cnt,
			DISPATCH_OBJECT_SUSPEND_INTERVAL, relaxed);
	if (slowpath(dispatch_atomic_sub2o(dq, dq_running, 2, release) == 0)) {
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
					DISPATCH_OBJECT_SUSPEND_INTERVAL, relaxed);
			// rdar://9032024 running lock must be held until sync_f_slow
			// returns: increment by 2 and decrement by 1
			(void)dispatch_atomic_inc2o(dq, dq_running, relaxed);
			_dispatch_thread_semaphore_signal(sema);
			return;
		}
	}
	if (slowpath(dispatch_atomic_dec2o(dq, dq_running, release) == 0)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_f_invoke(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_function_invoke(dq, ctxt, func);
	if (slowpath(dq->dq_items_tail)) {
		return _dispatch_barrier_sync_f2(dq);
	}
	if (slowpath(dispatch_atomic_dec2o(dq, dq_running, release) == 0)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_f_recurse(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_function_recurse(dq, ctxt, func);
	if (slowpath(dq->dq_items_tail)) {
		return _dispatch_barrier_sync_f2(dq);
	}
	if (slowpath(dispatch_atomic_dec2o(dq, dq_running, release) == 0)) {
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
	if (slowpath(!dispatch_atomic_cmpxchg2o(dq, dq_running, 0, 1, acquire))) {
		// global concurrent queues and queues bound to non-dispatch threads
		// always fall into the slow case
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
	dispatch_barrier_sync_f(dq, work, _dispatch_Block_invoke(work));
}
#endif

void
dispatch_barrier_sync(dispatch_queue_t dq, void (^work)(void))
{
#if DISPATCH_COCOA_COMPAT
	if (slowpath(dq->dq_is_thread_bound)) {
		return _dispatch_barrier_sync_slow(dq, work);
	}
#endif
	dispatch_barrier_sync_f(dq, work, _dispatch_Block_invoke(work));
}
#endif

DISPATCH_NOINLINE
static void
_dispatch_barrier_trysync_f_invoke(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_function_invoke(dq, ctxt, func);
	if (slowpath(dispatch_atomic_dec2o(dq, dq_running, release) == 0)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_NOINLINE
void
_dispatch_barrier_trysync_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	// Use for mutation of queue-/source-internal state only, ignores target
	// queue hierarchy!
	if (slowpath(dq->dq_items_tail) || slowpath(DISPATCH_OBJECT_SUSPENDED(dq))
			|| slowpath(!dispatch_atomic_cmpxchg2o(dq, dq_running, 0, 1,
					acquire))) {
		return dispatch_barrier_async_f(dq, ctxt, func);
	}
	_dispatch_barrier_trysync_f_invoke(dq, ctxt, func);
}

#pragma mark -
#pragma mark dispatch_sync

DISPATCH_NOINLINE
static void
_dispatch_sync_f_slow(dispatch_queue_t dq, void *ctxt, dispatch_function_t func,
		bool wakeup)
{
	_dispatch_thread_semaphore_t sema = _dispatch_get_thread_semaphore();
	struct dispatch_continuation_s dss = {
		.do_vtable = (void*)DISPATCH_OBJ_SYNC_SLOW_BIT,
#if DISPATCH_INTROSPECTION
		.dc_func = func,
		.dc_ctxt = ctxt,
		.dc_data = (void*)_dispatch_thread_self(),
#endif
		.dc_other = (void*)sema,
	};
	_dispatch_queue_push_wakeup(dq, &dss, wakeup);

	_dispatch_thread_semaphore_wait(sema);
	_dispatch_put_thread_semaphore(sema);

	if (slowpath(dq->do_targetq->do_targetq)) {
		_dispatch_function_recurse(dq, ctxt, func);
	} else {
		_dispatch_function_invoke(dq, ctxt, func);
	}
	if (slowpath(dispatch_atomic_sub2o(dq, dq_running, 2, relaxed) == 0)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_sync_f_invoke(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_function_invoke(dq, ctxt, func);
	if (slowpath(dispatch_atomic_sub2o(dq, dq_running, 2, relaxed) == 0)) {
		_dispatch_wakeup(dq);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_sync_f_recurse(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_function_recurse(dq, ctxt, func);
	if (slowpath(dispatch_atomic_sub2o(dq, dq_running, 2, relaxed) == 0)) {
		_dispatch_wakeup(dq);
	}
}

static inline void
_dispatch_sync_f2(dispatch_queue_t dq, void *ctxt, dispatch_function_t func)
{
	// 1) ensure that this thread hasn't enqueued anything ahead of this call
	// 2) the queue is not suspended
	if (slowpath(dq->dq_items_tail) || slowpath(DISPATCH_OBJECT_SUSPENDED(dq))){
		return _dispatch_sync_f_slow(dq, ctxt, func, false);
	}
	uint32_t running = dispatch_atomic_add2o(dq, dq_running, 2, relaxed);
	if (slowpath(running & 1)) {
		running = dispatch_atomic_sub2o(dq, dq_running, 2, relaxed);
		return _dispatch_sync_f_slow(dq, ctxt, func, running == 0);
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
		// the global concurrent queues do not need strict ordering
		(void)dispatch_atomic_add2o(dq, dq_running, 2, relaxed);
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
	dispatch_sync_f(dq, work, _dispatch_Block_invoke(work));
}
#endif

void
dispatch_sync(dispatch_queue_t dq, void (^work)(void))
{
#if DISPATCH_COCOA_COMPAT
	if (slowpath(dq->dq_is_thread_bound)) {
		return _dispatch_sync_slow(dq, work);
	}
#endif
	dispatch_sync_f(dq, work, _dispatch_Block_invoke(work));
}
#endif

#pragma mark -
#pragma mark dispatch_after

void
_dispatch_after_timer_callback(void *ctxt)
{
	dispatch_continuation_t dc = ctxt, dc1;
	dispatch_source_t ds = dc->dc_data;
	dc1 = _dispatch_continuation_free_cacheonly(dc);
	_dispatch_client_callout(dc->dc_ctxt, dc->dc_func);
	dispatch_source_cancel(ds);
	dispatch_release(ds);
	if (slowpath(dc1)) {
		_dispatch_continuation_free_to_cache_limit(dc1);
	}
}

DISPATCH_NOINLINE
void
dispatch_after_f(dispatch_time_t when, dispatch_queue_t queue, void *ctxt,
		dispatch_function_t func)
{
	uint64_t delta, leeway;
	dispatch_source_t ds;

	if (when == DISPATCH_TIME_FOREVER) {
#if DISPATCH_DEBUG
		DISPATCH_CLIENT_CRASH(
				"dispatch_after_f() called with 'when' == infinity");
#endif
		return;
	}

	delta = _dispatch_timeout(when);
	if (delta == 0) {
		return dispatch_async_f(queue, ctxt, func);
	}
	leeway = delta / 10; // <rdar://problem/13447496>
	if (leeway < NSEC_PER_MSEC) leeway = NSEC_PER_MSEC;
	if (leeway > 60 * NSEC_PER_SEC) leeway = 60 * NSEC_PER_SEC;

	// this function can and should be optimized to not use a dispatch source
	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
	dispatch_assert(ds);

	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	dc->do_vtable = (void *)(DISPATCH_OBJ_ASYNC_BIT | DISPATCH_OBJ_BARRIER_BIT);
	dc->dc_func = func;
	dc->dc_ctxt = ctxt;
	dc->dc_data = ds;

	dispatch_set_context(ds, dc);
	dispatch_source_set_event_handler_f(ds, _dispatch_after_timer_callback);
	dispatch_source_set_timer(ds, when, DISPATCH_TIME_FOREVER, leeway);
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
#pragma mark dispatch_queue_push

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
	if (dx_type(dq) == DISPATCH_QUEUE_ROOT_TYPE && !dq->dq_is_thread_bound) {
		dispatch_atomic_store2o(dq, dq_items_head, obj, relaxed);
		return _dispatch_queue_wakeup_global2(dq, n);
	}
	_dispatch_queue_push_list_slow2(dq, obj);
}

DISPATCH_NOINLINE
void
_dispatch_queue_push_slow(dispatch_queue_t dq,
		struct dispatch_object_s *obj)
{
	if (dx_type(dq) == DISPATCH_QUEUE_ROOT_TYPE && !dq->dq_is_thread_bound) {
		dispatch_atomic_store2o(dq, dq_items_head, obj, relaxed);
		return _dispatch_queue_wakeup_global(dq);
	}
	_dispatch_queue_push_list_slow2(dq, obj);
}

#pragma mark -
#pragma mark dispatch_queue_probe

unsigned long
_dispatch_queue_probe(dispatch_queue_t dq)
{
	return (unsigned long)slowpath(dq->dq_items_tail != NULL);
}

#if DISPATCH_COCOA_COMPAT
unsigned long
_dispatch_runloop_queue_probe(dispatch_queue_t dq)
{
	if (_dispatch_queue_probe(dq)) {
		if (dq->do_xref_cnt == -1) return true; // <rdar://problem/14026816>
		return _dispatch_runloop_queue_wakeup(dq);
	}
	return false;
}
#endif

unsigned long
_dispatch_mgr_queue_probe(dispatch_queue_t dq)
{
	if (_dispatch_queue_probe(dq)) {
		return _dispatch_mgr_wakeup(dq);
	}
	return false;
}

unsigned long
_dispatch_root_queue_probe(dispatch_queue_t dq)
{
	_dispatch_queue_wakeup_global(dq);
	return false;
}

#pragma mark -
#pragma mark dispatch_wakeup

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
dispatch_queue_t
_dispatch_wakeup(dispatch_object_t dou)
{
	if (slowpath(DISPATCH_OBJECT_SUSPENDED(dou._do))) {
		return NULL;
	}
	if (!dx_probe(dou._do)) {
		return NULL;
	}
	if (!dispatch_atomic_cmpxchg2o(dou._do, do_suspend_cnt, 0,
			DISPATCH_OBJECT_SUSPEND_LOCK, release)) {
#if DISPATCH_COCOA_COMPAT
		if (dou._dq == &_dispatch_main_q) {
			return _dispatch_main_queue_wakeup();
		}
#endif
		return NULL;
	}
	_dispatch_retain(dou._do);
	dispatch_queue_t tq = dou._do->do_targetq;
	_dispatch_queue_push(tq, dou._do);
	return tq;	// libdispatch does not need this, but the Instrument DTrace
				// probe does
}

#if DISPATCH_COCOA_COMPAT
static inline void
_dispatch_runloop_queue_wakeup_thread(dispatch_queue_t dq)
{
	mach_port_t mp = (mach_port_t)dq->do_ctxt;
	if (!mp) {
		return;
	}
	kern_return_t kr = _dispatch_send_wakeup_runloop_thread(mp, 0);
	switch (kr) {
	case MACH_SEND_TIMEOUT:
	case MACH_SEND_TIMED_OUT:
	case MACH_SEND_INVALID_DEST:
		break;
	default:
		(void)dispatch_assume_zero(kr);
		break;
	}
}

DISPATCH_NOINLINE DISPATCH_WEAK
unsigned long
_dispatch_runloop_queue_wakeup(dispatch_queue_t dq)
{
	_dispatch_runloop_queue_wakeup_thread(dq);
	return false;
}

DISPATCH_NOINLINE
static dispatch_queue_t
_dispatch_main_queue_wakeup(void)
{
	dispatch_queue_t dq = &_dispatch_main_q;
	if (!dq->dq_is_thread_bound) {
		return NULL;
	}
	dispatch_once_f(&_dispatch_main_q_port_pred, dq,
			_dispatch_runloop_queue_port_init);
	_dispatch_runloop_queue_wakeup_thread(dq);
	return NULL;
}
#endif

DISPATCH_NOINLINE
static void
_dispatch_queue_wakeup_global_slow(dispatch_queue_t dq, unsigned int n)
{
	static dispatch_once_t pred;
	dispatch_root_queue_context_t qc = dq->do_ctxt;
	uint32_t i = n;
	int r;

	_dispatch_debug_root_queue(dq, __func__);
	dispatch_once_f(&pred, NULL, _dispatch_root_queues_init);

#if HAVE_PTHREAD_WORKQUEUES
#if DISPATCH_USE_PTHREAD_POOL
	if (qc->dgq_kworkqueue != (void*)(~0ul))
#endif
	{
		_dispatch_root_queue_debug("requesting new worker thread for global "
				"queue: %p", dq);
#if DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
		if (qc->dgq_kworkqueue) {
			pthread_workitem_handle_t wh;
			unsigned int gen_cnt;
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
				qc->dgq_wq_options, (int)i);
		(void)dispatch_assume_zero(r);
#endif
		return;
	}
#endif // HAVE_PTHREAD_WORKQUEUES
#if DISPATCH_USE_PTHREAD_POOL
	if (fastpath(qc->dgq_thread_mediator)) {
		while (dispatch_semaphore_signal(qc->dgq_thread_mediator)) {
			if (!--i) {
				return;
			}
		}
	}
	uint32_t j, t_count = qc->dgq_thread_pool_size;
	do {
		if (!t_count) {
			_dispatch_root_queue_debug("pthread pool is full for root queue: "
					"%p", dq);
			return;
		}
		j = i > t_count ? t_count : i;
	} while (!dispatch_atomic_cmpxchgvw2o(qc, dgq_thread_pool_size, t_count,
			t_count - j, &t_count, relaxed));

	dispatch_pthread_root_queue_context_t pqc = qc->dgq_ctxt;
	pthread_attr_t *attr = pqc ? &pqc->dpq_thread_attr : NULL;
	pthread_t tid, *pthr = &tid;
#if DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES
	if (slowpath(dq == &_dispatch_mgr_root_queue)) {
		pthr = _dispatch_mgr_root_queue_init();
	}
#endif
	do {
		_dispatch_retain(dq);
		while ((r = pthread_create(pthr, attr, _dispatch_worker_thread, dq))) {
			if (r != EAGAIN) {
				(void)dispatch_assume_zero(r);
			}
			_dispatch_temporary_resource_shortage();
		}
		if (!attr) {
			r = pthread_detach(*pthr);
			(void)dispatch_assume_zero(r);
		}
	} while (--j);
#endif // DISPATCH_USE_PTHREAD_POOL
}

static inline void
_dispatch_queue_wakeup_global2(dispatch_queue_t dq, unsigned int n)
{
	if (!dq->dq_items_tail) {
		return;
	}
#if HAVE_PTHREAD_WORKQUEUES
	dispatch_root_queue_context_t qc = dq->do_ctxt;
	if (
#if DISPATCH_USE_PTHREAD_POOL
			(qc->dgq_kworkqueue != (void*)(~0ul)) &&
#endif
			!dispatch_atomic_cmpxchg2o(qc, dgq_pending, 0, n, relaxed)) {
		_dispatch_root_queue_debug("worker thread request still pending for "
				"global queue: %p", dq);
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

#pragma mark -
#pragma mark dispatch_queue_invoke

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_t
dispatch_queue_invoke2(dispatch_object_t dou,
		_dispatch_thread_semaphore_t *sema_ptr)
{
	dispatch_queue_t dq = dou._dq;
	dispatch_queue_t otq = dq->do_targetq;
	*sema_ptr = _dispatch_queue_drain(dq);

	if (slowpath(otq != dq->do_targetq)) {
		// An item on the queue changed the target queue
		return dq->do_targetq;
	}
	return NULL;
}

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
DISPATCH_NOINLINE
void
_dispatch_queue_invoke(dispatch_queue_t dq)
{
	_dispatch_queue_class_invoke(dq, dispatch_queue_invoke2);
}

#pragma mark -
#pragma mark dispatch_queue_drain

DISPATCH_ALWAYS_INLINE
static inline struct dispatch_object_s*
_dispatch_queue_head(dispatch_queue_t dq)
{
	struct dispatch_object_s *dc;
	while (!(dc = fastpath(dq->dq_items_head))) {
		dispatch_hardware_pause();
	}
	return dc;
}

DISPATCH_ALWAYS_INLINE
static inline struct dispatch_object_s*
_dispatch_queue_next(dispatch_queue_t dq, struct dispatch_object_s *dc)
{
	struct dispatch_object_s *next_dc;
	next_dc = fastpath(dc->do_next);
	dq->dq_items_head = next_dc;
	if (!next_dc && !dispatch_atomic_cmpxchg2o(dq, dq_items_tail, dc, NULL,
			relaxed)) {
		// Enqueue is TIGHTLY controlled, we won't wait long.
		while (!(next_dc = fastpath(dc->do_next))) {
			dispatch_hardware_pause();
		}
		dq->dq_items_head = next_dc;
	}
	return next_dc;
}

_dispatch_thread_semaphore_t
_dispatch_queue_drain(dispatch_object_t dou)
{
	dispatch_queue_t dq = dou._dq, orig_tq, old_dq;
	old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	struct dispatch_object_s *dc, *next_dc;
	_dispatch_thread_semaphore_t sema = 0;

	// Continue draining sources after target queue change rdar://8928171
	bool check_tq = (dx_type(dq) != DISPATCH_SOURCE_KEVENT_TYPE);

	orig_tq = dq->do_targetq;

	_dispatch_thread_setspecific(dispatch_queue_key, dq);
	//dispatch_debug_queue(dq, __func__);

	while (dq->dq_items_tail) {
		dc = _dispatch_queue_head(dq);
		do {
			if (DISPATCH_OBJECT_SUSPENDED(dq)) {
				goto out;
			}
			if (dq->dq_running > dq->dq_width) {
				goto out;
			}
			if (slowpath(orig_tq != dq->do_targetq) && check_tq) {
				goto out;
			}
			bool redirect = false;
			if (!fastpath(dq->dq_width == 1)) {
				if (!DISPATCH_OBJ_IS_VTABLE(dc) &&
						(long)dc->do_vtable & DISPATCH_OBJ_BARRIER_BIT) {
					if (dq->dq_running > 1) {
						goto out;
					}
				} else {
					redirect = true;
				}
			}
			next_dc = _dispatch_queue_next(dq, dc);
			if (redirect) {
				_dispatch_continuation_redirect(dq, dc);
				continue;
			}
			if ((sema = _dispatch_barrier_sync_f_pop(dq, dc, true))) {
				goto out;
			}
			_dispatch_continuation_pop(dc);
			_dispatch_perfmon_workitem_inc();
		} while ((dc = next_dc));
	}

out:
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);
	return sema;
}

#if DISPATCH_COCOA_COMPAT
static void
_dispatch_main_queue_drain(void)
{
	dispatch_queue_t dq = &_dispatch_main_q;
	if (!dq->dq_items_tail) {
		return;
	}
	struct dispatch_continuation_s marker = {
		.do_vtable = NULL,
	};
	struct dispatch_object_s *dmarker = (void*)&marker;
	_dispatch_queue_push_notrace(dq, dmarker);

	_dispatch_perfmon_start();
	dispatch_queue_t old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	_dispatch_thread_setspecific(dispatch_queue_key, dq);

	struct dispatch_object_s *dc, *next_dc;
	dc = _dispatch_queue_head(dq);
	do {
		next_dc = _dispatch_queue_next(dq, dc);
		if (dc == dmarker) {
			goto out;
		}
		_dispatch_continuation_pop(dc);
		_dispatch_perfmon_workitem_inc();
	} while ((dc = next_dc));
	DISPATCH_CRASH("Main queue corruption");

out:
	if (next_dc) {
		_dispatch_main_queue_wakeup();
	}
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);
	_dispatch_perfmon_end();
	_dispatch_force_cache_cleanup();
}

static bool
_dispatch_runloop_queue_drain_one(dispatch_queue_t dq)
{
	if (!dq->dq_items_tail) {
		return false;
	}
	_dispatch_perfmon_start();
	dispatch_queue_t old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	_dispatch_thread_setspecific(dispatch_queue_key, dq);

	struct dispatch_object_s *dc, *next_dc;
	dc = _dispatch_queue_head(dq);
	next_dc = _dispatch_queue_next(dq, dc);
	_dispatch_continuation_pop(dc);
	_dispatch_perfmon_workitem_inc();

	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);
	_dispatch_perfmon_end();
	_dispatch_force_cache_cleanup();
	return next_dc;
}
#endif

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline _dispatch_thread_semaphore_t
_dispatch_queue_drain_one_barrier_sync(dispatch_queue_t dq)
{
	// rdar://problem/8290662 "lock transfer"
	struct dispatch_object_s *dc;
	_dispatch_thread_semaphore_t sema;

	// queue is locked, or suspended and not being drained
	dc = dq->dq_items_head;
	if (slowpath(!dc) || !(sema = _dispatch_barrier_sync_f_pop(dq, dc, false))){
		return 0;
	}
	// dequeue dc, it is a barrier sync
	(void)_dispatch_queue_next(dq, dc);
	return sema;
}

void
_dispatch_mgr_queue_drain(void)
{
	dispatch_queue_t dq = &_dispatch_mgr_q;
	if (!dq->dq_items_tail) {
		return _dispatch_force_cache_cleanup();
	}
	_dispatch_perfmon_start();
	if (slowpath(_dispatch_queue_drain(dq))) {
		DISPATCH_CRASH("Sync onto manager queue");
	}
	_dispatch_perfmon_end();
	_dispatch_force_cache_cleanup();
}

#pragma mark -
#pragma mark dispatch_root_queue_drain

#ifndef DISPATCH_CONTENTION_USE_RAND
#define DISPATCH_CONTENTION_USE_RAND (!TARGET_OS_EMBEDDED)
#endif
#ifndef DISPATCH_CONTENTION_SPINS_MAX
#define DISPATCH_CONTENTION_SPINS_MAX (128 - 1)
#endif
#ifndef DISPATCH_CONTENTION_SPINS_MIN
#define DISPATCH_CONTENTION_SPINS_MIN (32 - 1)
#endif
#ifndef DISPATCH_CONTENTION_USLEEP_START
#define DISPATCH_CONTENTION_USLEEP_START 500
#endif
#ifndef DISPATCH_CONTENTION_USLEEP_MAX
#define DISPATCH_CONTENTION_USLEEP_MAX 100000
#endif

DISPATCH_NOINLINE
static bool
_dispatch_queue_concurrent_drain_one_slow(dispatch_queue_t dq)
{
	dispatch_root_queue_context_t qc = dq->do_ctxt;
	struct dispatch_object_s *const mediator = (void *)~0ul;
	bool pending = false, available = true;
	unsigned int spins, sleep_time = DISPATCH_CONTENTION_USLEEP_START;

	do {
		// Spin for a short while in case the contention is temporary -- e.g.
		// when starting up after dispatch_apply, or when executing a few
		// short continuations in a row.
#if DISPATCH_CONTENTION_USE_RAND
		// Use randomness to prevent threads from resonating at the same
		// frequency and permanently contending. All threads sharing the same
		// seed value is safe with the FreeBSD rand_r implementation.
		static unsigned int seed;
		spins = (rand_r(&seed) & DISPATCH_CONTENTION_SPINS_MAX) |
				DISPATCH_CONTENTION_SPINS_MIN;
#else
		spins = DISPATCH_CONTENTION_SPINS_MIN +
				(DISPATCH_CONTENTION_SPINS_MAX-DISPATCH_CONTENTION_SPINS_MIN)/2;
#endif
		while (spins--) {
			dispatch_hardware_pause();
			if (fastpath(dq->dq_items_head != mediator)) goto out;
		};
		// Since we have serious contention, we need to back off.
		if (!pending) {
			// Mark this queue as pending to avoid requests for further threads
			(void)dispatch_atomic_inc2o(qc, dgq_pending, relaxed);
			pending = true;
		}
		_dispatch_contention_usleep(sleep_time);
		if (fastpath(dq->dq_items_head != mediator)) goto out;
		sleep_time *= 2;
	} while (sleep_time < DISPATCH_CONTENTION_USLEEP_MAX);

	// The ratio of work to libdispatch overhead must be bad. This
	// scenario implies that there are too many threads in the pool.
	// Create a new pending thread and then exit this thread.
	// The kernel will grant a new thread when the load subsides.
	_dispatch_debug("contention on global queue: %p", dq);
	_dispatch_queue_wakeup_global(dq);
	available = false;
out:
	if (pending) {
		(void)dispatch_atomic_dec2o(qc, dgq_pending, relaxed);
	}
	return available;
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline struct dispatch_object_s *
_dispatch_queue_concurrent_drain_one(dispatch_queue_t dq)
{
	struct dispatch_object_s *head, *next, *const mediator = (void *)~0ul;

start:
	// The mediator value acts both as a "lock" and a signal
	head = dispatch_atomic_xchg2o(dq, dq_items_head, mediator, relaxed);

	if (slowpath(head == NULL)) {
		// The first xchg on the tail will tell the enqueueing thread that it
		// is safe to blindly write out to the head pointer. A cmpxchg honors
		// the algorithm.
		(void)dispatch_atomic_cmpxchg2o(dq, dq_items_head, mediator, NULL,
				relaxed);
		_dispatch_root_queue_debug("no work on global queue: %p", dq);
		return NULL;
	}

	if (slowpath(head == mediator)) {
		// This thread lost the race for ownership of the queue.
		if (fastpath(_dispatch_queue_concurrent_drain_one_slow(dq))) {
			goto start;
		}
		return NULL;
	}

	// Restore the head pointer to a sane value before returning.
	// If 'next' is NULL, then this item _might_ be the last item.
	next = fastpath(head->do_next);

	if (slowpath(!next)) {
		dispatch_atomic_store2o(dq, dq_items_head, NULL, relaxed);

		if (dispatch_atomic_cmpxchg2o(dq, dq_items_tail, head, NULL, relaxed)) {
			// both head and tail are NULL now
			goto out;
		}

		// There must be a next item now. This thread won't wait long.
		while (!(next = head->do_next)) {
			dispatch_hardware_pause();
		}
	}

	dispatch_atomic_store2o(dq, dq_items_head, next, relaxed);
	_dispatch_queue_wakeup_global(dq);
out:
	return head;
}

static void
_dispatch_root_queue_drain(dispatch_queue_t dq)
{
#if DISPATCH_DEBUG
	if (_dispatch_thread_getspecific(dispatch_queue_key)) {
		DISPATCH_CRASH("Premature thread recycling");
	}
#endif
	_dispatch_thread_setspecific(dispatch_queue_key, dq);

#if DISPATCH_COCOA_COMPAT
	// ensure that high-level memory management techniques do not leak/crash
	if (dispatch_begin_thread_4GC) {
		dispatch_begin_thread_4GC();
	}
	void *pool = _dispatch_autorelease_pool_push();
#endif // DISPATCH_COCOA_COMPAT

	_dispatch_perfmon_start();
	struct dispatch_object_s *item;
	while ((item = fastpath(_dispatch_queue_concurrent_drain_one(dq)))) {
		_dispatch_continuation_pop(item);
	}
	_dispatch_perfmon_end();

#if DISPATCH_COCOA_COMPAT
	_dispatch_autorelease_pool_pop(pool);
	if (dispatch_end_thread_4GC) {
		dispatch_end_thread_4GC();
	}
#endif // DISPATCH_COCOA_COMPAT

	_dispatch_thread_setspecific(dispatch_queue_key, NULL);
}

#pragma mark -
#pragma mark dispatch_worker_thread

#if HAVE_PTHREAD_WORKQUEUES
static void
_dispatch_worker_thread3(void *context)
{
	dispatch_queue_t dq = context;
	dispatch_root_queue_context_t qc = dq->do_ctxt;

	_dispatch_introspection_thread_add();

	(void)dispatch_atomic_dec2o(qc, dgq_pending, relaxed);
	_dispatch_root_queue_drain(dq);
	__asm__(""); // prevent tailcall (for Instrument DTrace probe)

}

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

	return _dispatch_worker_thread3(dq);
}
#endif // HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
#endif // HAVE_PTHREAD_WORKQUEUES

#if DISPATCH_USE_PTHREAD_POOL
// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
static void *
_dispatch_worker_thread(void *context)
{
	dispatch_queue_t dq = context;
	dispatch_root_queue_context_t qc = dq->do_ctxt;
	dispatch_pthread_root_queue_context_t pqc = qc->dgq_ctxt;

	if (pqc && pqc->dpq_thread_configure) {
		pqc->dpq_thread_configure();
	}

	sigset_t mask;
	int r;
	// workaround tweaks the kernel workqueue does for us
	r = sigfillset(&mask);
	(void)dispatch_assume_zero(r);
	r = _dispatch_pthread_sigmask(SIG_BLOCK, &mask, NULL);
	(void)dispatch_assume_zero(r);
	_dispatch_introspection_thread_add();

	// Non-pthread-root-queue pthreads use a 65 second timeout in case there
	// are any timers that run once a minute <rdar://problem/11744973>
	const int64_t timeout = (pqc ? 5ull : 65ull) * NSEC_PER_SEC;

	do {
		_dispatch_root_queue_drain(dq);
	} while (dispatch_semaphore_wait(qc->dgq_thread_mediator,
			dispatch_time(0, timeout)) == 0);

	(void)dispatch_atomic_inc2o(qc, dgq_thread_pool_size, relaxed);
	_dispatch_queue_wakeup_global(dq);
	_dispatch_release(dq);

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
#endif // DISPATCH_USE_PTHREAD_POOL

#pragma mark -
#pragma mark dispatch_runloop_queue

static bool _dispatch_program_is_probably_callback_driven;

#if DISPATCH_COCOA_COMPAT

dispatch_queue_t
_dispatch_runloop_root_queue_create_4CF(const char *label, unsigned long flags)
{
	dispatch_queue_t dq;
	size_t dqs;

	if (slowpath(flags)) {
		return NULL;
	}
	dqs = sizeof(struct dispatch_queue_s) - DISPATCH_QUEUE_CACHELINE_PAD;
	dq = _dispatch_alloc(DISPATCH_VTABLE(queue_runloop), dqs);
	_dispatch_queue_init(dq);
	dq->do_targetq = _dispatch_get_root_queue(0, true);
	dq->dq_label = label ? label : "runloop-queue"; // no-copy contract
	dq->do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK;
	dq->dq_running = 1;
	dq->dq_is_thread_bound = 1;
	_dispatch_runloop_queue_port_init(dq);
	_dispatch_queue_set_bound_thread(dq);
	_dispatch_object_debug(dq, "%s", __func__);
	return _dispatch_introspection_queue_create(dq);
}

void
_dispatch_runloop_queue_xref_dispose(dispatch_queue_t dq)
{
	_dispatch_object_debug(dq, "%s", __func__);
	(void)dispatch_atomic_dec2o(dq, dq_running, relaxed);
	unsigned int suspend_cnt = dispatch_atomic_sub2o(dq, do_suspend_cnt,
			DISPATCH_OBJECT_SUSPEND_LOCK, release);
	_dispatch_queue_clear_bound_thread(dq);
	if (suspend_cnt == 0) {
		_dispatch_wakeup(dq);
	}
}

void
_dispatch_runloop_queue_dispose(dispatch_queue_t dq)
{
	_dispatch_object_debug(dq, "%s", __func__);
	_dispatch_introspection_queue_dispose(dq);
	_dispatch_runloop_queue_port_dispose(dq);
	_dispatch_queue_destroy(dq);
}

bool
_dispatch_runloop_root_queue_perform_4CF(dispatch_queue_t dq)
{
	if (slowpath(dq->do_vtable != DISPATCH_VTABLE(queue_runloop))) {
		DISPATCH_CLIENT_CRASH("Not a runloop queue");
	}
	dispatch_retain(dq);
	bool r = _dispatch_runloop_queue_drain_one(dq);
	dispatch_release(dq);
	return r;
}

void
_dispatch_runloop_root_queue_wakeup_4CF(dispatch_queue_t dq)
{
	if (slowpath(dq->do_vtable != DISPATCH_VTABLE(queue_runloop))) {
		DISPATCH_CLIENT_CRASH("Not a runloop queue");
	}
	_dispatch_runloop_queue_probe(dq);
}

mach_port_t
_dispatch_runloop_root_queue_get_port_4CF(dispatch_queue_t dq)
{
	if (slowpath(dq->do_vtable != DISPATCH_VTABLE(queue_runloop))) {
		DISPATCH_CLIENT_CRASH("Not a runloop queue");
	}
	return (mach_port_t)dq->do_ctxt;
}

static void
_dispatch_runloop_queue_port_init(void *ctxt)
{
	dispatch_queue_t dq = (dispatch_queue_t)ctxt;
	mach_port_t mp;
	kern_return_t kr;

	_dispatch_safe_fork = false;
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &mp);
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
	kr = mach_port_insert_right(mach_task_self(), mp, mp,
			MACH_MSG_TYPE_MAKE_SEND);
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
	if (dq != &_dispatch_main_q) {
		struct mach_port_limits limits = {
			.mpl_qlimit = 1,
		};
		kr = mach_port_set_attributes(mach_task_self(), mp,
				MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits,
				sizeof(limits));
		DISPATCH_VERIFY_MIG(kr);
		(void)dispatch_assume_zero(kr);
	}
	dq->do_ctxt = (void*)(uintptr_t)mp;

	_dispatch_program_is_probably_callback_driven = true;
}

static void
_dispatch_runloop_queue_port_dispose(dispatch_queue_t dq)
{
	mach_port_t mp = (mach_port_t)dq->do_ctxt;
	if (!mp) {
		return;
	}
	dq->do_ctxt = NULL;
	kern_return_t kr = mach_port_deallocate(mach_task_self(), mp);
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
	kr = mach_port_mod_refs(mach_task_self(), mp, MACH_PORT_RIGHT_RECEIVE, -1);
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
}

#pragma mark -
#pragma mark dispatch_main_queue

mach_port_t
_dispatch_get_main_queue_port_4CF(void)
{
	dispatch_queue_t dq = &_dispatch_main_q;
	dispatch_once_f(&_dispatch_main_q_port_pred, dq,
			_dispatch_runloop_queue_port_init);
	return (mach_port_t)dq->do_ctxt;
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
		_dispatch_object_debug(&_dispatch_main_q, "%s", __func__);
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
	dispatch_queue_t dq = &_dispatch_main_q;
	(void)dispatch_atomic_dec2o(dq, dq_running, relaxed);
	unsigned int suspend_cnt = dispatch_atomic_sub2o(dq, do_suspend_cnt,
			DISPATCH_OBJECT_SUSPEND_LOCK, release);
	dq->dq_is_thread_bound = 0;
	if (suspend_cnt == 0) {
		_dispatch_wakeup(dq);
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
	dispatch_once_f(&_dispatch_main_q_port_pred, dq,
			_dispatch_runloop_queue_port_init);
	_dispatch_runloop_queue_port_dispose(dq);
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
