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
#if HAVE_PTHREAD_WORKQUEUES && (!HAVE_PTHREAD_WORKQUEUE_QOS || DISPATCH_DEBUG) \
		&& !defined(DISPATCH_USE_NOQOS_WORKQUEUE_FALLBACK)
#define DISPATCH_USE_NOQOS_WORKQUEUE_FALLBACK 1
#endif
#if HAVE_PTHREAD_WORKQUEUES && DISPATCH_USE_NOQOS_WORKQUEUE_FALLBACK && \
		!HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP && \
		!defined(DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK)
#define DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK 1
#endif
#if HAVE_PTHREAD_WORKQUEUE_QOS && !DISPATCH_USE_NOQOS_WORKQUEUE_FALLBACK
#undef HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
#define HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP 0
#endif
#if HAVE_PTHREAD_WORKQUEUES && DISPATCH_USE_PTHREAD_POOL && \
		!DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
#define pthread_workqueue_t void*
#endif

static void _dispatch_sig_thread(void *ctxt);
static void _dispatch_cache_cleanup(void *value);
static void _dispatch_sync_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func, pthread_priority_t pp);
static void _dispatch_async_f2(dispatch_queue_t dq, dispatch_continuation_t dc);
static void _dispatch_queue_cleanup(void *ctxt);
static void _dispatch_deferred_items_cleanup(void *ctxt);
static void _dispatch_frame_cleanup(void *ctxt);
static void _dispatch_context_cleanup(void *ctxt);
static void _dispatch_non_barrier_complete(dispatch_queue_t dq);
static inline void _dispatch_global_queue_poke(dispatch_queue_t dq);
#if HAVE_PTHREAD_WORKQUEUES
static void _dispatch_worker_thread4(void *context);
#if HAVE_PTHREAD_WORKQUEUE_QOS
static void _dispatch_worker_thread3(pthread_priority_t priority);
#endif
#if HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
static void _dispatch_worker_thread2(int priority, int options, void *context);
#endif
#endif
#if DISPATCH_USE_PTHREAD_POOL
static void *_dispatch_worker_thread(void *context);
static int _dispatch_pthread_sigmask(int how, sigset_t *set, sigset_t *oset);
#endif

#if DISPATCH_COCOA_COMPAT
static dispatch_once_t _dispatch_main_q_handle_pred;
static void _dispatch_runloop_queue_poke(dispatch_queue_t dq,
		pthread_priority_t pp, dispatch_wakeup_flags_t flags);
static void _dispatch_runloop_queue_handle_init(void *ctxt);
static void _dispatch_runloop_queue_handle_dispose(dispatch_queue_t dq);
#endif

static void _dispatch_root_queues_init_once(void *context);
static dispatch_once_t _dispatch_root_queues_pred;

#pragma mark -
#pragma mark dispatch_root_queue

struct dispatch_pthread_root_queue_context_s {
	pthread_attr_t dpq_thread_attr;
	dispatch_block_t dpq_thread_configure;
	struct dispatch_semaphore_s dpq_thread_mediator;
	dispatch_pthread_root_queue_observer_hooks_s dpq_observer_hooks;
};
typedef struct dispatch_pthread_root_queue_context_s *
		dispatch_pthread_root_queue_context_t;

#if DISPATCH_ENABLE_THREAD_POOL
static struct dispatch_pthread_root_queue_context_s
		_dispatch_pthread_root_queue_contexts[] = {
	[DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS] = {
		.dpq_thread_mediator = {
			DISPATCH_GLOBAL_OBJECT_HEADER(semaphore),
	}},
	[DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS_OVERCOMMIT] = {
		.dpq_thread_mediator = {
			DISPATCH_GLOBAL_OBJECT_HEADER(semaphore),
	}},
	[DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS] = {
		.dpq_thread_mediator = {
			DISPATCH_GLOBAL_OBJECT_HEADER(semaphore),
	}},
	[DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS_OVERCOMMIT] = {
		.dpq_thread_mediator = {
			DISPATCH_GLOBAL_OBJECT_HEADER(semaphore),
	}},
	[DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS] = {
		.dpq_thread_mediator = {
			DISPATCH_GLOBAL_OBJECT_HEADER(semaphore),
	}},
	[DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS_OVERCOMMIT] = {
		.dpq_thread_mediator = {
			DISPATCH_GLOBAL_OBJECT_HEADER(semaphore),
	}},
	[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS] = {
		.dpq_thread_mediator = {
			DISPATCH_GLOBAL_OBJECT_HEADER(semaphore),
	}},
	[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS_OVERCOMMIT] = {
		.dpq_thread_mediator = {
			DISPATCH_GLOBAL_OBJECT_HEADER(semaphore),
	}},
	[DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS] = {
		.dpq_thread_mediator = {
			DISPATCH_GLOBAL_OBJECT_HEADER(semaphore),
	}},
	[DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS_OVERCOMMIT] = {
		.dpq_thread_mediator = {
			DISPATCH_GLOBAL_OBJECT_HEADER(semaphore),
	}},
	[DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS] = {
		.dpq_thread_mediator = {
			DISPATCH_GLOBAL_OBJECT_HEADER(semaphore),
	}},
	[DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS_OVERCOMMIT] = {
		.dpq_thread_mediator = {
			DISPATCH_GLOBAL_OBJECT_HEADER(semaphore),
	}},
};
#endif

#define MAX_PTHREAD_COUNT 255

struct dispatch_root_queue_context_s {
	union {
		struct {
			unsigned int volatile dgq_pending;
#if HAVE_PTHREAD_WORKQUEUES
			qos_class_t dgq_qos;
			int dgq_wq_priority, dgq_wq_options;
#if DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK || DISPATCH_USE_PTHREAD_POOL
			pthread_workqueue_t dgq_kworkqueue;
#endif
#endif // HAVE_PTHREAD_WORKQUEUES
#if DISPATCH_USE_PTHREAD_POOL
			void *dgq_ctxt;
			uint32_t volatile dgq_thread_pool_size;
#endif
		};
		char _dgq_pad[DISPATCH_CACHELINE_SIZE];
	};
};
typedef struct dispatch_root_queue_context_s *dispatch_root_queue_context_t;

#define WORKQ_PRIO_INVALID (-1)
#ifndef WORKQ_BG_PRIOQUEUE_CONDITIONAL
#define WORKQ_BG_PRIOQUEUE_CONDITIONAL WORKQ_PRIO_INVALID
#endif
#ifndef WORKQ_HIGH_PRIOQUEUE_CONDITIONAL
#define WORKQ_HIGH_PRIOQUEUE_CONDITIONAL WORKQ_PRIO_INVALID
#endif

DISPATCH_CACHELINE_ALIGN
static struct dispatch_root_queue_context_s _dispatch_root_queue_contexts[] = {
	[DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_qos = _DISPATCH_QOS_CLASS_MAINTENANCE,
		.dgq_wq_priority = WORKQ_BG_PRIOQUEUE,
		.dgq_wq_options = 0,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_ctxt = &_dispatch_pthread_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS],
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS_OVERCOMMIT] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_qos = _DISPATCH_QOS_CLASS_MAINTENANCE,
		.dgq_wq_priority = WORKQ_BG_PRIOQUEUE,
		.dgq_wq_options = WORKQ_ADDTHREADS_OPTION_OVERCOMMIT,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_ctxt = &_dispatch_pthread_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS_OVERCOMMIT],
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_qos = _DISPATCH_QOS_CLASS_BACKGROUND,
		.dgq_wq_priority = WORKQ_BG_PRIOQUEUE_CONDITIONAL,
		.dgq_wq_options = 0,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_ctxt = &_dispatch_pthread_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS],
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS_OVERCOMMIT] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_qos = _DISPATCH_QOS_CLASS_BACKGROUND,
		.dgq_wq_priority = WORKQ_BG_PRIOQUEUE_CONDITIONAL,
		.dgq_wq_options = WORKQ_ADDTHREADS_OPTION_OVERCOMMIT,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_ctxt = &_dispatch_pthread_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS_OVERCOMMIT],
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_qos = _DISPATCH_QOS_CLASS_UTILITY,
		.dgq_wq_priority = WORKQ_LOW_PRIOQUEUE,
		.dgq_wq_options = 0,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_ctxt = &_dispatch_pthread_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS],
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS_OVERCOMMIT] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_qos = _DISPATCH_QOS_CLASS_UTILITY,
		.dgq_wq_priority = WORKQ_LOW_PRIOQUEUE,
		.dgq_wq_options = WORKQ_ADDTHREADS_OPTION_OVERCOMMIT,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_ctxt = &_dispatch_pthread_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS_OVERCOMMIT],
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_qos = _DISPATCH_QOS_CLASS_DEFAULT,
		.dgq_wq_priority = WORKQ_DEFAULT_PRIOQUEUE,
		.dgq_wq_options = 0,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_ctxt = &_dispatch_pthread_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS],
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS_OVERCOMMIT] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_qos = _DISPATCH_QOS_CLASS_DEFAULT,
		.dgq_wq_priority = WORKQ_DEFAULT_PRIOQUEUE,
		.dgq_wq_options = WORKQ_ADDTHREADS_OPTION_OVERCOMMIT,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_ctxt = &_dispatch_pthread_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS_OVERCOMMIT],
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_qos = _DISPATCH_QOS_CLASS_USER_INITIATED,
		.dgq_wq_priority = WORKQ_HIGH_PRIOQUEUE,
		.dgq_wq_options = 0,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_ctxt = &_dispatch_pthread_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS],
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS_OVERCOMMIT] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_qos = _DISPATCH_QOS_CLASS_USER_INITIATED,
		.dgq_wq_priority = WORKQ_HIGH_PRIOQUEUE,
		.dgq_wq_options = WORKQ_ADDTHREADS_OPTION_OVERCOMMIT,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_ctxt = &_dispatch_pthread_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS_OVERCOMMIT],
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_qos = _DISPATCH_QOS_CLASS_USER_INTERACTIVE,
		.dgq_wq_priority = WORKQ_HIGH_PRIOQUEUE_CONDITIONAL,
		.dgq_wq_options = 0,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_ctxt = &_dispatch_pthread_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS],
#endif
	}}},
	[DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS_OVERCOMMIT] = {{{
#if HAVE_PTHREAD_WORKQUEUES
		.dgq_qos = _DISPATCH_QOS_CLASS_USER_INTERACTIVE,
		.dgq_wq_priority = WORKQ_HIGH_PRIOQUEUE_CONDITIONAL,
		.dgq_wq_options = WORKQ_ADDTHREADS_OPTION_OVERCOMMIT,
#endif
#if DISPATCH_ENABLE_THREAD_POOL
		.dgq_ctxt = &_dispatch_pthread_root_queue_contexts[
				DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS_OVERCOMMIT],
#endif
	}}},
};

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
DISPATCH_CACHELINE_ALIGN
struct dispatch_queue_s _dispatch_root_queues[] = {
#define _DISPATCH_ROOT_QUEUE_ENTRY(n, ...) \
	[DISPATCH_ROOT_QUEUE_IDX_##n] = { \
		DISPATCH_GLOBAL_OBJECT_HEADER(queue_root), \
		.dq_state = DISPATCH_ROOT_QUEUE_STATE_INIT_VALUE, \
		.do_ctxt = &_dispatch_root_queue_contexts[ \
				DISPATCH_ROOT_QUEUE_IDX_##n], \
		.dq_width = DISPATCH_QUEUE_WIDTH_POOL, \
		.dq_override_voucher = DISPATCH_NO_VOUCHER, \
		.dq_override = DISPATCH_SATURATED_OVERRIDE, \
		__VA_ARGS__ \
	}
	_DISPATCH_ROOT_QUEUE_ENTRY(MAINTENANCE_QOS,
		.dq_label = "com.apple.root.maintenance-qos",
		.dq_serialnum = 4,
	),
	_DISPATCH_ROOT_QUEUE_ENTRY(MAINTENANCE_QOS_OVERCOMMIT,
		.dq_label = "com.apple.root.maintenance-qos.overcommit",
		.dq_serialnum = 5,
	),
	_DISPATCH_ROOT_QUEUE_ENTRY(BACKGROUND_QOS,
		.dq_label = "com.apple.root.background-qos",
		.dq_serialnum = 6,
	),
	_DISPATCH_ROOT_QUEUE_ENTRY(BACKGROUND_QOS_OVERCOMMIT,
		.dq_label = "com.apple.root.background-qos.overcommit",
		.dq_serialnum = 7,
	),
	_DISPATCH_ROOT_QUEUE_ENTRY(UTILITY_QOS,
		.dq_label = "com.apple.root.utility-qos",
		.dq_serialnum = 8,
	),
	_DISPATCH_ROOT_QUEUE_ENTRY(UTILITY_QOS_OVERCOMMIT,
		.dq_label = "com.apple.root.utility-qos.overcommit",
		.dq_serialnum = 9,
	),
	_DISPATCH_ROOT_QUEUE_ENTRY(DEFAULT_QOS,
		.dq_label = "com.apple.root.default-qos",
		.dq_serialnum = 10,
	),
	_DISPATCH_ROOT_QUEUE_ENTRY(DEFAULT_QOS_OVERCOMMIT,
		.dq_label = "com.apple.root.default-qos.overcommit",
		.dq_serialnum = 11,
	),
	_DISPATCH_ROOT_QUEUE_ENTRY(USER_INITIATED_QOS,
		.dq_label = "com.apple.root.user-initiated-qos",
		.dq_serialnum = 12,
	),
	_DISPATCH_ROOT_QUEUE_ENTRY(USER_INITIATED_QOS_OVERCOMMIT,
		.dq_label = "com.apple.root.user-initiated-qos.overcommit",
		.dq_serialnum = 13,
	),
	_DISPATCH_ROOT_QUEUE_ENTRY(USER_INTERACTIVE_QOS,
		.dq_label = "com.apple.root.user-interactive-qos",
		.dq_serialnum = 14,
	),
	_DISPATCH_ROOT_QUEUE_ENTRY(USER_INTERACTIVE_QOS_OVERCOMMIT,
		.dq_label = "com.apple.root.user-interactive-qos.overcommit",
		.dq_serialnum = 15,
	),
};

#if HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
static const dispatch_queue_t _dispatch_wq2root_queues[][2] = {
	[WORKQ_BG_PRIOQUEUE][0] = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS],
	[WORKQ_BG_PRIOQUEUE][WORKQ_ADDTHREADS_OPTION_OVERCOMMIT] =
			&_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS_OVERCOMMIT],
	[WORKQ_LOW_PRIOQUEUE][0] = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS],
	[WORKQ_LOW_PRIOQUEUE][WORKQ_ADDTHREADS_OPTION_OVERCOMMIT] =
			&_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS_OVERCOMMIT],
	[WORKQ_DEFAULT_PRIOQUEUE][0] = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS],
	[WORKQ_DEFAULT_PRIOQUEUE][WORKQ_ADDTHREADS_OPTION_OVERCOMMIT] =
			&_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS_OVERCOMMIT],
	[WORKQ_HIGH_PRIOQUEUE][0] = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS],
	[WORKQ_HIGH_PRIOQUEUE][WORKQ_ADDTHREADS_OPTION_OVERCOMMIT] =
			&_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS_OVERCOMMIT],
};
#endif // HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP

#define DISPATCH_PRIORITY_COUNT 5

enum {
	// No DISPATCH_PRIORITY_IDX_MAINTENANCE define because there is no legacy
	// maintenance priority
	DISPATCH_PRIORITY_IDX_BACKGROUND = 0,
	DISPATCH_PRIORITY_IDX_NON_INTERACTIVE,
	DISPATCH_PRIORITY_IDX_LOW,
	DISPATCH_PRIORITY_IDX_DEFAULT,
	DISPATCH_PRIORITY_IDX_HIGH,
};

static qos_class_t _dispatch_priority2qos[] = {
	[DISPATCH_PRIORITY_IDX_BACKGROUND] = _DISPATCH_QOS_CLASS_BACKGROUND,
	[DISPATCH_PRIORITY_IDX_NON_INTERACTIVE] = _DISPATCH_QOS_CLASS_UTILITY,
	[DISPATCH_PRIORITY_IDX_LOW] = _DISPATCH_QOS_CLASS_UTILITY,
	[DISPATCH_PRIORITY_IDX_DEFAULT] = _DISPATCH_QOS_CLASS_DEFAULT,
	[DISPATCH_PRIORITY_IDX_HIGH] = _DISPATCH_QOS_CLASS_USER_INITIATED,
};

#if HAVE_PTHREAD_WORKQUEUE_QOS
static const int _dispatch_priority2wq[] = {
	[DISPATCH_PRIORITY_IDX_BACKGROUND] = WORKQ_BG_PRIOQUEUE,
	[DISPATCH_PRIORITY_IDX_NON_INTERACTIVE] = WORKQ_NON_INTERACTIVE_PRIOQUEUE,
	[DISPATCH_PRIORITY_IDX_LOW] = WORKQ_LOW_PRIOQUEUE,
	[DISPATCH_PRIORITY_IDX_DEFAULT] = WORKQ_DEFAULT_PRIOQUEUE,
	[DISPATCH_PRIORITY_IDX_HIGH] = WORKQ_HIGH_PRIOQUEUE,
};
#endif

#if DISPATCH_USE_MGR_THREAD && DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES
static struct dispatch_queue_s _dispatch_mgr_root_queue;
#else
#define _dispatch_mgr_root_queue _dispatch_root_queues[\
		DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS_OVERCOMMIT]
#endif

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
DISPATCH_CACHELINE_ALIGN
struct dispatch_queue_s _dispatch_mgr_q = {
	DISPATCH_GLOBAL_OBJECT_HEADER(queue_mgr),
	.dq_state = DISPATCH_QUEUE_STATE_INIT_VALUE(1),
	.do_targetq = &_dispatch_mgr_root_queue,
	.dq_label = "com.apple.libdispatch-manager",
	.dq_width = 1,
	.dq_override_voucher = DISPATCH_NO_VOUCHER,
	.dq_override = DISPATCH_SATURATED_OVERRIDE,
	.dq_serialnum = 2,
};

dispatch_queue_t
dispatch_get_global_queue(long priority, unsigned long flags)
{
	if (flags & ~(unsigned long)DISPATCH_QUEUE_OVERCOMMIT) {
		return DISPATCH_BAD_INPUT;
	}
	dispatch_once_f(&_dispatch_root_queues_pred, NULL,
			_dispatch_root_queues_init_once);
	qos_class_t qos;
	switch (priority) {
#if DISPATCH_USE_NOQOS_WORKQUEUE_FALLBACK
	case _DISPATCH_QOS_CLASS_MAINTENANCE:
		if (!_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS]
				.dq_priority) {
			// map maintenance to background on old kernel
			qos = _dispatch_priority2qos[DISPATCH_PRIORITY_IDX_BACKGROUND];
		} else {
			qos = (qos_class_t)priority;
		}
		break;
#endif // DISPATCH_USE_NOQOS_WORKQUEUE_FALLBACK
	case DISPATCH_QUEUE_PRIORITY_BACKGROUND:
		qos = _dispatch_priority2qos[DISPATCH_PRIORITY_IDX_BACKGROUND];
		break;
	case DISPATCH_QUEUE_PRIORITY_NON_INTERACTIVE:
		qos = _dispatch_priority2qos[DISPATCH_PRIORITY_IDX_NON_INTERACTIVE];
		break;
	case DISPATCH_QUEUE_PRIORITY_LOW:
		qos = _dispatch_priority2qos[DISPATCH_PRIORITY_IDX_LOW];
		break;
	case DISPATCH_QUEUE_PRIORITY_DEFAULT:
		qos = _dispatch_priority2qos[DISPATCH_PRIORITY_IDX_DEFAULT];
		break;
	case DISPATCH_QUEUE_PRIORITY_HIGH:
		qos = _dispatch_priority2qos[DISPATCH_PRIORITY_IDX_HIGH];
		break;
	case _DISPATCH_QOS_CLASS_USER_INTERACTIVE:
#if DISPATCH_USE_NOQOS_WORKQUEUE_FALLBACK
		if (!_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS]
				.dq_priority) {
			qos = _dispatch_priority2qos[DISPATCH_PRIORITY_IDX_HIGH];
			break;
		}
#endif
		// fallthrough
	default:
		qos = (qos_class_t)priority;
		break;
	}
	return _dispatch_get_root_queue(qos, flags & DISPATCH_QUEUE_OVERCOMMIT);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_t
_dispatch_get_current_queue(void)
{
	return _dispatch_queue_get_current() ?:
			_dispatch_get_root_queue(_DISPATCH_QOS_CLASS_DEFAULT, true);
}

dispatch_queue_t
dispatch_get_current_queue(void)
{
	return _dispatch_get_current_queue();
}

DISPATCH_NOINLINE DISPATCH_NORETURN
static void
_dispatch_assert_queue_fail(dispatch_queue_t dq, bool expected)
{
	_dispatch_client_assert_fail(
			"Block was %sexpected to execute on queue [%s]",
			expected ? "" : "not ", dq->dq_label ?: "");
}

DISPATCH_NOINLINE DISPATCH_NORETURN
static void
_dispatch_assert_queue_barrier_fail(dispatch_queue_t dq)
{
	_dispatch_client_assert_fail(
			"Block was expected to act as a barrier on queue [%s]",
			dq->dq_label ?: "");
}

void
dispatch_assert_queue(dispatch_queue_t dq)
{
	unsigned long metatype = dx_metatype(dq);
	if (unlikely(metatype != _DISPATCH_QUEUE_TYPE)) {
		DISPATCH_CLIENT_CRASH(metatype, "invalid queue passed to "
				"dispatch_assert_queue()");
	}
	uint64_t dq_state = os_atomic_load2o(dq, dq_state, relaxed);
	if (unlikely(_dq_state_drain_pended(dq_state))) {
		goto fail;
	}
	if (likely(_dq_state_drain_owner(dq_state) == _dispatch_tid_self())) {
		return;
	}
	if (likely(dq->dq_width > 1)) {
		// we can look at the width: if it is changing while we read it,
		// it means that a barrier is running on `dq` concurrently, which
		// proves that we're not on `dq`. Hence reading a stale '1' is ok.
		if (fastpath(_dispatch_thread_frame_find_queue(dq))) {
			return;
		}
	}
fail:
	_dispatch_assert_queue_fail(dq, true);
}

void
dispatch_assert_queue_not(dispatch_queue_t dq)
{
	unsigned long metatype = dx_metatype(dq);
	if (unlikely(metatype != _DISPATCH_QUEUE_TYPE)) {
		DISPATCH_CLIENT_CRASH(metatype, "invalid queue passed to "
				"dispatch_assert_queue_not()");
	}
	uint64_t dq_state = os_atomic_load2o(dq, dq_state, relaxed);
	if (_dq_state_drain_pended(dq_state)) {
		return;
	}
	if (likely(_dq_state_drain_owner(dq_state) != _dispatch_tid_self())) {
		if (likely(dq->dq_width == 1)) {
			// we can look at the width: if it is changing while we read it,
			// it means that a barrier is running on `dq` concurrently, which
			// proves that we're not on `dq`. Hence reading a stale '1' is ok.
			return;
		}
		if (likely(!_dispatch_thread_frame_find_queue(dq))) {
			return;
		}
	}
	_dispatch_assert_queue_fail(dq, false);
}

void
dispatch_assert_queue_barrier(dispatch_queue_t dq)
{
	dispatch_assert_queue(dq);

	if (likely(dq->dq_width == 1)) {
		return;
	}

	if (likely(dq->do_targetq)) {
		uint64_t dq_state = os_atomic_load2o(dq, dq_state, relaxed);
		if (likely(_dq_state_is_in_barrier(dq_state))) {
			return;
		}
	}

	_dispatch_assert_queue_barrier_fail(dq);
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

#if HAVE_PTHREAD_WORKQUEUE_QOS
pthread_priority_t _dispatch_background_priority;
pthread_priority_t _dispatch_user_initiated_priority;

static void
_dispatch_root_queues_init_qos(int supported)
{
	pthread_priority_t p;
	qos_class_t qos;
	unsigned int i;
	for (i = 0; i < DISPATCH_PRIORITY_COUNT; i++) {
		p = _pthread_qos_class_encode_workqueue(_dispatch_priority2wq[i], 0);
		qos = _pthread_qos_class_decode(p, NULL, NULL);
		dispatch_assert(qos != _DISPATCH_QOS_CLASS_UNSPECIFIED);
		_dispatch_priority2qos[i] = qos;
	}
	for (i = 0; i < DISPATCH_ROOT_QUEUE_COUNT; i++) {
		qos = _dispatch_root_queue_contexts[i].dgq_qos;
		if (qos == _DISPATCH_QOS_CLASS_MAINTENANCE &&
				!(supported & WORKQ_FEATURE_MAINTENANCE)) {
			continue;
		}
		unsigned long flags = i & 1 ? _PTHREAD_PRIORITY_OVERCOMMIT_FLAG : 0;
		flags |= _PTHREAD_PRIORITY_ROOTQUEUE_FLAG;
		if (i == DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS ||
				i == DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS_OVERCOMMIT) {
			flags |= _PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG;
		}
		p = _pthread_qos_class_encode(qos, 0, flags);
		_dispatch_root_queues[i].dq_priority = (dispatch_priority_t)p;
	}
}
#endif // HAVE_PTHREAD_WORKQUEUE_QOS

static inline bool
_dispatch_root_queues_init_workq(int *wq_supported)
{
	int r;
	bool result = false;
	*wq_supported = 0;
#if HAVE_PTHREAD_WORKQUEUES
	bool disable_wq = false;
#if DISPATCH_ENABLE_THREAD_POOL && DISPATCH_DEBUG
	disable_wq = slowpath(getenv("LIBDISPATCH_DISABLE_KWQ"));
#endif
#if DISPATCH_USE_KEVENT_WORKQUEUE || HAVE_PTHREAD_WORKQUEUE_QOS
	bool disable_qos = false;
#if DISPATCH_DEBUG
	disable_qos = slowpath(getenv("LIBDISPATCH_DISABLE_QOS"));
#endif
#if DISPATCH_USE_KEVENT_WORKQUEUE
	bool disable_kevent_wq = false;
#if DISPATCH_DEBUG
	disable_kevent_wq = slowpath(getenv("LIBDISPATCH_DISABLE_KEVENT_WQ"));
#endif
#endif
	if (!disable_wq && !disable_qos) {
		*wq_supported = _pthread_workqueue_supported();
#if DISPATCH_USE_KEVENT_WORKQUEUE
		if (!disable_kevent_wq && (*wq_supported & WORKQ_FEATURE_KEVENT)) {
			r = _pthread_workqueue_init_with_kevent(_dispatch_worker_thread3,
					(pthread_workqueue_function_kevent_t)
					_dispatch_kevent_worker_thread,
					offsetof(struct dispatch_queue_s, dq_serialnum), 0);
#if DISPATCH_USE_MGR_THREAD
			_dispatch_kevent_workqueue_enabled = !r;
#endif
#if DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK
			_dispatch_evfilt_machport_direct_enabled = !r;
#endif
			result = !r;
		} else
#endif
		if (*wq_supported & WORKQ_FEATURE_FINEPRIO) {
#if DISPATCH_USE_MGR_THREAD
			r = _pthread_workqueue_init(_dispatch_worker_thread3,
					offsetof(struct dispatch_queue_s, dq_serialnum), 0);
			result = !r;
#endif
		}
		if (result) _dispatch_root_queues_init_qos(*wq_supported);
	}
#endif // DISPATCH_USE_KEVENT_WORKQUEUE || HAVE_PTHREAD_WORKQUEUE_QOS
#if HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
	if (!result && !disable_wq) {
		pthread_workqueue_setdispatchoffset_np(
				offsetof(struct dispatch_queue_s, dq_serialnum));
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
			if (!disable_wq && qc->dgq_wq_priority != WORKQ_PRIO_INVALID) {
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
		uint8_t pool_size, bool overcommit)
{
	dispatch_pthread_root_queue_context_t pqc = qc->dgq_ctxt;
	uint32_t thread_pool_size = overcommit ? MAX_PTHREAD_COUNT :
			dispatch_hw_config(active_cpus);
	if (slowpath(pool_size) && pool_size < thread_pool_size) {
		thread_pool_size = pool_size;
	}
	qc->dgq_thread_pool_size = thread_pool_size;
#if HAVE_PTHREAD_WORKQUEUES
	if (qc->dgq_qos) {
		(void)dispatch_assume_zero(pthread_attr_init(&pqc->dpq_thread_attr));
		(void)dispatch_assume_zero(pthread_attr_setdetachstate(
				&pqc->dpq_thread_attr, PTHREAD_CREATE_DETACHED));
#if HAVE_PTHREAD_WORKQUEUE_QOS
		(void)dispatch_assume_zero(pthread_attr_set_qos_class_np(
				&pqc->dpq_thread_attr, qc->dgq_qos, 0));
#endif
	}
#endif // HAVE_PTHREAD_WORKQUEUES
#if USE_MACH_SEM
	// override the default FIFO behavior for the pool semaphores
	kern_return_t kr = semaphore_create(mach_task_self(),
			&pqc->dpq_thread_mediator.dsema_port, SYNC_POLICY_LIFO, 0);
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
	(void)dispatch_assume(pqc->dpq_thread_mediator.dsema_port);
#elif USE_POSIX_SEM
	/* XXXRW: POSIX semaphores don't support LIFO? */
	int ret = sem_init(&(pqc->dpq_thread_mediator.dsema_sem), 0, 0);
	(void)dispatch_assume_zero(ret);
#endif
}
#endif // DISPATCH_USE_PTHREAD_POOL

static dispatch_once_t _dispatch_root_queues_pred;

void
_dispatch_root_queues_init(void)
{
	dispatch_once_f(&_dispatch_root_queues_pred, NULL,
			_dispatch_root_queues_init_once);
}

static void
_dispatch_root_queues_init_once(void *context DISPATCH_UNUSED)
{
	int wq_supported;
	_dispatch_fork_becomes_unsafe();
	if (!_dispatch_root_queues_init_workq(&wq_supported)) {
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
					&_dispatch_root_queue_contexts[i], 0, overcommit);
		}
#else
		DISPATCH_INTERNAL_CRASH((errno << 16) | wq_supported,
				"Root queue initialization failed");
#endif // DISPATCH_ENABLE_THREAD_POOL
	}
}

DISPATCH_EXPORT DISPATCH_NOTHROW
void
libdispatch_init(void)
{
	dispatch_assert(DISPATCH_QUEUE_QOS_COUNT == 6);
	dispatch_assert(DISPATCH_ROOT_QUEUE_COUNT == 12);

	dispatch_assert(DISPATCH_QUEUE_PRIORITY_LOW ==
			-DISPATCH_QUEUE_PRIORITY_HIGH);
	dispatch_assert(countof(_dispatch_root_queues) ==
			DISPATCH_ROOT_QUEUE_COUNT);
	dispatch_assert(countof(_dispatch_root_queue_contexts) ==
			DISPATCH_ROOT_QUEUE_COUNT);
	dispatch_assert(countof(_dispatch_priority2qos) ==
			DISPATCH_PRIORITY_COUNT);
#if HAVE_PTHREAD_WORKQUEUE_QOS
	dispatch_assert(countof(_dispatch_priority2wq) ==
			DISPATCH_PRIORITY_COUNT);
#endif
#if HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
	dispatch_assert(sizeof(_dispatch_wq2root_queues) /
			sizeof(_dispatch_wq2root_queues[0][0]) ==
			WORKQ_NUM_PRIOQUEUE * 2);
#endif
#if DISPATCH_ENABLE_THREAD_POOL
	dispatch_assert(countof(_dispatch_pthread_root_queue_contexts) ==
			DISPATCH_ROOT_QUEUE_COUNT);
#endif

	dispatch_assert(offsetof(struct dispatch_continuation_s, do_next) ==
			offsetof(struct dispatch_object_s, do_next));
	dispatch_assert(offsetof(struct dispatch_continuation_s, do_vtable) ==
			offsetof(struct dispatch_object_s, do_vtable));
	dispatch_assert(sizeof(struct dispatch_apply_s) <=
			DISPATCH_CONTINUATION_SIZE);
	dispatch_assert(sizeof(struct dispatch_queue_s) % DISPATCH_CACHELINE_SIZE
			== 0);
	dispatch_assert(offsetof(struct dispatch_queue_s, dq_state) % _Alignof(uint64_t) == 0);
	dispatch_assert(sizeof(struct dispatch_root_queue_context_s) %
			DISPATCH_CACHELINE_SIZE == 0);


#if HAVE_PTHREAD_WORKQUEUE_QOS
	// 26497968 _dispatch_user_initiated_priority should be set for qos
	//          propagation to work properly
	pthread_priority_t p = _pthread_qos_class_encode(qos_class_main(), 0, 0);
	_dispatch_main_q.dq_priority = (dispatch_priority_t)p;
	_dispatch_main_q.dq_override = p & _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	p = _pthread_qos_class_encode(_DISPATCH_QOS_CLASS_USER_INITIATED, 0, 0);
	_dispatch_user_initiated_priority = p;
	p = _pthread_qos_class_encode(_DISPATCH_QOS_CLASS_BACKGROUND, 0, 0);
	_dispatch_background_priority = p;
#if DISPATCH_DEBUG
	if (!slowpath(getenv("LIBDISPATCH_DISABLE_SET_QOS"))) {
		_dispatch_set_qos_class_enabled = 1;
	}
#endif
#endif

#if DISPATCH_USE_THREAD_LOCAL_STORAGE
	_dispatch_thread_key_create(&__dispatch_tsd_key, _libdispatch_tsd_cleanup);
#else
	_dispatch_thread_key_create(&dispatch_queue_key, _dispatch_queue_cleanup);
	_dispatch_thread_key_create(&dispatch_deferred_items_key,
			_dispatch_deferred_items_cleanup);
	_dispatch_thread_key_create(&dispatch_frame_key, _dispatch_frame_cleanup);
	_dispatch_thread_key_create(&dispatch_voucher_key, _voucher_thread_cleanup);
	_dispatch_thread_key_create(&dispatch_cache_key, _dispatch_cache_cleanup);
	_dispatch_thread_key_create(&dispatch_context_key, _dispatch_context_cleanup);
	_dispatch_thread_key_create(&dispatch_defaultpriority_key, NULL);
	_dispatch_thread_key_create(&dispatch_pthread_root_queue_observer_hooks_key,
			NULL);
#if DISPATCH_PERF_MON && !DISPATCH_INTROSPECTION
	_dispatch_thread_key_create(&dispatch_bcounter_key, NULL);
#endif
#if DISPATCH_LOCK_USE_SEMAPHORE_FALLBACK
	if (DISPATCH_LOCK_USE_SEMAPHORE_FALLBACK) {
		_dispatch_thread_key_create(&dispatch_sema4_key,
				_dispatch_thread_semaphore_dispose);
	}
#endif
#endif

#if DISPATCH_USE_RESOLVERS // rdar://problem/8541707
	_dispatch_main_q.do_targetq = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS_OVERCOMMIT];
#endif

	_dispatch_queue_set_current(&_dispatch_main_q);
	_dispatch_queue_set_bound_thread(&_dispatch_main_q);

#if DISPATCH_USE_PTHREAD_ATFORK
	(void)dispatch_assume_zero(pthread_atfork(dispatch_atfork_prepare,
			dispatch_atfork_parent, dispatch_atfork_child));
#endif
	_dispatch_hw_config_init();
	_dispatch_vtable_init();
	_os_object_init();
	_voucher_init();
	_dispatch_introspection_init();
}

#if HAVE_MACH
static dispatch_once_t _dispatch_mach_host_port_pred;
static mach_port_t _dispatch_mach_host_port;

static void
_dispatch_mach_host_port_init(void *ctxt DISPATCH_UNUSED)
{
	kern_return_t kr;
	mach_port_t mp, mhp = mach_host_self();
	kr = host_get_host_port(mhp, &mp);
	DISPATCH_VERIFY_MIG(kr);
	if (fastpath(!kr)) {
		// mach_host_self returned the HOST_PRIV port
		kr = mach_port_deallocate(mach_task_self(), mhp);
		DISPATCH_VERIFY_MIG(kr);
		mhp = mp;
	} else if (kr != KERN_INVALID_ARGUMENT) {
		(void)dispatch_assume_zero(kr);
	}
	if (!fastpath(mhp)) {
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
#endif

#if DISPATCH_USE_THREAD_LOCAL_STORAGE
#include <unistd.h>
#include <sys/syscall.h>

#ifndef __ANDROID__
#ifdef SYS_gettid
DISPATCH_ALWAYS_INLINE
static inline pid_t
gettid(void)
{
	return (pid_t) syscall(SYS_gettid);
}
#else
#error "SYS_gettid unavailable on this system"
#endif /* SYS_gettid */
#endif /* ! __ANDROID__ */

#define _tsd_call_cleanup(k, f)  do { \
		if ((f) && tsd->k) ((void(*)(void*))(f))(tsd->k); \
    } while (0)

void
_libdispatch_tsd_cleanup(void *ctx)
{
	struct dispatch_tsd *tsd = (struct dispatch_tsd*) ctx;

	_tsd_call_cleanup(dispatch_queue_key, _dispatch_queue_cleanup);
	_tsd_call_cleanup(dispatch_frame_key, _dispatch_frame_cleanup);
	_tsd_call_cleanup(dispatch_cache_key, _dispatch_cache_cleanup);
	_tsd_call_cleanup(dispatch_context_key, _dispatch_context_cleanup);
	_tsd_call_cleanup(dispatch_pthread_root_queue_observer_hooks_key,
			NULL);
	_tsd_call_cleanup(dispatch_defaultpriority_key, NULL);
#if DISPATCH_PERF_MON && !DISPATCH_INTROSPECTION
	_tsd_call_cleanup(dispatch_bcounter_key, NULL);
#endif
#if DISPATCH_LOCK_USE_SEMAPHORE_FALLBACK
	_tsd_call_cleanup(dispatch_sema4_key, _dispatch_thread_semaphore_dispose);
#endif
	_tsd_call_cleanup(dispatch_priority_key, NULL);
	_tsd_call_cleanup(dispatch_voucher_key, _voucher_thread_cleanup);
	_tsd_call_cleanup(dispatch_deferred_items_key,
			_dispatch_deferred_items_cleanup);
	tsd->tid = 0;
}

DISPATCH_NOINLINE
void
libdispatch_tsd_init(void)
{
	pthread_setspecific(__dispatch_tsd_key, &__dispatch_tsd);
	__dispatch_tsd.tid = gettid();
}
#endif

DISPATCH_EXPORT DISPATCH_NOTHROW
void
dispatch_atfork_child(void)
{
	void *crash = (void *)0x100;
	size_t i;

#if HAVE_MACH
	_dispatch_mach_host_port_pred = 0;
	_dispatch_mach_host_port = MACH_VOUCHER_NULL;
#endif
	_voucher_atfork_child();
	if (!_dispatch_is_multithreaded_inline()) {
		// clear the _PROHIBIT bit if set
		_dispatch_unsafe_fork = 0;
		return;
	}
	_dispatch_unsafe_fork = 0;
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
#pragma mark dispatch_queue_attr_t

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_qos_class_valid(dispatch_qos_class_t qos_class, int relative_priority)
{
	qos_class_t qos = (qos_class_t)qos_class;
	switch (qos) {
	case _DISPATCH_QOS_CLASS_MAINTENANCE:
	case _DISPATCH_QOS_CLASS_BACKGROUND:
	case _DISPATCH_QOS_CLASS_UTILITY:
	case _DISPATCH_QOS_CLASS_DEFAULT:
	case _DISPATCH_QOS_CLASS_USER_INITIATED:
	case _DISPATCH_QOS_CLASS_USER_INTERACTIVE:
	case _DISPATCH_QOS_CLASS_UNSPECIFIED:
		break;
	default:
		return false;
	}
	if (relative_priority > 0 || relative_priority < QOS_MIN_RELATIVE_PRIORITY){
		return false;
	}
	return true;
}

#define DISPATCH_QUEUE_ATTR_QOS2IDX_INITIALIZER(qos) \
		[_DISPATCH_QOS_CLASS_##qos] = DQA_INDEX_QOS_CLASS_##qos

static const
_dispatch_queue_attr_index_qos_class_t _dispatch_queue_attr_qos2idx[] = {
	DISPATCH_QUEUE_ATTR_QOS2IDX_INITIALIZER(UNSPECIFIED),
	DISPATCH_QUEUE_ATTR_QOS2IDX_INITIALIZER(MAINTENANCE),
	DISPATCH_QUEUE_ATTR_QOS2IDX_INITIALIZER(BACKGROUND),
	DISPATCH_QUEUE_ATTR_QOS2IDX_INITIALIZER(UTILITY),
	DISPATCH_QUEUE_ATTR_QOS2IDX_INITIALIZER(DEFAULT),
	DISPATCH_QUEUE_ATTR_QOS2IDX_INITIALIZER(USER_INITIATED),
	DISPATCH_QUEUE_ATTR_QOS2IDX_INITIALIZER(USER_INTERACTIVE),
};

#define DISPATCH_QUEUE_ATTR_OVERCOMMIT2IDX(overcommit) \
		((overcommit) == _dispatch_queue_attr_overcommit_disabled ? \
		DQA_INDEX_NON_OVERCOMMIT : \
		((overcommit) == _dispatch_queue_attr_overcommit_enabled ? \
		DQA_INDEX_OVERCOMMIT : DQA_INDEX_UNSPECIFIED_OVERCOMMIT))

#define DISPATCH_QUEUE_ATTR_CONCURRENT2IDX(concurrent) \
		((concurrent) ? DQA_INDEX_CONCURRENT : DQA_INDEX_SERIAL)

#define DISPATCH_QUEUE_ATTR_INACTIVE2IDX(inactive) \
		((inactive) ? DQA_INDEX_INACTIVE : DQA_INDEX_ACTIVE)

#define DISPATCH_QUEUE_ATTR_AUTORELEASE_FREQUENCY2IDX(frequency) \
		(frequency)

#define DISPATCH_QUEUE_ATTR_PRIO2IDX(prio) (-(prio))

#define DISPATCH_QUEUE_ATTR_QOS2IDX(qos) (_dispatch_queue_attr_qos2idx[(qos)])

static inline dispatch_queue_attr_t
_dispatch_get_queue_attr(qos_class_t qos, int prio,
		_dispatch_queue_attr_overcommit_t overcommit,
		dispatch_autorelease_frequency_t frequency,
		bool concurrent, bool inactive)
{
	return (dispatch_queue_attr_t)&_dispatch_queue_attrs
			[DISPATCH_QUEUE_ATTR_QOS2IDX(qos)]
			[DISPATCH_QUEUE_ATTR_PRIO2IDX(prio)]
			[DISPATCH_QUEUE_ATTR_OVERCOMMIT2IDX(overcommit)]
			[DISPATCH_QUEUE_ATTR_AUTORELEASE_FREQUENCY2IDX(frequency)]
			[DISPATCH_QUEUE_ATTR_CONCURRENT2IDX(concurrent)]
			[DISPATCH_QUEUE_ATTR_INACTIVE2IDX(inactive)];
}

dispatch_queue_attr_t
_dispatch_get_default_queue_attr(void)
{
	return _dispatch_get_queue_attr(_DISPATCH_QOS_CLASS_UNSPECIFIED, 0,
				_dispatch_queue_attr_overcommit_unspecified,
				DISPATCH_AUTORELEASE_FREQUENCY_INHERIT, false, false);
}

dispatch_queue_attr_t
dispatch_queue_attr_make_with_qos_class(dispatch_queue_attr_t dqa,
		dispatch_qos_class_t qos_class, int relative_priority)
{
	if (!_dispatch_qos_class_valid(qos_class, relative_priority)) {
		return DISPATCH_BAD_INPUT;
	}
	if (!slowpath(dqa)) {
		dqa = _dispatch_get_default_queue_attr();
	} else if (dqa->do_vtable != DISPATCH_VTABLE(queue_attr)) {
		DISPATCH_CLIENT_CRASH(dqa->do_vtable, "Invalid queue attribute");
	}
	return _dispatch_get_queue_attr(qos_class, relative_priority,
			dqa->dqa_overcommit, dqa->dqa_autorelease_frequency,
			dqa->dqa_concurrent, dqa->dqa_inactive);
}

dispatch_queue_attr_t
dispatch_queue_attr_make_initially_inactive(dispatch_queue_attr_t dqa)
{
	if (!slowpath(dqa)) {
		dqa = _dispatch_get_default_queue_attr();
	} else if (dqa->do_vtable != DISPATCH_VTABLE(queue_attr)) {
		DISPATCH_CLIENT_CRASH(dqa->do_vtable, "Invalid queue attribute");
	}
	return _dispatch_get_queue_attr(dqa->dqa_qos_class,
			dqa->dqa_relative_priority, dqa->dqa_overcommit,
			dqa->dqa_autorelease_frequency, dqa->dqa_concurrent, true);
}

dispatch_queue_attr_t
dispatch_queue_attr_make_with_overcommit(dispatch_queue_attr_t dqa,
		bool overcommit)
{
	if (!slowpath(dqa)) {
		dqa = _dispatch_get_default_queue_attr();
	} else if (dqa->do_vtable != DISPATCH_VTABLE(queue_attr)) {
		DISPATCH_CLIENT_CRASH(dqa->do_vtable, "Invalid queue attribute");
	}
	return _dispatch_get_queue_attr(dqa->dqa_qos_class,
			dqa->dqa_relative_priority, overcommit ?
			_dispatch_queue_attr_overcommit_enabled :
			_dispatch_queue_attr_overcommit_disabled,
			dqa->dqa_autorelease_frequency, dqa->dqa_concurrent,
			dqa->dqa_inactive);
}

dispatch_queue_attr_t
dispatch_queue_attr_make_with_autorelease_frequency(dispatch_queue_attr_t dqa,
		dispatch_autorelease_frequency_t frequency)
{
	switch (frequency) {
	case DISPATCH_AUTORELEASE_FREQUENCY_INHERIT:
	case DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM:
	case DISPATCH_AUTORELEASE_FREQUENCY_NEVER:
		break;
	default:
		return DISPATCH_BAD_INPUT;
	}
	if (!slowpath(dqa)) {
		dqa = _dispatch_get_default_queue_attr();
	} else if (dqa->do_vtable != DISPATCH_VTABLE(queue_attr)) {
		DISPATCH_CLIENT_CRASH(dqa->do_vtable, "Invalid queue attribute");
	}
	return _dispatch_get_queue_attr(dqa->dqa_qos_class,
			dqa->dqa_relative_priority, dqa->dqa_overcommit,
			frequency, dqa->dqa_concurrent, dqa->dqa_inactive);
}

#pragma mark -
#pragma mark dispatch_queue_t

// skip zero
// 1 - main_q
// 2 - mgr_q
// 3 - mgr_root_q
// 4,5,6,7,8,9,10,11,12,13,14,15 - global queues
// we use 'xadd' on Intel, so the initial value == next assigned
unsigned long volatile _dispatch_queue_serial_numbers = 16;

DISPATCH_NOINLINE
static dispatch_queue_t
_dispatch_queue_create_with_target(const char *label, dispatch_queue_attr_t dqa,
		dispatch_queue_t tq, bool legacy)
{
#if DISPATCH_USE_NOQOS_WORKQUEUE_FALLBACK
	// Be sure the root queue priorities are set
	dispatch_once_f(&_dispatch_root_queues_pred, NULL,
			_dispatch_root_queues_init_once);
#endif
	if (!slowpath(dqa)) {
		dqa = _dispatch_get_default_queue_attr();
	} else if (dqa->do_vtable != DISPATCH_VTABLE(queue_attr)) {
		DISPATCH_CLIENT_CRASH(dqa->do_vtable, "Invalid queue attribute");
	}

	//
	// Step 1: Normalize arguments (qos, overcommit, tq)
	//

	qos_class_t qos = dqa->dqa_qos_class;
#if DISPATCH_USE_NOQOS_WORKQUEUE_FALLBACK
	if (qos == _DISPATCH_QOS_CLASS_USER_INTERACTIVE &&
			!_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS].dq_priority) {
		qos = _DISPATCH_QOS_CLASS_USER_INITIATED;
	}
#endif
	bool maintenance_fallback = false;
#if DISPATCH_USE_NOQOS_WORKQUEUE_FALLBACK
	maintenance_fallback = true;
#endif // DISPATCH_USE_NOQOS_WORKQUEUE_FALLBACK
	if (maintenance_fallback) {
		if (qos == _DISPATCH_QOS_CLASS_MAINTENANCE &&
				!_dispatch_root_queues[
				DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS].dq_priority) {
			qos = _DISPATCH_QOS_CLASS_BACKGROUND;
		}
	}

	_dispatch_queue_attr_overcommit_t overcommit = dqa->dqa_overcommit;
	if (overcommit != _dispatch_queue_attr_overcommit_unspecified && tq) {
		if (tq->do_targetq) {
			DISPATCH_CLIENT_CRASH(tq, "Cannot specify both overcommit and "
					"a non-global target queue");
		}
	}

	if (tq && !tq->do_targetq &&
			tq->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT) {
		// Handle discrepancies between attr and target queue, attributes win
		if (overcommit == _dispatch_queue_attr_overcommit_unspecified) {
			if (tq->dq_priority & _PTHREAD_PRIORITY_OVERCOMMIT_FLAG) {
				overcommit = _dispatch_queue_attr_overcommit_enabled;
			} else {
				overcommit = _dispatch_queue_attr_overcommit_disabled;
			}
		}
		if (qos == _DISPATCH_QOS_CLASS_UNSPECIFIED) {
			tq = _dispatch_get_root_queue_with_overcommit(tq,
					overcommit == _dispatch_queue_attr_overcommit_enabled);
		} else {
			tq = NULL;
		}
	} else if (tq && !tq->do_targetq) {
		// target is a pthread or runloop root queue, setting QoS or overcommit
		// is disallowed
		if (overcommit != _dispatch_queue_attr_overcommit_unspecified) {
			DISPATCH_CLIENT_CRASH(tq, "Cannot specify an overcommit attribute "
					"and use this kind of target queue");
		}
		if (qos != _DISPATCH_QOS_CLASS_UNSPECIFIED) {
			DISPATCH_CLIENT_CRASH(tq, "Cannot specify a QoS attribute "
					"and use this kind of target queue");
		}
	} else {
		if (overcommit == _dispatch_queue_attr_overcommit_unspecified) {
			 // Serial queues default to overcommit!
			overcommit = dqa->dqa_concurrent ?
					_dispatch_queue_attr_overcommit_disabled :
					_dispatch_queue_attr_overcommit_enabled;
		}
	}
	if (!tq) {
		qos_class_t tq_qos = qos == _DISPATCH_QOS_CLASS_UNSPECIFIED ?
				_DISPATCH_QOS_CLASS_DEFAULT : qos;
		tq = _dispatch_get_root_queue(tq_qos, overcommit ==
				_dispatch_queue_attr_overcommit_enabled);
		if (slowpath(!tq)) {
			DISPATCH_CLIENT_CRASH(qos, "Invalid queue attribute");
		}
	}

	//
	// Step 2: Initialize the queue
	//

	if (legacy) {
		// if any of these attributes is specified, use non legacy classes
		if (dqa->dqa_inactive || dqa->dqa_autorelease_frequency) {
			legacy = false;
		}
	}

	const void *vtable;
	dispatch_queue_flags_t dqf = 0;
	if (legacy) {
		vtable = DISPATCH_VTABLE(queue);
	} else if (dqa->dqa_concurrent) {
		vtable = DISPATCH_VTABLE(queue_concurrent);
	} else {
		vtable = DISPATCH_VTABLE(queue_serial);
	}
	switch (dqa->dqa_autorelease_frequency) {
	case DISPATCH_AUTORELEASE_FREQUENCY_NEVER:
		dqf |= DQF_AUTORELEASE_NEVER;
		break;
	case DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM:
		dqf |= DQF_AUTORELEASE_ALWAYS;
		break;
	}
	if (label) {
		const char *tmp = _dispatch_strdup_if_mutable(label);
		if (tmp != label) {
			dqf |= DQF_LABEL_NEEDS_FREE;
			label = tmp;
		}
	}

	dispatch_queue_t dq = _dispatch_alloc(vtable,
			sizeof(struct dispatch_queue_s) - DISPATCH_QUEUE_CACHELINE_PAD);
	_dispatch_queue_init(dq, dqf, dqa->dqa_concurrent ?
			DISPATCH_QUEUE_WIDTH_MAX : 1, dqa->dqa_inactive);

	dq->dq_label = label;

#if HAVE_PTHREAD_WORKQUEUE_QOS
	dq->dq_priority = (dispatch_priority_t)_pthread_qos_class_encode(qos,
			dqa->dqa_relative_priority,
			overcommit == _dispatch_queue_attr_overcommit_enabled ?
			_PTHREAD_PRIORITY_OVERCOMMIT_FLAG : 0);
#endif
	_dispatch_retain(tq);
	if (qos == _DISPATCH_QOS_CLASS_UNSPECIFIED) {
		// legacy way of inherithing the QoS from the target
		_dispatch_queue_priority_inherit_from_target(dq, tq);
	}
	if (!dqa->dqa_inactive) {
		_dispatch_queue_atomic_flags_set(tq, DQF_TARGETED);
	}
	dq->do_targetq = tq;
	_dispatch_object_debug(dq, "%s", __func__);
	return _dispatch_introspection_queue_create(dq);
}

dispatch_queue_t
dispatch_queue_create_with_target(const char *label, dispatch_queue_attr_t dqa,
		dispatch_queue_t tq)
{
	return _dispatch_queue_create_with_target(label, dqa, tq, false);
}

dispatch_queue_t
dispatch_queue_create(const char *label, dispatch_queue_attr_t attr)
{
	return _dispatch_queue_create_with_target(label, attr,
			DISPATCH_TARGET_QUEUE_DEFAULT, true);
}

dispatch_queue_t
dispatch_queue_create_with_accounting_override_voucher(const char *label,
		dispatch_queue_attr_t attr, voucher_t voucher)
{
	dispatch_queue_t dq = dispatch_queue_create_with_target(label, attr,
			DISPATCH_TARGET_QUEUE_DEFAULT);
	dq->dq_override_voucher = _voucher_create_accounting_voucher(voucher);
	return dq;
}

void
_dispatch_queue_destroy(dispatch_queue_t dq)
{
	uint64_t dq_state = os_atomic_load2o(dq, dq_state, relaxed);
	uint64_t initial_state = DISPATCH_QUEUE_STATE_INIT_VALUE(dq->dq_width);

	if (dx_type(dq) == DISPATCH_QUEUE_GLOBAL_ROOT_TYPE) {
		initial_state = DISPATCH_ROOT_QUEUE_STATE_INIT_VALUE;
	}
	if (dx_type(dq) == DISPATCH_SOURCE_KEVENT_TYPE) {
		// dispatch_cancel_and_wait may apply overrides in a racy way with
		// the source cancellation finishing. This race is expensive and not
		// really worthwhile to resolve since the source becomes dead anyway.
		dq_state &= ~DISPATCH_QUEUE_HAS_OVERRIDE;
	}
	if (slowpath(dq_state != initial_state)) {
		if (_dq_state_drain_locked(dq_state)) {
			DISPATCH_CLIENT_CRASH(dq, "Release of a locked queue");
		}
#ifndef __LP64__
		dq_state >>= 32;
#endif
		DISPATCH_CLIENT_CRASH((uintptr_t)dq_state,
				"Release of a queue with corrupt state");
	}
	if (slowpath(dq == _dispatch_queue_get_current())) {
		DISPATCH_CLIENT_CRASH(dq, "Release of a queue by itself");
	}
	if (slowpath(dq->dq_items_tail)) {
		DISPATCH_CLIENT_CRASH(dq->dq_items_tail,
				"Release of a queue while items are enqueued");
	}

	// trash the queue so that use after free will crash
	dq->dq_items_head = (void *)0x200;
	dq->dq_items_tail = (void *)0x200;
	// poison the state with something that is suspended and is easy to spot
	dq->dq_state = 0xdead000000000000;

	dispatch_queue_t dqsq = os_atomic_xchg2o(dq, dq_specific_q,
			(void *)0x200, relaxed);
	if (dqsq) {
		_dispatch_release(dqsq);
	}
	if (dq->dq_override_voucher != DISPATCH_NO_VOUCHER) {
		if (dq->dq_override_voucher) _voucher_release(dq->dq_override_voucher);
		dq->dq_override_voucher = DISPATCH_NO_VOUCHER;
	}
}

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
void
_dispatch_queue_dispose(dispatch_queue_t dq)
{
	_dispatch_object_debug(dq, "%s", __func__);
	_dispatch_introspection_queue_dispose(dq);
	if (dq->dq_label && _dispatch_queue_label_needs_free(dq)) {
		free((void*)dq->dq_label);
	}
	_dispatch_queue_destroy(dq);
}

DISPATCH_NOINLINE
static void
_dispatch_queue_suspend_slow(dispatch_queue_t dq)
{
	uint64_t dq_state, value, delta;

	_dispatch_queue_sidelock_lock(dq);

	// what we want to transfer (remove from dq_state)
	delta  = DISPATCH_QUEUE_SUSPEND_HALF * DISPATCH_QUEUE_SUSPEND_INTERVAL;
	// but this is a suspend so add a suspend count at the same time
	delta -= DISPATCH_QUEUE_SUSPEND_INTERVAL;
	if (dq->dq_side_suspend_cnt == 0) {
		// we substract delta from dq_state, and we want to set this bit
		delta -= DISPATCH_QUEUE_HAS_SIDE_SUSPEND_CNT;
	}

	os_atomic_rmw_loop2o(dq, dq_state, dq_state, value, relaxed, {
		// unsigned underflow of the substraction can happen because other
		// threads could have touched this value while we were trying to acquire
		// the lock, or because another thread raced us to do the same operation
		// and got to the lock first.
		if (slowpath(os_sub_overflow(dq_state, delta, &value))) {
			os_atomic_rmw_loop_give_up(goto retry);
		}
	});
	if (slowpath(os_add_overflow(dq->dq_side_suspend_cnt,
			DISPATCH_QUEUE_SUSPEND_HALF, &dq->dq_side_suspend_cnt))) {
		DISPATCH_CLIENT_CRASH(0, "Too many nested calls to dispatch_suspend()");
	}
	return _dispatch_queue_sidelock_unlock(dq);

retry:
	_dispatch_queue_sidelock_unlock(dq);
	return dx_vtable(dq)->do_suspend(dq);
}

void
_dispatch_queue_suspend(dispatch_queue_t dq)
{
	dispatch_assert(dq->do_ref_cnt != DISPATCH_OBJECT_GLOBAL_REFCNT);

	uint64_t dq_state, value;

	os_atomic_rmw_loop2o(dq, dq_state, dq_state, value, relaxed, {
		value = DISPATCH_QUEUE_SUSPEND_INTERVAL;
		if (slowpath(os_add_overflow(dq_state, value, &value))) {
			os_atomic_rmw_loop_give_up({
				return _dispatch_queue_suspend_slow(dq);
			});
		}
	});

	if (!_dq_state_is_suspended(dq_state)) {
		// rdar://8181908 we need to extend the queue life for the duration
		// of the call to wakeup at _dispatch_queue_resume() time.
		_dispatch_retain(dq);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_queue_resume_slow(dispatch_queue_t dq)
{
	uint64_t dq_state, value, delta;

	_dispatch_queue_sidelock_lock(dq);

	// what we want to transfer
	delta  = DISPATCH_QUEUE_SUSPEND_HALF * DISPATCH_QUEUE_SUSPEND_INTERVAL;
	// but this is a resume so consume a suspend count at the same time
	delta -= DISPATCH_QUEUE_SUSPEND_INTERVAL;
	switch (dq->dq_side_suspend_cnt) {
	case 0:
		goto retry;
	case DISPATCH_QUEUE_SUSPEND_HALF:
		// we will transition the side count to 0, so we want to clear this bit
		delta -= DISPATCH_QUEUE_HAS_SIDE_SUSPEND_CNT;
		break;
	}
	os_atomic_rmw_loop2o(dq, dq_state, dq_state, value, relaxed, {
		// unsigned overflow of the addition can happen because other
		// threads could have touched this value while we were trying to acquire
		// the lock, or because another thread raced us to do the same operation
		// and got to the lock first.
		if (slowpath(os_add_overflow(dq_state, delta, &value))) {
			os_atomic_rmw_loop_give_up(goto retry);
		}
	});
	dq->dq_side_suspend_cnt -= DISPATCH_QUEUE_SUSPEND_HALF;
	return _dispatch_queue_sidelock_unlock(dq);

retry:
	_dispatch_queue_sidelock_unlock(dq);
	return dx_vtable(dq)->do_resume(dq, false);
}

DISPATCH_NOINLINE
static void
_dispatch_queue_resume_finalize_activation(dispatch_queue_t dq)
{
	// Step 2: run the activation finalizer
	if (dx_vtable(dq)->do_finalize_activation) {
		dx_vtable(dq)->do_finalize_activation(dq);
	}
	// Step 3: consume the suspend count
	return dx_vtable(dq)->do_resume(dq, false);
}

void
_dispatch_queue_resume(dispatch_queue_t dq, bool activate)
{
	// covers all suspend and inactive bits, including side suspend bit
	const uint64_t suspend_bits = DISPATCH_QUEUE_SUSPEND_BITS_MASK;
	// backward compatibility: only dispatch sources can abuse
	// dispatch_resume() to really mean dispatch_activate()
	bool resume_can_activate = (dx_type(dq) == DISPATCH_SOURCE_KEVENT_TYPE);
	uint64_t dq_state, value;

	dispatch_assert(dq->do_ref_cnt != DISPATCH_OBJECT_GLOBAL_REFCNT);

	// Activation is a bit tricky as it needs to finalize before the wakeup.
	//
	// If after doing its updates to the suspend count and/or inactive bit,
	// the last suspension related bit that would remain is the
	// NEEDS_ACTIVATION one, then this function:
	//
	// 1. moves the state to { sc:1 i:0 na:0 } (converts the needs-activate into
	//    a suspend count)
	// 2. runs the activation finalizer
	// 3. consumes the suspend count set in (1), and finishes the resume flow
	//
	// Concurrently, some property setters such as setting dispatch source
	// handlers or _dispatch_queue_set_target_queue try to do in-place changes
	// before activation. These protect their action by taking a suspend count.
	// Step (1) above cannot happen if such a setter has locked the object.
	if (activate) {
		// relaxed atomic because this doesn't publish anything, this is only
		// about picking the thread that gets to finalize the activation
		os_atomic_rmw_loop2o(dq, dq_state, dq_state, value, relaxed, {
			if ((dq_state & suspend_bits) ==
					DISPATCH_QUEUE_NEEDS_ACTIVATION + DISPATCH_QUEUE_INACTIVE) {
				// { sc:0 i:1 na:1 } -> { sc:1 i:0 na:0 }
				value = dq_state - DISPATCH_QUEUE_INACTIVE
						- DISPATCH_QUEUE_NEEDS_ACTIVATION
						+ DISPATCH_QUEUE_SUSPEND_INTERVAL;
			} else if (_dq_state_is_inactive(dq_state)) {
				// { sc:>0 i:1 na:1 } -> { i:0 na:1 }
				// simple activation because sc is not 0
				// resume will deal with na:1 later
				value = dq_state - DISPATCH_QUEUE_INACTIVE;
			} else {
				// object already active, this is a no-op, just exit
				os_atomic_rmw_loop_give_up(return);
			}
		});
	} else {
		// release barrier needed to publish the effect of
		// - dispatch_set_target_queue()
		// - dispatch_set_*_handler()
		// - do_finalize_activation()
		os_atomic_rmw_loop2o(dq, dq_state, dq_state, value, release, {
			if ((dq_state & suspend_bits) == DISPATCH_QUEUE_SUSPEND_INTERVAL
					+ DISPATCH_QUEUE_NEEDS_ACTIVATION) {
				// { sc:1 i:0 na:1 } -> { sc:1 i:0 na:0 }
				value = dq_state - DISPATCH_QUEUE_NEEDS_ACTIVATION;
			} else if (resume_can_activate && (dq_state & suspend_bits) ==
					DISPATCH_QUEUE_NEEDS_ACTIVATION + DISPATCH_QUEUE_INACTIVE) {
				// { sc:0 i:1 na:1 } -> { sc:1 i:0 na:0 }
				value = dq_state - DISPATCH_QUEUE_INACTIVE
						- DISPATCH_QUEUE_NEEDS_ACTIVATION
						+ DISPATCH_QUEUE_SUSPEND_INTERVAL;
			} else {
				value = DISPATCH_QUEUE_SUSPEND_INTERVAL;
				if (slowpath(os_sub_overflow(dq_state, value, &value))) {
					// underflow means over-resume or a suspend count transfer
					// to the side count is needed
					os_atomic_rmw_loop_give_up({
						if (!(dq_state & DISPATCH_QUEUE_HAS_SIDE_SUSPEND_CNT)) {
							goto over_resume;
						}
						return _dispatch_queue_resume_slow(dq);
					});
				}
				if (_dq_state_is_runnable(value) &&
						!_dq_state_drain_locked(value)) {
					uint64_t full_width = value;
					if (_dq_state_has_pending_barrier(value)) {
						full_width -= DISPATCH_QUEUE_PENDING_BARRIER;
						full_width += DISPATCH_QUEUE_WIDTH_INTERVAL;
						full_width += DISPATCH_QUEUE_IN_BARRIER;
					} else {
						full_width += dq->dq_width * DISPATCH_QUEUE_WIDTH_INTERVAL;
						full_width += DISPATCH_QUEUE_IN_BARRIER;
					}
					if ((full_width & DISPATCH_QUEUE_WIDTH_MASK) ==
							DISPATCH_QUEUE_WIDTH_FULL_BIT) {
						value = full_width;
						value &= ~DISPATCH_QUEUE_DIRTY;
						value |= _dispatch_tid_self();
					}
				}
			}
		});
	}

	if ((dq_state ^ value) & DISPATCH_QUEUE_NEEDS_ACTIVATION) {
		// we cleared the NEEDS_ACTIVATION bit and we have a valid suspend count
		return _dispatch_queue_resume_finalize_activation(dq);
	}

	if (activate) {
		// if we're still in an activate codepath here we should have
		// { sc:>0 na:1 }, if not we've got a corrupt state
		if (!fastpath(_dq_state_is_suspended(value))) {
			DISPATCH_CLIENT_CRASH(dq, "Invalid suspension state");
		}
		return;
	}

	if (_dq_state_is_suspended(value)) {
		return;
	}

	if ((dq_state ^ value) & DISPATCH_QUEUE_IN_BARRIER) {
		_dispatch_release(dq);
		return _dispatch_try_lock_transfer_or_wakeup(dq);
	}

	if (_dq_state_should_wakeup(value)) {
		// <rdar://problem/14637483>
		// seq_cst wrt state changes that were flushed and not acted upon
		os_atomic_thread_fence(acquire);
		pthread_priority_t pp = _dispatch_queue_reset_override_priority(dq,
				_dispatch_queue_is_thread_bound(dq));
		return dx_wakeup(dq, pp, DISPATCH_WAKEUP_CONSUME);
	}
	return _dispatch_release_tailcall(dq);

over_resume:
	if (slowpath(_dq_state_is_inactive(dq_state))) {
		DISPATCH_CLIENT_CRASH(dq, "Over-resume of an inactive object");
	}
	DISPATCH_CLIENT_CRASH(dq, "Over-resume of an object");
}

const char *
dispatch_queue_get_label(dispatch_queue_t dq)
{
	if (slowpath(dq == DISPATCH_CURRENT_QUEUE_LABEL)) {
		dq = _dispatch_get_current_queue();
	}
	return dq->dq_label ? dq->dq_label : "";
}

qos_class_t
dispatch_queue_get_qos_class(dispatch_queue_t dq, int *relative_priority_ptr)
{
	qos_class_t qos = _DISPATCH_QOS_CLASS_UNSPECIFIED;
	int relative_priority = 0;
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pthread_priority_t dqp = dq->dq_priority;
	if (dqp & _PTHREAD_PRIORITY_INHERIT_FLAG) dqp = 0;
	qos = _pthread_qos_class_decode(dqp, &relative_priority, NULL);
#else
	(void)dq;
#endif
	if (relative_priority_ptr) *relative_priority_ptr = relative_priority;
	return qos;
}

static void
_dispatch_queue_set_width2(void *ctxt)
{
	int w = (int)(intptr_t)ctxt; // intentional truncation
	uint32_t tmp;
	dispatch_queue_t dq = _dispatch_queue_get_current();

	if (w > 0) {
		tmp = (unsigned int)w;
	} else switch (w) {
	case 0:
		tmp = 1;
		break;
	case DISPATCH_QUEUE_WIDTH_MAX_PHYSICAL_CPUS:
		tmp = dispatch_hw_config(physical_cpus);
		break;
	case DISPATCH_QUEUE_WIDTH_ACTIVE_CPUS:
		tmp = dispatch_hw_config(active_cpus);
		break;
	default:
		// fall through
	case DISPATCH_QUEUE_WIDTH_MAX_LOGICAL_CPUS:
		tmp = dispatch_hw_config(logical_cpus);
		break;
	}
	if (tmp > DISPATCH_QUEUE_WIDTH_MAX) {
		tmp = DISPATCH_QUEUE_WIDTH_MAX;
	}

	dispatch_queue_flags_t old_dqf, new_dqf;
	os_atomic_rmw_loop2o(dq, dq_atomic_flags, old_dqf, new_dqf, relaxed, {
		new_dqf = old_dqf & ~DQF_WIDTH_MASK;
		new_dqf |= (tmp << DQF_WIDTH_SHIFT);
	});
	_dispatch_object_debug(dq, "%s", __func__);
}

void
dispatch_queue_set_width(dispatch_queue_t dq, long width)
{
	if (slowpath(dq->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT) ||
			slowpath(dx_hastypeflag(dq, QUEUE_ROOT))) {
		return;
	}

	unsigned long type = dx_type(dq);
	switch (type) {
	case DISPATCH_QUEUE_LEGACY_TYPE:
	case DISPATCH_QUEUE_CONCURRENT_TYPE:
		break;
	case DISPATCH_QUEUE_SERIAL_TYPE:
		DISPATCH_CLIENT_CRASH(type, "Cannot set width of a serial queue");
	default:
		DISPATCH_CLIENT_CRASH(type, "Unexpected dispatch object type");
	}

	_dispatch_barrier_trysync_or_async_f(dq, (void*)(intptr_t)width,
			_dispatch_queue_set_width2);
}

static void
_dispatch_queue_legacy_set_target_queue(void *ctxt)
{
	dispatch_queue_t dq = _dispatch_queue_get_current();
	dispatch_queue_t tq = ctxt;
	dispatch_queue_t otq = dq->do_targetq;

	if (_dispatch_queue_atomic_flags(dq) & DQF_TARGETED) {
		_dispatch_ktrace3(DISPATCH_PERF_non_leaf_retarget, dq, otq, tq);
		_dispatch_bug_deprecated("Changing the target of a queue "
				"already targeted by other dispatch objects");
	}

	_dispatch_queue_priority_inherit_from_target(dq, tq);
	_dispatch_queue_atomic_flags_set(tq, DQF_TARGETED);
#if HAVE_PTHREAD_WORKQUEUE_QOS
	// see _dispatch_queue_class_wakeup()
	_dispatch_queue_sidelock_lock(dq);
#endif
	dq->do_targetq = tq;
#if HAVE_PTHREAD_WORKQUEUE_QOS
	// see _dispatch_queue_class_wakeup()
	_dispatch_queue_sidelock_unlock(dq);
#endif

	_dispatch_object_debug(dq, "%s", __func__);
	_dispatch_introspection_target_queue_changed(dq);
	_dispatch_release_tailcall(otq);
}

void
_dispatch_queue_set_target_queue(dispatch_queue_t dq, dispatch_queue_t tq)
{
	dispatch_assert(dq->do_ref_cnt != DISPATCH_OBJECT_GLOBAL_REFCNT &&
			dq->do_targetq);

	if (slowpath(!tq)) {
		bool is_concurrent_q = (dq->dq_width > 1);
		tq = _dispatch_get_root_queue(_DISPATCH_QOS_CLASS_DEFAULT,
				!is_concurrent_q);
	}

	if (_dispatch_queue_try_inactive_suspend(dq)) {
		_dispatch_object_set_target_queue_inline(dq, tq);
		return dx_vtable(dq)->do_resume(dq, false);
	}

	if (dq->dq_override_voucher != DISPATCH_NO_VOUCHER) {
		DISPATCH_CLIENT_CRASH(dq, "Cannot change the target of a queue or "
				"source with an accounting override voucher "
				"after it has been activated");
	}

	unsigned long type = dx_type(dq);
	switch (type) {
	case DISPATCH_QUEUE_LEGACY_TYPE:
		if (_dispatch_queue_atomic_flags(dq) & DQF_TARGETED) {
			_dispatch_bug_deprecated("Changing the target of a queue "
					"already targeted by other dispatch objects");
		}
		break;
	case DISPATCH_SOURCE_KEVENT_TYPE:
	case DISPATCH_MACH_CHANNEL_TYPE:
		_dispatch_ktrace1(DISPATCH_PERF_post_activate_retarget, dq);
		_dispatch_bug_deprecated("Changing the target of a source "
				"after it has been activated");
		break;

	case DISPATCH_QUEUE_SERIAL_TYPE:
	case DISPATCH_QUEUE_CONCURRENT_TYPE:
		DISPATCH_CLIENT_CRASH(type, "Cannot change the target of this queue "
				"after it has been activated");
	default:
		DISPATCH_CLIENT_CRASH(type, "Unexpected dispatch object type");
	}

	_dispatch_retain(tq);
	return _dispatch_barrier_trysync_or_async_f(dq, tq,
			_dispatch_queue_legacy_set_target_queue);
}

#pragma mark -
#pragma mark dispatch_mgr_queue

#if DISPATCH_USE_MGR_THREAD && DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES
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
	DISPATCH_GLOBAL_OBJECT_HEADER(queue_root),
	.dq_state = DISPATCH_ROOT_QUEUE_STATE_INIT_VALUE,
	.do_ctxt = &_dispatch_mgr_root_queue_context,
	.dq_label = "com.apple.root.libdispatch-manager",
	.dq_width = DISPATCH_QUEUE_WIDTH_POOL,
	.dq_override = DISPATCH_SATURATED_OVERRIDE,
	.dq_override_voucher = DISPATCH_NO_VOUCHER,
	.dq_serialnum = 3,
};
#endif // DISPATCH_USE_MGR_THREAD && DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES

#if DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES || DISPATCH_USE_KEVENT_WORKQUEUE
static struct {
	volatile int prio;
	volatile qos_class_t qos;
	int default_prio;
	int policy;
	pthread_t tid;
} _dispatch_mgr_sched;

static dispatch_once_t _dispatch_mgr_sched_pred;

// TODO: switch to "event-reflector thread" property <rdar://problem/18126138>

#if HAVE_PTHREAD_WORKQUEUE_QOS
// Must be kept in sync with list of qos classes in sys/qos.h
static const int _dispatch_mgr_sched_qos2prio[] = {
	[_DISPATCH_QOS_CLASS_MAINTENANCE] = 4,
	[_DISPATCH_QOS_CLASS_BACKGROUND] = 4,
	[_DISPATCH_QOS_CLASS_UTILITY] = 20,
	[_DISPATCH_QOS_CLASS_DEFAULT] = 31,
	[_DISPATCH_QOS_CLASS_USER_INITIATED] = 37,
	[_DISPATCH_QOS_CLASS_USER_INTERACTIVE] = 47,
};
#endif // HAVE_PTHREAD_WORKQUEUE_QOS

static void
_dispatch_mgr_sched_init(void *ctxt DISPATCH_UNUSED)
{
	struct sched_param param;
#if DISPATCH_USE_MGR_THREAD && DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES
	pthread_attr_t *attr;
	attr = &_dispatch_mgr_root_queue_pthread_context.dpq_thread_attr;
#else
	pthread_attr_t a, *attr = &a;
#endif
	(void)dispatch_assume_zero(pthread_attr_init(attr));
	(void)dispatch_assume_zero(pthread_attr_getschedpolicy(attr,
			&_dispatch_mgr_sched.policy));
	(void)dispatch_assume_zero(pthread_attr_getschedparam(attr, &param));
#if HAVE_PTHREAD_WORKQUEUE_QOS
	qos_class_t qos = qos_class_main();
	if (qos == _DISPATCH_QOS_CLASS_DEFAULT) {
		qos = _DISPATCH_QOS_CLASS_USER_INITIATED; // rdar://problem/17279292
	}
	if (qos) {
		_dispatch_mgr_sched.qos = qos;
		param.sched_priority = _dispatch_mgr_sched_qos2prio[qos];
	}
#endif
	_dispatch_mgr_sched.default_prio = param.sched_priority;
	_dispatch_mgr_sched.prio = _dispatch_mgr_sched.default_prio;
}
#endif // DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES || DISPATCH_USE_KEVENT_WORKQUEUE

#if DISPATCH_USE_MGR_THREAD && DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES
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
#if HAVE_PTHREAD_WORKQUEUE_QOS
	qos_class_t qos = _dispatch_mgr_sched.qos;
	if (qos) {
		if (_dispatch_set_qos_class_enabled) {
			(void)dispatch_assume_zero(pthread_attr_set_qos_class_np(attr,
					qos, 0));
		}
		_dispatch_mgr_q.dq_priority =
				(dispatch_priority_t)_pthread_qos_class_encode(qos, 0, 0);
	}
#endif
	param.sched_priority = _dispatch_mgr_sched.prio;
	if (param.sched_priority > _dispatch_mgr_sched.default_prio) {
		(void)dispatch_assume_zero(pthread_attr_setschedparam(attr, &param));
	}
	return &_dispatch_mgr_sched.tid;
}

static inline void
_dispatch_mgr_priority_apply(void)
{
	struct sched_param param;
	do {
		param.sched_priority = _dispatch_mgr_sched.prio;
		if (param.sched_priority > _dispatch_mgr_sched.default_prio) {
			(void)dispatch_assume_zero(pthread_setschedparam(
					_dispatch_mgr_sched.tid, _dispatch_mgr_sched.policy,
					&param));
		}
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
#if HAVE_PTHREAD_WORKQUEUE_QOS
	qos_class_t qos = 0;
	(void)pthread_attr_get_qos_class_np(attr, &qos, NULL);
	if (_dispatch_mgr_sched.qos > qos && _dispatch_set_qos_class_enabled) {
		(void)pthread_set_qos_class_self_np(_dispatch_mgr_sched.qos, 0);
		int p = _dispatch_mgr_sched_qos2prio[_dispatch_mgr_sched.qos];
		if (p > param.sched_priority) {
			param.sched_priority = p;
		}
	}
#endif
	if (slowpath(_dispatch_mgr_sched.prio > param.sched_priority)) {
		return _dispatch_mgr_priority_apply();
	}
}
#endif // DISPATCH_USE_MGR_THREAD && DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES

#if DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES
DISPATCH_NOINLINE
static void
_dispatch_mgr_priority_raise(const pthread_attr_t *attr)
{
	dispatch_once_f(&_dispatch_mgr_sched_pred, NULL, _dispatch_mgr_sched_init);
	struct sched_param param;
	(void)dispatch_assume_zero(pthread_attr_getschedparam(attr, &param));
#if HAVE_PTHREAD_WORKQUEUE_QOS
	qos_class_t q, qos = 0;
	(void)pthread_attr_get_qos_class_np((pthread_attr_t *)attr, &qos, NULL);
	if (qos) {
		param.sched_priority = _dispatch_mgr_sched_qos2prio[qos];
		os_atomic_rmw_loop2o(&_dispatch_mgr_sched, qos, q, qos, relaxed, {
			if (q >= qos) os_atomic_rmw_loop_give_up(break);
		});
	}
#endif
	int p, prio = param.sched_priority;
	os_atomic_rmw_loop2o(&_dispatch_mgr_sched, prio, p, prio, relaxed, {
		if (p >= prio) os_atomic_rmw_loop_give_up(return);
	});
#if DISPATCH_USE_KEVENT_WORKQUEUE
	dispatch_once_f(&_dispatch_root_queues_pred, NULL,
			_dispatch_root_queues_init_once);
	if (_dispatch_kevent_workqueue_enabled) {
		pthread_priority_t pp = 0;
		if (prio > _dispatch_mgr_sched.default_prio) {
			// The values of _PTHREAD_PRIORITY_SCHED_PRI_FLAG and
			// _PTHREAD_PRIORITY_ROOTQUEUE_FLAG overlap, but that is not
			// problematic in this case, since it the second one is only ever
			// used on dq_priority fields.
			// We never pass the _PTHREAD_PRIORITY_ROOTQUEUE_FLAG to a syscall,
			// it is meaningful to libdispatch only.
			pp = (pthread_priority_t)prio | _PTHREAD_PRIORITY_SCHED_PRI_FLAG;
		} else if (qos) {
			pp = _pthread_qos_class_encode(qos, 0, 0);
		}
		if (pp) {
			int r = _pthread_workqueue_set_event_manager_priority(pp);
			(void)dispatch_assume_zero(r);
		}
		return;
	}
#endif
#if DISPATCH_USE_MGR_THREAD
	if (_dispatch_mgr_sched.tid) {
		return _dispatch_mgr_priority_apply();
	}
#endif
}
#endif // DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES

#if DISPATCH_USE_KEVENT_WORKQUEUE
void
_dispatch_kevent_workqueue_init(void)
{
	// Initialize kevent workqueue support
	dispatch_once_f(&_dispatch_root_queues_pred, NULL,
			_dispatch_root_queues_init_once);
	if (!_dispatch_kevent_workqueue_enabled) return;
	dispatch_once_f(&_dispatch_mgr_sched_pred, NULL, _dispatch_mgr_sched_init);
	qos_class_t qos = _dispatch_mgr_sched.qos;
	int prio = _dispatch_mgr_sched.prio;
	pthread_priority_t pp = 0;
	if (qos) {
		pp = _pthread_qos_class_encode(qos, 0, 0);
		_dispatch_mgr_q.dq_priority = (dispatch_priority_t)pp;
	}
	if (prio > _dispatch_mgr_sched.default_prio) {
		pp = (pthread_priority_t)prio | _PTHREAD_PRIORITY_SCHED_PRI_FLAG;
	}
	if (pp) {
		int r = _pthread_workqueue_set_event_manager_priority(pp);
		(void)dispatch_assume_zero(r);
	}
}
#endif

#pragma mark -
#pragma mark dispatch_pthread_root_queue

#if DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES
static dispatch_queue_t
_dispatch_pthread_root_queue_create(const char *label, unsigned long flags,
		const pthread_attr_t *attr, dispatch_block_t configure,
		dispatch_pthread_root_queue_observer_hooks_t observer_hooks)
{
	dispatch_queue_t dq;
	dispatch_root_queue_context_t qc;
	dispatch_pthread_root_queue_context_t pqc;
	dispatch_queue_flags_t dqf = 0;
	size_t dqs;
	uint8_t pool_size = flags & _DISPATCH_PTHREAD_ROOT_QUEUE_FLAG_POOL_SIZE ?
			(uint8_t)(flags & ~_DISPATCH_PTHREAD_ROOT_QUEUE_FLAG_POOL_SIZE) : 0;

	dqs = sizeof(struct dispatch_queue_s) - DISPATCH_QUEUE_CACHELINE_PAD;
	dqs = roundup(dqs, _Alignof(struct dispatch_root_queue_context_s));
	dq = _dispatch_alloc(DISPATCH_VTABLE(queue_root), dqs +
			sizeof(struct dispatch_root_queue_context_s) +
			sizeof(struct dispatch_pthread_root_queue_context_s));
	qc = (void*)dq + dqs;
	dispatch_assert((uintptr_t)qc % _Alignof(typeof(*qc)) == 0);
	pqc = (void*)qc + sizeof(struct dispatch_root_queue_context_s);
	dispatch_assert((uintptr_t)pqc % _Alignof(typeof(*pqc)) == 0);
	if (label) {
		const char *tmp = _dispatch_strdup_if_mutable(label);
		if (tmp != label) {
			dqf |= DQF_LABEL_NEEDS_FREE;
			label = tmp;
		}
	}

	_dispatch_queue_init(dq, dqf, DISPATCH_QUEUE_WIDTH_POOL, false);
	dq->dq_label = label;
	dq->dq_state = DISPATCH_ROOT_QUEUE_STATE_INIT_VALUE,
	dq->dq_override = DISPATCH_SATURATED_OVERRIDE;
	dq->do_ctxt = qc;
	dq->do_targetq = NULL;

	pqc->dpq_thread_mediator.do_vtable = DISPATCH_VTABLE(semaphore);
	qc->dgq_ctxt = pqc;
#if HAVE_PTHREAD_WORKQUEUES
	qc->dgq_kworkqueue = (void*)(~0ul);
#endif
	_dispatch_root_queue_init_pthread_pool(qc, pool_size, true);

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
	if (observer_hooks) {
		pqc->dpq_observer_hooks = *observer_hooks;
	}
	_dispatch_object_debug(dq, "%s", __func__);
	return _dispatch_introspection_queue_create(dq);
}

dispatch_queue_t
dispatch_pthread_root_queue_create(const char *label, unsigned long flags,
		const pthread_attr_t *attr, dispatch_block_t configure)
{
	return _dispatch_pthread_root_queue_create(label, flags, attr, configure,
			NULL);
}

#if DISPATCH_IOHID_SPI
dispatch_queue_t
_dispatch_pthread_root_queue_create_with_observer_hooks_4IOHID(const char *label,
		unsigned long flags, const pthread_attr_t *attr,
		dispatch_pthread_root_queue_observer_hooks_t observer_hooks,
		dispatch_block_t configure)
{
	if (!observer_hooks->queue_will_execute ||
			!observer_hooks->queue_did_execute) {
		DISPATCH_CLIENT_CRASH(0, "Invalid pthread root queue observer hooks");
	}
	return _dispatch_pthread_root_queue_create(label, flags, attr, configure,
			observer_hooks);
}
#endif

dispatch_queue_t
dispatch_pthread_root_queue_copy_current(void)
{
	dispatch_queue_t dq = _dispatch_queue_get_current();
	if (!dq) return NULL;
	while (slowpath(dq->do_targetq)) {
		dq = dq->do_targetq;
	}
	if (dx_type(dq) != DISPATCH_QUEUE_GLOBAL_ROOT_TYPE ||
			dq->do_xref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT) {
		return NULL;
	}
	return (dispatch_queue_t)_os_object_retain_with_resurrect(dq->_as_os_obj);
}

#endif // DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES

void
_dispatch_pthread_root_queue_dispose(dispatch_queue_t dq)
{
	if (slowpath(dq->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		DISPATCH_INTERNAL_CRASH(dq, "Global root queue disposed");
	}
	_dispatch_object_debug(dq, "%s", __func__);
	_dispatch_introspection_queue_dispose(dq);
#if DISPATCH_USE_PTHREAD_POOL
	dispatch_root_queue_context_t qc = dq->do_ctxt;
	dispatch_pthread_root_queue_context_t pqc = qc->dgq_ctxt;

	pthread_attr_destroy(&pqc->dpq_thread_attr);
	_dispatch_semaphore_dispose(&pqc->dpq_thread_mediator);
	if (pqc->dpq_thread_configure) {
		Block_release(pqc->dpq_thread_configure);
	}
	dq->do_targetq = _dispatch_get_root_queue(_DISPATCH_QOS_CLASS_DEFAULT,
			false);
#endif
	if (dq->dq_label && _dispatch_queue_label_needs_free(dq)) {
		free((void*)dq->dq_label);
	}
	_dispatch_queue_destroy(dq);
}

#pragma mark -
#pragma mark dispatch_queue_specific

struct dispatch_queue_specific_queue_s {
	DISPATCH_QUEUE_HEADER(queue_specific_queue);
	TAILQ_HEAD(dispatch_queue_specific_head_s,
			dispatch_queue_specific_s) dqsq_contexts;
} DISPATCH_QUEUE_ALIGN;

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
					_DISPATCH_QOS_CLASS_DEFAULT, false), dqs->dqs_ctxt,
					dqs->dqs_destructor);
		}
		free(dqs);
	}
	_dispatch_queue_destroy(dqsq->_as_dq);
}

static void
_dispatch_queue_init_specific(dispatch_queue_t dq)
{
	dispatch_queue_specific_queue_t dqsq;

	dqsq = _dispatch_alloc(DISPATCH_VTABLE(queue_specific_queue),
			sizeof(struct dispatch_queue_specific_queue_s));
	_dispatch_queue_init(dqsq->_as_dq, DQF_NONE,
			DISPATCH_QUEUE_WIDTH_MAX, false);
	dqsq->do_xref_cnt = -1;
	dqsq->do_targetq = _dispatch_get_root_queue(
			_DISPATCH_QOS_CLASS_USER_INITIATED, true);
	dqsq->dq_label = "queue-specific";
	TAILQ_INIT(&dqsq->dqsq_contexts);
	if (slowpath(!os_atomic_cmpxchg2o(dq, dq_specific_q, NULL,
			dqsq->_as_dq, release))) {
		_dispatch_release(dqsq->_as_dq);
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
						_DISPATCH_QOS_CLASS_DEFAULT, false), dqs->dqs_ctxt,
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
	_dispatch_barrier_trysync_or_async_f(dq->dq_specific_q, dqs,
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

#if DISPATCH_IOHID_SPI
bool
_dispatch_queue_is_exclusively_owned_by_current_thread_4IOHID(
		dispatch_queue_t dq) // rdar://problem/18033810
{
	if (dq->dq_width != 1) {
		DISPATCH_CLIENT_CRASH(dq->dq_width, "Invalid queue type");
	}
	uint64_t dq_state = os_atomic_load2o(dq, dq_state, relaxed);
	return _dq_state_drain_locked_by(dq_state, _dispatch_tid_self());
}
#endif

#pragma mark -
#pragma mark dispatch_queue_debug

size_t
_dispatch_queue_debug_attr(dispatch_queue_t dq, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	dispatch_queue_t target = dq->do_targetq;
	uint64_t dq_state = os_atomic_load2o(dq, dq_state, relaxed);

	offset += dsnprintf(&buf[offset], bufsiz - offset,
			"target = %s[%p], width = 0x%x, state = 0x%016llx",
			target && target->dq_label ? target->dq_label : "", target,
			dq->dq_width, (unsigned long long)dq_state);
	if (_dq_state_is_suspended(dq_state)) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, ", suspended = %d",
			_dq_state_suspend_cnt(dq_state));
	}
	if (_dq_state_is_inactive(dq_state)) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, ", inactive");
	} else if (_dq_state_needs_activation(dq_state)) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, ", needs-activation");
	}
	if (_dq_state_is_enqueued(dq_state)) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, ", enqueued");
	}
	if (_dq_state_is_dirty(dq_state)) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, ", dirty");
	}
	if (_dq_state_has_override(dq_state)) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, ", async-override");
	}
	mach_port_t owner = _dq_state_drain_owner(dq_state);
	if (!_dispatch_queue_is_thread_bound(dq) && owner) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, ", draining on 0x%x",
				owner);
	}
	if (_dq_state_is_in_barrier(dq_state)) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, ", in-barrier");
	} else  {
		offset += dsnprintf(&buf[offset], bufsiz - offset, ", in-flight = %d",
				_dq_state_used_width(dq_state, dq->dq_width));
	}
	if (_dq_state_has_pending_barrier(dq_state)) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, ", pending-barrier");
	}
	if (_dispatch_queue_is_thread_bound(dq)) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, ", thread = 0x%x ",
				owner);
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

#if DISPATCH_PERF_MON && !DISPATCH_INTROSPECTION
static OSSpinLock _dispatch_stats_lock;
static struct {
	uint64_t time_total;
	uint64_t count_total;
	uint64_t thread_total;
} _dispatch_stats[65]; // ffs*/fls*() returns zero when no bits are set

static void
_dispatch_queue_merge_stats(uint64_t start)
{
	uint64_t delta = _dispatch_absolute_time() - start;
	unsigned long count;

	count = (unsigned long)_dispatch_thread_getspecific(dispatch_bcounter_key);
	_dispatch_thread_setspecific(dispatch_bcounter_key, NULL);

	int bucket = flsl((long)count);

	// 64-bit counters on 32-bit require a lock or a queue
	OSSpinLockLock(&_dispatch_stats_lock);

	_dispatch_stats[bucket].time_total += delta;
	_dispatch_stats[bucket].count_total += count;
	_dispatch_stats[bucket].thread_total++;

	OSSpinLockUnlock(&_dispatch_stats_lock);
}
#endif

#pragma mark -
#pragma mark _dispatch_set_priority_and_mach_voucher
#if HAVE_PTHREAD_WORKQUEUE_QOS

DISPATCH_NOINLINE
void
_dispatch_set_priority_and_mach_voucher_slow(pthread_priority_t pp,
		mach_voucher_t kv)
{
	_pthread_set_flags_t pflags = 0;
	if (pp && _dispatch_set_qos_class_enabled) {
		pthread_priority_t old_pri = _dispatch_get_priority();
		if (pp != old_pri) {
			if (old_pri & _PTHREAD_PRIORITY_NEEDS_UNBIND_FLAG) {
				pflags |= _PTHREAD_SET_SELF_WQ_KEVENT_UNBIND;
				// when we unbind, overcomitness can flip, so we need to learn
				// it from the defaultpri, see _dispatch_priority_compute_update
				pp |= (_dispatch_get_defaultpriority() &
						_PTHREAD_PRIORITY_OVERCOMMIT_FLAG);
			} else {
				// else we need to keep the one that is set in the current pri
				pp |= (old_pri & _PTHREAD_PRIORITY_OVERCOMMIT_FLAG);
			}
			if (likely(old_pri & ~_PTHREAD_PRIORITY_FLAGS_MASK)) {
				pflags |= _PTHREAD_SET_SELF_QOS_FLAG;
			}
			if (unlikely(DISPATCH_QUEUE_DRAIN_OWNER(&_dispatch_mgr_q) ==
					_dispatch_tid_self())) {
				DISPATCH_INTERNAL_CRASH(pp,
						"Changing the QoS while on the manager queue");
			}
			if (unlikely(pp & _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG)) {
				DISPATCH_INTERNAL_CRASH(pp, "Cannot raise oneself to manager");
			}
			if (old_pri & _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG) {
				DISPATCH_INTERNAL_CRASH(old_pri,
						"Cannot turn a manager thread into a normal one");
			}
		}
	}
	if (kv != VOUCHER_NO_MACH_VOUCHER) {
#if VOUCHER_USE_MACH_VOUCHER
		pflags |= _PTHREAD_SET_SELF_VOUCHER_FLAG;
#endif
	}
	if (!pflags) return;
	int r = _pthread_set_properties_self(pflags, pp, kv);
	if (r == EINVAL) {
		DISPATCH_INTERNAL_CRASH(pp, "_pthread_set_properties_self failed");
	}
	(void)dispatch_assume_zero(r);
}

DISPATCH_NOINLINE
voucher_t
_dispatch_set_priority_and_voucher_slow(pthread_priority_t priority,
		voucher_t v, _dispatch_thread_set_self_t flags)
{
	voucher_t ov = DISPATCH_NO_VOUCHER;
	mach_voucher_t kv = VOUCHER_NO_MACH_VOUCHER;
	if (v != DISPATCH_NO_VOUCHER) {
		bool retained = flags & DISPATCH_VOUCHER_CONSUME;
		ov = _voucher_get();
		if (ov == v && (flags & DISPATCH_VOUCHER_REPLACE)) {
			if (retained && v) _voucher_release_no_dispose(v);
			ov = DISPATCH_NO_VOUCHER;
		} else {
			if (!retained && v) _voucher_retain(v);
			kv = _voucher_swap_and_get_mach_voucher(ov, v);
		}
	}
#if !PTHREAD_WORKQUEUE_RESETS_VOUCHER_AND_PRIORITY_ON_PARK
	flags &= ~(_dispatch_thread_set_self_t)DISPATCH_THREAD_PARK;
#endif
	if (!(flags & DISPATCH_THREAD_PARK)) {
		_dispatch_set_priority_and_mach_voucher_slow(priority, kv);
	}
	if (ov != DISPATCH_NO_VOUCHER && (flags & DISPATCH_VOUCHER_REPLACE)) {
		if (ov) _voucher_release(ov);
		ov = DISPATCH_NO_VOUCHER;
	}
	return ov;
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

#if DISPATCH_USE_MEMORYPRESSURE_SOURCE
DISPATCH_NOINLINE
void
_dispatch_continuation_free_to_cache_limit(dispatch_continuation_t dc)
{
	_dispatch_continuation_free_to_heap(dc);
	dispatch_continuation_t next_dc;
	dc = _dispatch_thread_getspecific(dispatch_cache_key);
	int cnt;
	if (!dc || (cnt = dc->dc_cache_cnt -
			_dispatch_continuation_cache_limit) <= 0){
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
_dispatch_continuation_slow_item_signal(dispatch_queue_t dq,
		dispatch_object_t dou)
{
	dispatch_continuation_t dc = dou._dc;
	pthread_priority_t pp = dq->dq_override;

	_dispatch_trace_continuation_pop(dq, dc);
	if (pp > (dc->dc_priority & _PTHREAD_PRIORITY_QOS_CLASS_MASK)) {
		_dispatch_wqthread_override_start((mach_port_t)dc->dc_data, pp);
	}
	_dispatch_thread_event_signal((dispatch_thread_event_t)dc->dc_other);
	_dispatch_introspection_queue_item_complete(dc);
}

DISPATCH_NOINLINE
static void
_dispatch_continuation_push(dispatch_queue_t dq, dispatch_continuation_t dc)
{
	_dispatch_queue_push(dq, dc,
			_dispatch_continuation_get_override_priority(dq, dc));
}

DISPATCH_NOINLINE
static void
_dispatch_continuation_push_sync_slow(dispatch_queue_t dq,
		dispatch_continuation_t dc)
{
	_dispatch_queue_push_inline(dq, dc,
			_dispatch_continuation_get_override_priority(dq, dc),
			DISPATCH_WAKEUP_SLOW_WAITER);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_async2(dispatch_queue_t dq, dispatch_continuation_t dc,
		bool barrier)
{
	if (fastpath(barrier || !DISPATCH_QUEUE_USES_REDIRECTION(dq->dq_width))) {
		return _dispatch_continuation_push(dq, dc);
	}
	return _dispatch_async_f2(dq, dc);
}

DISPATCH_NOINLINE
void
_dispatch_continuation_async(dispatch_queue_t dq, dispatch_continuation_t dc)
{
	_dispatch_continuation_async2(dq, dc,
			dc->dc_flags & DISPATCH_OBJ_BARRIER_BIT);
}

#pragma mark -
#pragma mark dispatch_block_create

#if __BLOCKS__

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_block_flags_valid(dispatch_block_flags_t flags)
{
	return ((flags & ~DISPATCH_BLOCK_API_MASK) == 0);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_block_flags_t
_dispatch_block_normalize_flags(dispatch_block_flags_t flags)
{
	if (flags & (DISPATCH_BLOCK_NO_VOUCHER|DISPATCH_BLOCK_DETACHED)) {
		flags |= DISPATCH_BLOCK_HAS_VOUCHER;
	}
	if (flags & (DISPATCH_BLOCK_NO_QOS_CLASS|DISPATCH_BLOCK_DETACHED)) {
		flags |= DISPATCH_BLOCK_HAS_PRIORITY;
	}
	return flags;
}

static inline dispatch_block_t
_dispatch_block_create_with_voucher_and_priority(dispatch_block_flags_t flags,
		voucher_t voucher, pthread_priority_t pri, dispatch_block_t block)
{
	flags = _dispatch_block_normalize_flags(flags);
	bool assign = (flags & DISPATCH_BLOCK_ASSIGN_CURRENT);

	if (assign && !(flags & DISPATCH_BLOCK_HAS_VOUCHER)) {
#if OS_VOUCHER_ACTIVITY_SPI
		voucher = VOUCHER_CURRENT;
#endif
		flags |= DISPATCH_BLOCK_HAS_VOUCHER;
	}
#if OS_VOUCHER_ACTIVITY_SPI
	if (voucher == VOUCHER_CURRENT) {
		voucher = _voucher_get();
	}
#endif
	if (assign && !(flags & DISPATCH_BLOCK_HAS_PRIORITY)) {
		pri = _dispatch_priority_propagate();
		flags |= DISPATCH_BLOCK_HAS_PRIORITY;
	}
	dispatch_block_t db = _dispatch_block_create(flags, voucher, pri, block);
#if DISPATCH_DEBUG
	dispatch_assert(_dispatch_block_get_data(db));
#endif
	return db;
}

dispatch_block_t
dispatch_block_create(dispatch_block_flags_t flags, dispatch_block_t block)
{
	if (!_dispatch_block_flags_valid(flags)) return DISPATCH_BAD_INPUT;
	return _dispatch_block_create_with_voucher_and_priority(flags, NULL, 0,
			block);
}

dispatch_block_t
dispatch_block_create_with_qos_class(dispatch_block_flags_t flags,
		dispatch_qos_class_t qos_class, int relative_priority,
		dispatch_block_t block)
{
	if (!_dispatch_block_flags_valid(flags) ||
			!_dispatch_qos_class_valid(qos_class, relative_priority)) {
		return DISPATCH_BAD_INPUT;
	}
	flags |= DISPATCH_BLOCK_HAS_PRIORITY;
	pthread_priority_t pri = 0;
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pri = _pthread_qos_class_encode(qos_class, relative_priority, 0);
#endif
	return _dispatch_block_create_with_voucher_and_priority(flags, NULL,
			pri, block);
}

dispatch_block_t
dispatch_block_create_with_voucher(dispatch_block_flags_t flags,
		voucher_t voucher, dispatch_block_t block)
{
	if (!_dispatch_block_flags_valid(flags)) return DISPATCH_BAD_INPUT;
	flags |= DISPATCH_BLOCK_HAS_VOUCHER;
	return _dispatch_block_create_with_voucher_and_priority(flags, voucher, 0,
			block);
}

dispatch_block_t
dispatch_block_create_with_voucher_and_qos_class(dispatch_block_flags_t flags,
		voucher_t voucher, dispatch_qos_class_t qos_class,
		int relative_priority, dispatch_block_t block)
{
	if (!_dispatch_block_flags_valid(flags) ||
			!_dispatch_qos_class_valid(qos_class, relative_priority)) {
		return DISPATCH_BAD_INPUT;
	}
	flags |= (DISPATCH_BLOCK_HAS_VOUCHER|DISPATCH_BLOCK_HAS_PRIORITY);
	pthread_priority_t pri = 0;
#if HAVE_PTHREAD_WORKQUEUE_QOS
	pri = _pthread_qos_class_encode(qos_class, relative_priority, 0);
#endif
	return _dispatch_block_create_with_voucher_and_priority(flags, voucher,
			pri, block);
}

void
dispatch_block_perform(dispatch_block_flags_t flags, dispatch_block_t block)
{
	if (!_dispatch_block_flags_valid(flags)) {
		DISPATCH_CLIENT_CRASH(flags, "Invalid flags passed to "
				"dispatch_block_perform()");
	}
	flags = _dispatch_block_normalize_flags(flags);
	struct dispatch_block_private_data_s dbpds =
			DISPATCH_BLOCK_PRIVATE_DATA_PERFORM_INITIALIZER(flags, block);
	return _dispatch_block_invoke_direct(&dbpds);
}

#define _dbpd_group(dbpd) ((dbpd)->dbpd_group)

void
_dispatch_block_invoke_direct(const struct dispatch_block_private_data_s *dbcpd)
{
	dispatch_block_private_data_t dbpd = (dispatch_block_private_data_t)dbcpd;
	dispatch_block_flags_t flags = dbpd->dbpd_flags;
	unsigned int atomic_flags = dbpd->dbpd_atomic_flags;
	if (slowpath(atomic_flags & DBF_WAITED)) {
		DISPATCH_CLIENT_CRASH(atomic_flags, "A block object may not be both "
				"run more than once and waited for");
	}
	if (atomic_flags & DBF_CANCELED) goto out;

	pthread_priority_t op = DISPATCH_NO_PRIORITY, p = DISPATCH_NO_PRIORITY;
	_dispatch_thread_set_self_t adopt_flags = 0;
	if (flags & DISPATCH_BLOCK_HAS_PRIORITY) {
		op = _dispatch_get_priority();
		p = dbpd->dbpd_priority;
		if (_dispatch_block_sync_should_enforce_qos_class(flags)) {
			adopt_flags |= DISPATCH_PRIORITY_ENFORCE;
		}
	}
	voucher_t ov, v = DISPATCH_NO_VOUCHER;
	if (flags & DISPATCH_BLOCK_HAS_VOUCHER) {
		v = dbpd->dbpd_voucher;
	}
	ov = _dispatch_adopt_priority_and_set_voucher(p, v, adopt_flags);
	dbpd->dbpd_thread = _dispatch_tid_self();
	_dispatch_client_callout(dbpd->dbpd_block,
			_dispatch_Block_invoke(dbpd->dbpd_block));
	_dispatch_reset_priority_and_voucher(op, ov);
out:
	if ((atomic_flags & DBF_PERFORM) == 0) {
		if (os_atomic_inc2o(dbpd, dbpd_performed, relaxed) == 1) {
			dispatch_group_leave(_dbpd_group(dbpd));
		}
	}
}

void
_dispatch_block_sync_invoke(void *block)
{
	dispatch_block_t b = block;
	dispatch_block_private_data_t dbpd = _dispatch_block_get_data(b);
	dispatch_block_flags_t flags = dbpd->dbpd_flags;
	unsigned int atomic_flags = dbpd->dbpd_atomic_flags;
	if (slowpath(atomic_flags & DBF_WAITED)) {
		DISPATCH_CLIENT_CRASH(atomic_flags, "A block object may not be both "
				"run more than once and waited for");
	}
	if (atomic_flags & DBF_CANCELED) goto out;

	pthread_priority_t op = DISPATCH_NO_PRIORITY, p = DISPATCH_NO_PRIORITY;
	_dispatch_thread_set_self_t adopt_flags = 0;
	if (flags & DISPATCH_BLOCK_HAS_PRIORITY) {
		op = _dispatch_get_priority();
		p = dbpd->dbpd_priority;
		if (_dispatch_block_sync_should_enforce_qos_class(flags)) {
			adopt_flags |= DISPATCH_PRIORITY_ENFORCE;
		}
	}
	voucher_t ov, v = DISPATCH_NO_VOUCHER;
	if (flags & DISPATCH_BLOCK_HAS_VOUCHER) {
		v = dbpd->dbpd_voucher;
	}
	ov = _dispatch_adopt_priority_and_set_voucher(p, v, adopt_flags);
	dbpd->dbpd_block();
	_dispatch_reset_priority_and_voucher(op, ov);
out:
	if ((atomic_flags & DBF_PERFORM) == 0) {
		if (os_atomic_inc2o(dbpd, dbpd_performed, relaxed) == 1) {
			dispatch_group_leave(_dbpd_group(dbpd));
		}
	}

	os_mpsc_queue_t oq;
	oq = os_atomic_xchg2o(dbpd, dbpd_queue, NULL, relaxed);
	if (oq) {
		// balances dispatch_{,barrier_,}sync
		_os_object_release_internal(oq->_as_os_obj);
	}
}

DISPATCH_ALWAYS_INLINE
static void
_dispatch_block_async_invoke2(dispatch_block_t b, bool release)
{
	dispatch_block_private_data_t dbpd = _dispatch_block_get_data(b);
	unsigned int atomic_flags = dbpd->dbpd_atomic_flags;
	if (slowpath(atomic_flags & DBF_WAITED)) {
		DISPATCH_CLIENT_CRASH(atomic_flags, "A block object may not be both "
				"run more than once and waited for");
	}
	if (!slowpath(atomic_flags & DBF_CANCELED)) {
		dbpd->dbpd_block();
	}
	if ((atomic_flags & DBF_PERFORM) == 0) {
		if (os_atomic_inc2o(dbpd, dbpd_performed, relaxed) == 1) {
			dispatch_group_leave(_dbpd_group(dbpd));
		}
	}
	os_mpsc_queue_t oq;
	oq = os_atomic_xchg2o(dbpd, dbpd_queue, NULL, relaxed);
	if (oq) {
		// balances dispatch_{,barrier_,group_}async
		_os_object_release_internal_inline(oq->_as_os_obj);
	}
	if (release) {
		Block_release(b);
	}
}

static void
_dispatch_block_async_invoke(void *block)
{
	_dispatch_block_async_invoke2(block, false);
}

static void
_dispatch_block_async_invoke_and_release(void *block)
{
	_dispatch_block_async_invoke2(block, true);
}

void
dispatch_block_cancel(dispatch_block_t db)
{
	dispatch_block_private_data_t dbpd = _dispatch_block_get_data(db);
	if (!dbpd) {
		DISPATCH_CLIENT_CRASH(db, "Invalid block object passed to "
				"dispatch_block_cancel()");
	}
	(void)os_atomic_or2o(dbpd, dbpd_atomic_flags, DBF_CANCELED, relaxed);
}

long
dispatch_block_testcancel(dispatch_block_t db)
{
	dispatch_block_private_data_t dbpd = _dispatch_block_get_data(db);
	if (!dbpd) {
		DISPATCH_CLIENT_CRASH(db, "Invalid block object passed to "
				"dispatch_block_testcancel()");
	}
	return (bool)(dbpd->dbpd_atomic_flags & DBF_CANCELED);
}

long
dispatch_block_wait(dispatch_block_t db, dispatch_time_t timeout)
{
	dispatch_block_private_data_t dbpd = _dispatch_block_get_data(db);
	if (!dbpd) {
		DISPATCH_CLIENT_CRASH(db, "Invalid block object passed to "
				"dispatch_block_wait()");
	}

	unsigned int flags = os_atomic_or_orig2o(dbpd, dbpd_atomic_flags,
			DBF_WAITING, relaxed);
	if (slowpath(flags & (DBF_WAITED | DBF_WAITING))) {
		DISPATCH_CLIENT_CRASH(flags, "A block object may not be waited for "
				"more than once");
	}

	// <rdar://problem/17703192> If we know the queue where this block is
	// enqueued, or the thread that's executing it, then we should boost
	// it here.

	pthread_priority_t pp = _dispatch_get_priority();

	os_mpsc_queue_t boost_oq;
	boost_oq = os_atomic_xchg2o(dbpd, dbpd_queue, NULL, relaxed);
	if (boost_oq) {
		// release balances dispatch_{,barrier_,group_}async.
		// Can't put the queue back in the timeout case: the block might
		// finish after we fell out of group_wait and see our NULL, so
		// neither of us would ever release. Side effect: After a _wait
		// that times out, subsequent waits will not boost the qos of the
		// still-running block.
		dx_wakeup(boost_oq, pp, DISPATCH_WAKEUP_OVERRIDING |
				DISPATCH_WAKEUP_CONSUME);
	}

	mach_port_t boost_th = dbpd->dbpd_thread;
	if (boost_th) {
		_dispatch_thread_override_start(boost_th, pp, dbpd);
	}

	int performed = os_atomic_load2o(dbpd, dbpd_performed, relaxed);
	if (slowpath(performed > 1 || (boost_th && boost_oq))) {
		DISPATCH_CLIENT_CRASH(performed, "A block object may not be both "
				"run more than once and waited for");
	}

	long ret = dispatch_group_wait(_dbpd_group(dbpd), timeout);

	if (boost_th) {
		_dispatch_thread_override_end(boost_th, dbpd);
	}

	if (ret) {
		// timed out: reverse our changes
		(void)os_atomic_and2o(dbpd, dbpd_atomic_flags,
				~DBF_WAITING, relaxed);
	} else {
		(void)os_atomic_or2o(dbpd, dbpd_atomic_flags,
				DBF_WAITED, relaxed);
		// don't need to re-test here: the second call would see
		// the first call's WAITING
	}

	return ret;
}

void
dispatch_block_notify(dispatch_block_t db, dispatch_queue_t queue,
		dispatch_block_t notification_block)
{
	dispatch_block_private_data_t dbpd = _dispatch_block_get_data(db);
	if (!dbpd) {
		DISPATCH_CLIENT_CRASH(db, "Invalid block object passed to "
				"dispatch_block_notify()");
	}
	int performed = os_atomic_load2o(dbpd, dbpd_performed, relaxed);
	if (slowpath(performed > 1)) {
		DISPATCH_CLIENT_CRASH(performed, "A block object may not be both "
				"run more than once and observed");
	}

	return dispatch_group_notify(_dbpd_group(dbpd), queue, notification_block);
}

DISPATCH_NOINLINE
void
_dispatch_continuation_init_slow(dispatch_continuation_t dc,
		dispatch_queue_class_t dqu, dispatch_block_flags_t flags)
{
	dispatch_block_private_data_t dbpd = _dispatch_block_get_data(dc->dc_ctxt);
	dispatch_block_flags_t block_flags = dbpd->dbpd_flags;
	uintptr_t dc_flags = dc->dc_flags;
	os_mpsc_queue_t oq = dqu._oq;

	// balanced in d_block_async_invoke_and_release or d_block_wait
	if (os_atomic_cmpxchg2o(dbpd, dbpd_queue, NULL, oq, relaxed)) {
		_os_object_retain_internal_inline(oq->_as_os_obj);
	}

	if (dc_flags & DISPATCH_OBJ_CONSUME_BIT) {
		dc->dc_func = _dispatch_block_async_invoke_and_release;
	} else {
		dc->dc_func = _dispatch_block_async_invoke;
	}

	flags |= block_flags;
	if (block_flags & DISPATCH_BLOCK_HAS_PRIORITY) {
		_dispatch_continuation_priority_set(dc, dbpd->dbpd_priority, flags);
	} else {
		_dispatch_continuation_priority_set(dc, dc->dc_priority, flags);
	}
	if (block_flags & DISPATCH_BLOCK_BARRIER) {
		dc_flags |= DISPATCH_OBJ_BARRIER_BIT;
	}
	if (block_flags & DISPATCH_BLOCK_HAS_VOUCHER) {
		voucher_t v = dbpd->dbpd_voucher;
		dc->dc_voucher = v ? _voucher_retain(v) : NULL;
		dc_flags |= DISPATCH_OBJ_ENFORCE_VOUCHER;
		_dispatch_voucher_debug("continuation[%p] set", dc->dc_voucher, dc);
		_dispatch_voucher_ktrace_dc_push(dc);
	} else {
		_dispatch_continuation_voucher_set(dc, oq, flags);
	}
	dc_flags |= DISPATCH_OBJ_BLOCK_PRIVATE_DATA_BIT;
	dc->dc_flags = dc_flags;
}

void
_dispatch_continuation_update_bits(dispatch_continuation_t dc,
		uintptr_t dc_flags)
{
	dc->dc_flags = dc_flags;
	if (dc_flags & DISPATCH_OBJ_CONSUME_BIT) {
		if (dc_flags & DISPATCH_OBJ_BLOCK_PRIVATE_DATA_BIT) {
			dc->dc_func = _dispatch_block_async_invoke_and_release;
		} else if (dc_flags & DISPATCH_OBJ_BLOCK_BIT) {
			dc->dc_func = _dispatch_call_block_and_release;
		}
	} else {
		if (dc_flags & DISPATCH_OBJ_BLOCK_PRIVATE_DATA_BIT) {
			dc->dc_func = _dispatch_block_async_invoke;
		} else if (dc_flags & DISPATCH_OBJ_BLOCK_BIT) {
			dc->dc_func = _dispatch_Block_invoke(dc->dc_ctxt);
		}
	}
}

#endif // __BLOCKS__

#pragma mark -
#pragma mark dispatch_barrier_async

DISPATCH_NOINLINE
static void
_dispatch_async_f_slow(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func, pthread_priority_t pp,
		dispatch_block_flags_t flags, uintptr_t dc_flags)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc_from_heap();
	_dispatch_continuation_init_f(dc, dq, ctxt, func, pp, flags, dc_flags);
	_dispatch_continuation_async(dq, dc);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_barrier_async_f2(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func, pthread_priority_t pp,
		dispatch_block_flags_t flags)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc_cacheonly();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT | DISPATCH_OBJ_BARRIER_BIT;

	if (!fastpath(dc)) {
		return _dispatch_async_f_slow(dq, ctxt, func, pp, flags, dc_flags);
	}

	_dispatch_continuation_init_f(dc, dq, ctxt, func, pp, flags, dc_flags);
	_dispatch_continuation_push(dq, dc);
}

DISPATCH_NOINLINE
void
dispatch_barrier_async_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_barrier_async_f2(dq, ctxt, func, 0, 0);
}

DISPATCH_NOINLINE
void
_dispatch_barrier_async_detached_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	dc->dc_flags = DISPATCH_OBJ_CONSUME_BIT | DISPATCH_OBJ_BARRIER_BIT;
	dc->dc_func = func;
	dc->dc_ctxt = ctxt;
	dc->dc_voucher = DISPATCH_NO_VOUCHER;
	dc->dc_priority = DISPATCH_NO_PRIORITY;
	_dispatch_queue_push(dq, dc, 0);
}

#ifdef __BLOCKS__
void
dispatch_barrier_async(dispatch_queue_t dq, void (^work)(void))
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT | DISPATCH_OBJ_BARRIER_BIT;

	_dispatch_continuation_init(dc, dq, work, 0, 0, dc_flags);
	_dispatch_continuation_push(dq, dc);
}
#endif

#pragma mark -
#pragma mark dispatch_async

void
_dispatch_async_redirect_invoke(dispatch_continuation_t dc,
		dispatch_invoke_flags_t flags)
{
	dispatch_thread_frame_s dtf;
	struct dispatch_continuation_s *other_dc = dc->dc_other;
	dispatch_invoke_flags_t ctxt_flags = (dispatch_invoke_flags_t)dc->dc_ctxt;
	// if we went through _dispatch_root_queue_push_override,
	// the "right" root queue was stuffed into dc_func
	dispatch_queue_t assumed_rq = (dispatch_queue_t)dc->dc_func;
	dispatch_queue_t dq = dc->dc_data, rq, old_dq;
	struct _dispatch_identity_s di;

	pthread_priority_t op, dp, old_dp;

	if (ctxt_flags) {
		flags &= ~_DISPATCH_INVOKE_AUTORELEASE_MASK;
		flags |= ctxt_flags;
	}
	old_dq = _dispatch_get_current_queue();
	if (assumed_rq) {
		_dispatch_queue_set_current(assumed_rq);
		_dispatch_root_queue_identity_assume(&di, 0);
	}

	old_dp = _dispatch_set_defaultpriority(dq->dq_priority, &dp);
	op = dq->dq_override;
	if (op > (dp & _PTHREAD_PRIORITY_QOS_CLASS_MASK)) {
		_dispatch_wqthread_override_start(_dispatch_tid_self(), op);
		// Ensure that the root queue sees that this thread was overridden.
		_dispatch_set_defaultpriority_override();
	}

	_dispatch_thread_frame_push(&dtf, dq);
	_dispatch_continuation_pop_forwarded(dc, DISPATCH_NO_VOUCHER,
			DISPATCH_OBJ_CONSUME_BIT, {
		_dispatch_continuation_pop(other_dc, dq, flags);
	});
	_dispatch_thread_frame_pop(&dtf);
	if (assumed_rq) {
		_dispatch_root_queue_identity_restore(&di);
		_dispatch_queue_set_current(old_dq);
	}
	_dispatch_reset_defaultpriority(old_dp);

	rq = dq->do_targetq;
	while (slowpath(rq->do_targetq) && rq != old_dq) {
		_dispatch_non_barrier_complete(rq);
		rq = rq->do_targetq;
	}

	_dispatch_non_barrier_complete(dq);

	if (dtf.dtf_deferred) {
		struct dispatch_object_s *dou = dtf.dtf_deferred;
		return _dispatch_queue_drain_deferred_invoke(dq, flags, 0, dou);
	}

	_dispatch_release_tailcall(dq);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_continuation_t
_dispatch_async_redirect_wrap(dispatch_queue_t dq, dispatch_object_t dou)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();

	dou._do->do_next = NULL;
	dc->do_vtable = DC_VTABLE(ASYNC_REDIRECT);
	dc->dc_func = NULL;
	dc->dc_ctxt = (void *)(uintptr_t)_dispatch_queue_autorelease_frequency(dq);
	dc->dc_data = dq;
	dc->dc_other = dou._do;
	dc->dc_voucher = DISPATCH_NO_VOUCHER;
	dc->dc_priority = DISPATCH_NO_PRIORITY;
	_dispatch_retain(dq);
	return dc;
}

DISPATCH_NOINLINE
static void
_dispatch_async_f_redirect(dispatch_queue_t dq,
		dispatch_object_t dou, pthread_priority_t pp)
{
	if (!slowpath(_dispatch_object_is_redirection(dou))) {
		dou._dc = _dispatch_async_redirect_wrap(dq, dou);
	}
	dq = dq->do_targetq;

	// Find the queue to redirect to
	while (slowpath(DISPATCH_QUEUE_USES_REDIRECTION(dq->dq_width))) {
		if (!fastpath(_dispatch_queue_try_acquire_async(dq))) {
			break;
		}
		if (!dou._dc->dc_ctxt) {
			// find first queue in descending target queue order that has
			// an autorelease frequency set, and use that as the frequency for
			// this continuation.
			dou._dc->dc_ctxt = (void *)
					(uintptr_t)_dispatch_queue_autorelease_frequency(dq);
		}
		dq = dq->do_targetq;
	}

	_dispatch_queue_push(dq, dou, pp);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_redirect(dispatch_queue_t dq,
		struct dispatch_object_s *dc)
{
	_dispatch_trace_continuation_pop(dq, dc);
	// This is a re-redirect, overrides have already been applied
	// by _dispatch_async_f2.
	// However we want to end up on the root queue matching `dc` qos, so pick up
	// the current override of `dq` which includes dc's overrde (and maybe more)
	_dispatch_async_f_redirect(dq, dc, dq->dq_override);
	_dispatch_introspection_queue_item_complete(dc);
}

DISPATCH_NOINLINE
static void
_dispatch_async_f2(dispatch_queue_t dq, dispatch_continuation_t dc)
{
	// <rdar://problem/24738102&24743140> reserving non barrier width
	// doesn't fail if only the ENQUEUED bit is set (unlike its barrier width
	// equivalent), so we have to check that this thread hasn't enqueued
	// anything ahead of this call or we can break ordering
	if (slowpath(dq->dq_items_tail)) {
		return _dispatch_continuation_push(dq, dc);
	}

	if (slowpath(!_dispatch_queue_try_acquire_async(dq))) {
		return _dispatch_continuation_push(dq, dc);
	}

	return _dispatch_async_f_redirect(dq, dc,
			_dispatch_continuation_get_override_priority(dq, dc));
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_async_f(dispatch_queue_t dq, void *ctxt, dispatch_function_t func,
		pthread_priority_t pp, dispatch_block_flags_t flags)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc_cacheonly();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT;

	if (!fastpath(dc)) {
		return _dispatch_async_f_slow(dq, ctxt, func, pp, flags, dc_flags);
	}

	_dispatch_continuation_init_f(dc, dq, ctxt, func, pp, flags, dc_flags);
	_dispatch_continuation_async2(dq, dc, false);
}

DISPATCH_NOINLINE
void
dispatch_async_f(dispatch_queue_t dq, void *ctxt, dispatch_function_t func)
{
	_dispatch_async_f(dq, ctxt, func, 0, 0);
}

DISPATCH_NOINLINE
void
dispatch_async_enforce_qos_class_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_async_f(dq, ctxt, func, 0, DISPATCH_BLOCK_ENFORCE_QOS_CLASS);
}

#ifdef __BLOCKS__
void
dispatch_async(dispatch_queue_t dq, void (^work)(void))
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT;

	_dispatch_continuation_init(dc, dq, work, 0, 0, dc_flags);
	_dispatch_continuation_async(dq, dc);
}
#endif

#pragma mark -
#pragma mark dispatch_group_async

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_group_async(dispatch_group_t dg, dispatch_queue_t dq,
		dispatch_continuation_t dc)
{
	dispatch_group_enter(dg);
	dc->dc_data = dg;
	_dispatch_continuation_async(dq, dc);
}

DISPATCH_NOINLINE
void
dispatch_group_async_f(dispatch_group_t dg, dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT | DISPATCH_OBJ_GROUP_BIT;

	_dispatch_continuation_init_f(dc, dq, ctxt, func, 0, 0, dc_flags);
	_dispatch_continuation_group_async(dg, dq, dc);
}

#ifdef __BLOCKS__
void
dispatch_group_async(dispatch_group_t dg, dispatch_queue_t dq,
		dispatch_block_t db)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT | DISPATCH_OBJ_GROUP_BIT;

	_dispatch_continuation_init(dc, dq, db, 0, 0, dc_flags);
	_dispatch_continuation_group_async(dg, dq, dc);
}
#endif

#pragma mark -
#pragma mark dispatch_sync / dispatch_barrier_sync recurse and invoke

DISPATCH_NOINLINE
static void
_dispatch_sync_function_invoke_slow(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	voucher_t ov;
	dispatch_thread_frame_s dtf;
	_dispatch_thread_frame_push(&dtf, dq);
	ov = _dispatch_set_priority_and_voucher(0, dq->dq_override_voucher, 0);
	_dispatch_client_callout(ctxt, func);
	_dispatch_perfmon_workitem_inc();
	_dispatch_reset_voucher(ov, 0);
	_dispatch_thread_frame_pop(&dtf);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_sync_function_invoke_inline(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	if (slowpath(dq->dq_override_voucher != DISPATCH_NO_VOUCHER)) {
		return _dispatch_sync_function_invoke_slow(dq, ctxt, func);
	}
	dispatch_thread_frame_s dtf;
	_dispatch_thread_frame_push(&dtf, dq);
	_dispatch_client_callout(ctxt, func);
	_dispatch_perfmon_workitem_inc();
	_dispatch_thread_frame_pop(&dtf);
}

DISPATCH_NOINLINE
static void
_dispatch_sync_function_invoke(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_sync_function_invoke_inline(dq, ctxt, func);
}

void
_dispatch_sync_recurse_invoke(void *ctxt)
{
	dispatch_continuation_t dc = ctxt;
	_dispatch_sync_function_invoke(dc->dc_data, dc->dc_ctxt, dc->dc_func);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_sync_function_recurse(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func, pthread_priority_t pp)
{
	struct dispatch_continuation_s dc = {
		.dc_data = dq,
		.dc_func = func,
		.dc_ctxt = ctxt,
	};
	_dispatch_sync_f(dq->do_targetq, &dc, _dispatch_sync_recurse_invoke, pp);
}

DISPATCH_NOINLINE
static void
_dispatch_non_barrier_sync_f_invoke(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_sync_function_invoke_inline(dq, ctxt, func);
	_dispatch_non_barrier_complete(dq);
}

DISPATCH_NOINLINE
static void
_dispatch_non_barrier_sync_f_recurse(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func, pthread_priority_t pp)
{
	_dispatch_sync_function_recurse(dq, ctxt, func, pp);
	_dispatch_non_barrier_complete(dq);
}

DISPATCH_ALWAYS_INLINE
static void
_dispatch_non_barrier_sync_f_invoke_inline(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func, pthread_priority_t pp)
{
	_dispatch_introspection_non_barrier_sync_begin(dq, func);
	if (slowpath(dq->do_targetq->do_targetq)) {
		return _dispatch_non_barrier_sync_f_recurse(dq, ctxt, func, pp);
	}
	_dispatch_non_barrier_sync_f_invoke(dq, ctxt, func);
}

#pragma mark -
#pragma mark dispatch_barrier_sync

DISPATCH_NOINLINE
static void
_dispatch_barrier_complete(dispatch_queue_t dq)
{
	uint64_t owned = DISPATCH_QUEUE_IN_BARRIER +
			dq->dq_width * DISPATCH_QUEUE_WIDTH_INTERVAL;

	if (slowpath(dq->dq_items_tail)) {
		return _dispatch_try_lock_transfer_or_wakeup(dq);
	}

	if (!fastpath(_dispatch_queue_drain_try_unlock(dq, owned))) {
		// someone enqueued a slow item at the head
		// looping may be its last chance
		return _dispatch_try_lock_transfer_or_wakeup(dq);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_f_recurse(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func, pthread_priority_t pp)
{
	_dispatch_sync_function_recurse(dq, ctxt, func, pp);
	_dispatch_barrier_complete(dq);
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_f_invoke(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_sync_function_invoke_inline(dq, ctxt, func);
	_dispatch_barrier_complete(dq);
}

DISPATCH_ALWAYS_INLINE
static void
_dispatch_barrier_sync_f_invoke_inline(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func, pthread_priority_t pp)
{
	_dispatch_introspection_barrier_sync_begin(dq, func);
	if (slowpath(dq->do_targetq->do_targetq)) {
		return _dispatch_barrier_sync_f_recurse(dq, ctxt, func, pp);
	}
	_dispatch_barrier_sync_f_invoke(dq, ctxt, func);
}

typedef struct dispatch_barrier_sync_context_s {
	struct dispatch_continuation_s dbsc_dc;
	dispatch_thread_frame_s dbsc_dtf;
} *dispatch_barrier_sync_context_t;

static void
_dispatch_barrier_sync_f_slow_invoke(void *ctxt)
{
	dispatch_barrier_sync_context_t dbsc = ctxt;
	dispatch_continuation_t dc = &dbsc->dbsc_dc;
	dispatch_queue_t dq = dc->dc_data;
	dispatch_thread_event_t event = (dispatch_thread_event_t)dc->dc_other;

	dispatch_assert(dq == _dispatch_queue_get_current());
#if DISPATCH_COCOA_COMPAT
	if (slowpath(_dispatch_queue_is_thread_bound(dq))) {
		dispatch_assert(_dispatch_thread_frame_get_current() == NULL);

		// the block runs on the thread the queue is bound to and not
		// on the calling thread, but we mean to see the calling thread
		// dispatch thread frames, so we fake the link, and then undo it
		_dispatch_thread_frame_set_current(&dbsc->dbsc_dtf);
		// The queue is bound to a non-dispatch thread (e.g. main thread)
		_dispatch_continuation_voucher_adopt(dc, DISPATCH_NO_VOUCHER,
				DISPATCH_OBJ_CONSUME_BIT);
		_dispatch_client_callout(dc->dc_ctxt, dc->dc_func);
		os_atomic_store2o(dc, dc_func, NULL, release);
		_dispatch_thread_frame_set_current(NULL);
	}
#endif
	_dispatch_thread_event_signal(event); // release
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_f_slow(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func, pthread_priority_t pp)
{
	if (slowpath(!dq->do_targetq)) {
		// see DISPATCH_ROOT_QUEUE_STATE_INIT_VALUE
		return _dispatch_sync_function_invoke(dq, ctxt, func);
	}

	if (!pp) {
		pp = _dispatch_get_priority();
		pp &= ~_PTHREAD_PRIORITY_FLAGS_MASK;
		pp |= _PTHREAD_PRIORITY_ENFORCE_FLAG;
	}
	dispatch_thread_event_s event;
	_dispatch_thread_event_init(&event);
	struct dispatch_barrier_sync_context_s dbsc = {
		.dbsc_dc = {
			.dc_data = dq,
#if DISPATCH_COCOA_COMPAT
			.dc_func = func,
			.dc_ctxt = ctxt,
#endif
			.dc_other = &event,
		}
	};
#if DISPATCH_COCOA_COMPAT
	// It's preferred to execute synchronous blocks on the current thread
	// due to thread-local side effects, etc. However, blocks submitted
	// to the main thread MUST be run on the main thread
	if (slowpath(_dispatch_queue_is_thread_bound(dq))) {
		// consumed by _dispatch_barrier_sync_f_slow_invoke
		// or in the DISPATCH_COCOA_COMPAT hunk below
		_dispatch_continuation_voucher_set(&dbsc.dbsc_dc, dq, 0);
		// save frame linkage for _dispatch_barrier_sync_f_slow_invoke
		_dispatch_thread_frame_save_state(&dbsc.dbsc_dtf);
		// thread bound queues cannot mutate their target queue hierarchy
		// so it's fine to look now
		_dispatch_introspection_barrier_sync_begin(dq, func);
	}
#endif
	uint32_t th_self = _dispatch_tid_self();
	struct dispatch_continuation_s dbss = {
		.dc_flags = DISPATCH_OBJ_BARRIER_BIT | DISPATCH_OBJ_SYNC_SLOW_BIT,
		.dc_func = _dispatch_barrier_sync_f_slow_invoke,
		.dc_ctxt = &dbsc,
		.dc_data = (void*)(uintptr_t)th_self,
		.dc_priority = pp,
		.dc_other = &event,
		.dc_voucher = DISPATCH_NO_VOUCHER,
	};

	uint64_t dq_state = os_atomic_load2o(dq, dq_state, relaxed);
	if (unlikely(_dq_state_drain_locked_by(dq_state, th_self))) {
		DISPATCH_CLIENT_CRASH(dq, "dispatch_barrier_sync called on queue "
				"already owned by current thread");
	}

	_dispatch_continuation_push_sync_slow(dq, &dbss);
	_dispatch_thread_event_wait(&event); // acquire
	_dispatch_thread_event_destroy(&event);
	if (_dispatch_queue_received_override(dq, pp)) {
		// Ensure that the root queue sees that this thread was overridden.
		// pairs with the _dispatch_wqthread_override_start in
		// _dispatch_continuation_slow_item_signal
		_dispatch_set_defaultpriority_override();
	}

#if DISPATCH_COCOA_COMPAT
	// Queue bound to a non-dispatch thread
	if (dbsc.dbsc_dc.dc_func == NULL) {
		return;
	} else if (dbsc.dbsc_dc.dc_voucher) {
		// this almost never happens, unless a dispatch_sync() onto a thread
		// bound queue went to the slow path at the same time dispatch_main()
		// is called, or the queue is detached from the runloop.
		_voucher_release(dbsc.dbsc_dc.dc_voucher);
	}
#endif

	_dispatch_barrier_sync_f_invoke_inline(dq, ctxt, func, pp);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_barrier_sync_f2(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func, pthread_priority_t pp)
{
	if (slowpath(!_dispatch_queue_try_acquire_barrier_sync(dq))) {
		// global concurrent queues and queues bound to non-dispatch threads
		// always fall into the slow case
		return _dispatch_barrier_sync_f_slow(dq, ctxt, func, pp);
	}
	//
	// TODO: the more correct thing to do would be to set dq_override to the qos
	// of the thread that just acquired the barrier lock here. Unwinding that
	// would slow down the uncontended fastpath however.
	//
	// The chosen tradeoff is that if an enqueue on a lower priority thread
	// contends with this fastpath, this thread may receive a useless override.
	// Improving this requires the override level to be part of the atomic
	// dq_state
	//
	_dispatch_barrier_sync_f_invoke_inline(dq, ctxt, func, pp);
}

DISPATCH_NOINLINE
static void
_dispatch_barrier_sync_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func, pthread_priority_t pp)
{
	_dispatch_barrier_sync_f2(dq, ctxt, func, pp);
}

DISPATCH_NOINLINE
void
dispatch_barrier_sync_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	_dispatch_barrier_sync_f2(dq, ctxt, func, 0);
}

#ifdef __BLOCKS__
DISPATCH_NOINLINE
static void
_dispatch_sync_block_with_private_data(dispatch_queue_t dq,
		void (^work)(void), dispatch_block_flags_t flags)
{
	pthread_priority_t pp = _dispatch_block_get_priority(work);

	flags |= _dispatch_block_get_flags(work);
	if (flags & DISPATCH_BLOCK_HAS_PRIORITY) {
		pthread_priority_t tp = _dispatch_get_priority();
		tp &= ~_PTHREAD_PRIORITY_FLAGS_MASK;
		if (pp < tp) {
			pp = tp | _PTHREAD_PRIORITY_ENFORCE_FLAG;
		} else if (_dispatch_block_sync_should_enforce_qos_class(flags)) {
			pp |= _PTHREAD_PRIORITY_ENFORCE_FLAG;
		}
	}
	// balanced in d_block_sync_invoke or d_block_wait
	if (os_atomic_cmpxchg2o(_dispatch_block_get_data(work),
			dbpd_queue, NULL, dq, relaxed)) {
		_dispatch_retain(dq);
	}
	if (flags & DISPATCH_BLOCK_BARRIER) {
		_dispatch_barrier_sync_f(dq, work, _dispatch_block_sync_invoke, pp);
	} else {
		_dispatch_sync_f(dq, work, _dispatch_block_sync_invoke, pp);
	}
}

void
dispatch_barrier_sync(dispatch_queue_t dq, void (^work)(void))
{
	if (slowpath(_dispatch_block_has_private_data(work))) {
		dispatch_block_flags_t flags = DISPATCH_BLOCK_BARRIER;
		return _dispatch_sync_block_with_private_data(dq, work, flags);
	}
	dispatch_barrier_sync_f(dq, work, _dispatch_Block_invoke(work));
}
#endif

DISPATCH_NOINLINE
void
_dispatch_barrier_trysync_or_async_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	// Use for mutation of queue-/source-internal state only, ignores target
	// queue hierarchy!
	if (!fastpath(_dispatch_queue_try_acquire_barrier_sync(dq))) {
		return _dispatch_barrier_async_detached_f(dq, ctxt, func);
	}
	// skip the recursion because it's about the queue state only
	_dispatch_barrier_sync_f_invoke(dq, ctxt, func);
}

#pragma mark -
#pragma mark dispatch_sync

DISPATCH_NOINLINE
static void
_dispatch_non_barrier_complete(dispatch_queue_t dq)
{
	uint64_t old_state, new_state;

	os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, relaxed, {
		new_state = old_state - DISPATCH_QUEUE_WIDTH_INTERVAL;
		if (_dq_state_is_runnable(new_state)) {
			if (!_dq_state_is_runnable(old_state)) {
				// we're making a FULL -> non FULL transition
				new_state |= DISPATCH_QUEUE_DIRTY;
			}
			if (!_dq_state_drain_locked(new_state)) {
				uint64_t full_width = new_state;
				if (_dq_state_has_pending_barrier(new_state)) {
					full_width -= DISPATCH_QUEUE_PENDING_BARRIER;
					full_width += DISPATCH_QUEUE_WIDTH_INTERVAL;
					full_width += DISPATCH_QUEUE_IN_BARRIER;
				} else {
					full_width += dq->dq_width * DISPATCH_QUEUE_WIDTH_INTERVAL;
					full_width += DISPATCH_QUEUE_IN_BARRIER;
				}
				if ((full_width & DISPATCH_QUEUE_WIDTH_MASK) ==
						DISPATCH_QUEUE_WIDTH_FULL_BIT) {
					new_state = full_width;
					new_state &= ~DISPATCH_QUEUE_DIRTY;
					new_state |= _dispatch_tid_self();
				}
			}
		}
	});

	if (_dq_state_is_in_barrier(new_state)) {
		return _dispatch_try_lock_transfer_or_wakeup(dq);
	}
	if (!_dq_state_is_runnable(old_state)) {
		_dispatch_queue_try_wakeup(dq, new_state, 0);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_sync_f_slow(dispatch_queue_t dq, void *ctxt, dispatch_function_t func,
		pthread_priority_t pp)
{
	dispatch_assert(dq->do_targetq);
	if (!pp) {
		pp = _dispatch_get_priority();
		pp &= ~_PTHREAD_PRIORITY_FLAGS_MASK;
		pp |= _PTHREAD_PRIORITY_ENFORCE_FLAG;
	}
	dispatch_thread_event_s event;
	_dispatch_thread_event_init(&event);
	uint32_t th_self = _dispatch_tid_self();
	struct dispatch_continuation_s dc = {
		.dc_flags = DISPATCH_OBJ_SYNC_SLOW_BIT,
#if DISPATCH_INTROSPECTION
		.dc_func = func,
		.dc_ctxt = ctxt,
#endif
		.dc_data = (void*)(uintptr_t)th_self,
		.dc_other = &event,
		.dc_priority = pp,
		.dc_voucher = DISPATCH_NO_VOUCHER,
	};

	uint64_t dq_state = os_atomic_load2o(dq, dq_state, relaxed);
	if (unlikely(_dq_state_drain_locked_by(dq_state, th_self))) {
		DISPATCH_CLIENT_CRASH(dq, "dispatch_sync called on queue "
				"already owned by current thread");
	}

	_dispatch_continuation_push_sync_slow(dq, &dc);
	_dispatch_thread_event_wait(&event); // acquire
	_dispatch_thread_event_destroy(&event);
	if (_dispatch_queue_received_override(dq, pp)) {
		// Ensure that the root queue sees that this thread was overridden.
		// pairs with the _dispatch_wqthread_override_start in
		// _dispatch_continuation_slow_item_signal
		_dispatch_set_defaultpriority_override();
	}
	_dispatch_non_barrier_sync_f_invoke_inline(dq, ctxt, func, pp);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_sync_f2(dispatch_queue_t dq, void *ctxt, dispatch_function_t func,
		pthread_priority_t pp)
{
	// <rdar://problem/24738102&24743140> reserving non barrier width
	// doesn't fail if only the ENQUEUED bit is set (unlike its barrier width
	// equivalent), so we have to check that this thread hasn't enqueued
	// anything ahead of this call or we can break ordering
	if (slowpath(dq->dq_items_tail)) {
		return _dispatch_sync_f_slow(dq, ctxt, func, pp);
	}
	// concurrent queues do not respect width on sync
	if (slowpath(!_dispatch_queue_try_reserve_sync_width(dq))) {
		return _dispatch_sync_f_slow(dq, ctxt, func, pp);
	}
	_dispatch_non_barrier_sync_f_invoke_inline(dq, ctxt, func, pp);
}

DISPATCH_NOINLINE
static void
_dispatch_sync_f(dispatch_queue_t dq, void *ctxt, dispatch_function_t func,
		pthread_priority_t pp)
{
	if (DISPATCH_QUEUE_USES_REDIRECTION(dq->dq_width)) {
		return _dispatch_sync_f2(dq, ctxt, func, pp);
	}
	return _dispatch_barrier_sync_f(dq, ctxt, func, pp);
}

DISPATCH_NOINLINE
void
dispatch_sync_f(dispatch_queue_t dq, void *ctxt, dispatch_function_t func)
{
	if (DISPATCH_QUEUE_USES_REDIRECTION(dq->dq_width)) {
		return _dispatch_sync_f2(dq, ctxt, func, 0);
	}
	return dispatch_barrier_sync_f(dq, ctxt, func);
}

#ifdef __BLOCKS__
void
dispatch_sync(dispatch_queue_t dq, void (^work)(void))
{
	if (slowpath(_dispatch_block_has_private_data(work))) {
		return _dispatch_sync_block_with_private_data(dq, work, 0);
	}
	dispatch_sync_f(dq, work, _dispatch_Block_invoke(work));
}
#endif

#pragma mark -
#pragma mark dispatch_trysync

struct trysync_context {
	dispatch_queue_t tc_dq;
	void *tc_ctxt;
	dispatch_function_t tc_func;
};

DISPATCH_NOINLINE
static int
_dispatch_trysync_recurse(dispatch_queue_t dq,
		struct trysync_context *tc, bool barrier)
{
	dispatch_queue_t tq = dq->do_targetq;

	if (barrier) {
		if (slowpath(!_dispatch_queue_try_acquire_barrier_sync(dq))) {
			return EWOULDBLOCK;
		}
	} else {
		// <rdar://problem/24743140> check nothing was queued by the current
		// thread ahead of this call. _dispatch_queue_try_reserve_sync_width
		// ignores the ENQUEUED bit which could cause it to miss a barrier_async
		// made by the same thread just before.
		if (slowpath(dq->dq_items_tail)) {
			return EWOULDBLOCK;
		}
		// concurrent queues do not respect width on sync
		if (slowpath(!_dispatch_queue_try_reserve_sync_width(dq))) {
			return EWOULDBLOCK;
		}
	}

	int rc = 0;
	if (_dispatch_queue_cannot_trysync(tq)) {
		_dispatch_queue_atomic_flags_set(dq, DQF_CANNOT_TRYSYNC);
		rc = ENOTSUP;
	} else if (tq->do_targetq) {
		rc = _dispatch_trysync_recurse(tq, tc, tq->dq_width == 1);
		if (rc == ENOTSUP) {
			_dispatch_queue_atomic_flags_set(dq, DQF_CANNOT_TRYSYNC);
		}
	} else {
		dispatch_thread_frame_s dtf;
		_dispatch_thread_frame_push(&dtf, tq);
		_dispatch_sync_function_invoke(tc->tc_dq, tc->tc_ctxt, tc->tc_func);
		_dispatch_thread_frame_pop(&dtf);
	}
	if (barrier) {
		_dispatch_barrier_complete(dq);
	} else {
		_dispatch_non_barrier_complete(dq);
	}
	return rc;
}

DISPATCH_NOINLINE
bool
_dispatch_barrier_trysync_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t f)
{
	if (slowpath(!dq->do_targetq)) {
		_dispatch_sync_function_invoke(dq, ctxt, f);
		return true;
	}
	if (slowpath(_dispatch_queue_cannot_trysync(dq))) {
		return false;
	}
	struct trysync_context tc = {
		.tc_dq = dq,
		.tc_func = f,
		.tc_ctxt = ctxt,
	};
	return _dispatch_trysync_recurse(dq, &tc, true) == 0;
}

DISPATCH_NOINLINE
bool
_dispatch_trysync_f(dispatch_queue_t dq, void *ctxt, dispatch_function_t f)
{
	if (slowpath(!dq->do_targetq)) {
		_dispatch_sync_function_invoke(dq, ctxt, f);
		return true;
	}
	if (slowpath(_dispatch_queue_cannot_trysync(dq))) {
		return false;
	}
	struct trysync_context tc = {
		.tc_dq = dq,
		.tc_func = f,
		.tc_ctxt = ctxt,
	};
	return _dispatch_trysync_recurse(dq, &tc, dq->dq_width == 1) == 0;
}

#pragma mark -
#pragma mark dispatch_after

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_after(dispatch_time_t when, dispatch_queue_t queue,
		void *ctxt, void *handler, bool block)
{
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
	dispatch_assert(ds);

	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	if (block) {
		_dispatch_continuation_init(dc, ds, handler, 0, 0, 0);
	} else {
		_dispatch_continuation_init_f(dc, ds, ctxt, handler, 0, 0, 0);
	}
	// reference `ds` so that it doesn't show up as a leak
	dc->dc_data = ds;
	_dispatch_source_set_event_handler_continuation(ds, dc);
	dispatch_source_set_timer(ds, when, DISPATCH_TIME_FOREVER, leeway);
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
#pragma mark dispatch_queue_wakeup

DISPATCH_NOINLINE
void
_dispatch_queue_wakeup(dispatch_queue_t dq, pthread_priority_t pp,
		dispatch_wakeup_flags_t flags)
{
	dispatch_queue_wakeup_target_t target = DISPATCH_QUEUE_WAKEUP_NONE;

	if (_dispatch_queue_class_probe(dq)) {
		target = DISPATCH_QUEUE_WAKEUP_TARGET;
	}
	if (target) {
		return _dispatch_queue_class_wakeup(dq, pp, flags, target);
	} else if (pp) {
		return _dispatch_queue_class_override_drainer(dq, pp, flags);
	} else if (flags & DISPATCH_WAKEUP_CONSUME) {
		return _dispatch_release_tailcall(dq);
	}
}

#if DISPATCH_COCOA_COMPAT
DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_runloop_handle_is_valid(dispatch_runloop_handle_t handle)
{
#if TARGET_OS_MAC
	return MACH_PORT_VALID(handle);
#elif defined(__linux__)
	return handle >= 0;
#else
#error "runloop support not implemented on this platform"
#endif
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_runloop_handle_t
_dispatch_runloop_queue_get_handle(dispatch_queue_t dq)
{
#if TARGET_OS_MAC
	return ((dispatch_runloop_handle_t)(uintptr_t)dq->do_ctxt);
#elif defined(__linux__)
	// decode: 0 is a valid fd, so offset by 1 to distinguish from NULL
	return ((dispatch_runloop_handle_t)(uintptr_t)dq->do_ctxt) - 1;
#else
#error "runloop support not implemented on this platform"
#endif
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_runloop_queue_set_handle(dispatch_queue_t dq, dispatch_runloop_handle_t handle)
{
#if TARGET_OS_MAC
	dq->do_ctxt = (void *)(uintptr_t)handle;
#elif defined(__linux__)
	// encode: 0 is a valid fd, so offset by 1 to distinguish from NULL
	dq->do_ctxt = (void *)(uintptr_t)(handle + 1);
#else
#error "runloop support not implemented on this platform"
#endif
}
#endif // DISPATCH_COCOA_COMPAT

void
_dispatch_runloop_queue_wakeup(dispatch_queue_t dq, pthread_priority_t pp,
		dispatch_wakeup_flags_t flags)
{
#if DISPATCH_COCOA_COMPAT
	if (slowpath(_dispatch_queue_atomic_flags(dq) & DQF_RELEASED)) {
		// <rdar://problem/14026816>
		return _dispatch_queue_wakeup(dq, pp, flags);
	}

	if (_dispatch_queue_class_probe(dq)) {
		return _dispatch_runloop_queue_poke(dq, pp, flags);
	}

	pp = _dispatch_queue_reset_override_priority(dq, true);
	if (pp) {
		mach_port_t owner = DISPATCH_QUEUE_DRAIN_OWNER(dq);
		if (_dispatch_queue_class_probe(dq)) {
			_dispatch_runloop_queue_poke(dq, pp, flags);
		}
		_dispatch_thread_override_end(owner, dq);
		return;
	}
	if (flags & DISPATCH_WAKEUP_CONSUME) {
		return _dispatch_release_tailcall(dq);
	}
#else
	return _dispatch_queue_wakeup(dq, pp, flags);
#endif
}

void
_dispatch_main_queue_wakeup(dispatch_queue_t dq, pthread_priority_t pp,
		dispatch_wakeup_flags_t flags)
{
#if DISPATCH_COCOA_COMPAT
	if (_dispatch_queue_is_thread_bound(dq)) {
		return _dispatch_runloop_queue_wakeup(dq, pp, flags);
	}
#endif
	return _dispatch_queue_wakeup(dq, pp, flags);
}

void
_dispatch_root_queue_wakeup(dispatch_queue_t dq,
		pthread_priority_t pp DISPATCH_UNUSED,
		dispatch_wakeup_flags_t flags)
{
	if (flags & DISPATCH_WAKEUP_CONSUME) {
		// see _dispatch_queue_push_set_head
		dispatch_assert(flags & DISPATCH_WAKEUP_FLUSH);
	}
	_dispatch_global_queue_poke(dq);
}

#pragma mark -
#pragma mark dispatch root queues poke

#if DISPATCH_COCOA_COMPAT
static inline void
_dispatch_runloop_queue_class_poke(dispatch_queue_t dq)
{
	dispatch_runloop_handle_t handle = _dispatch_runloop_queue_get_handle(dq);
	if (!_dispatch_runloop_handle_is_valid(handle)) {
		return;
	}

#if TARGET_OS_MAC
	mach_port_t mp = handle;
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
#elif defined(__linux__)
	int result;
	do {
		result = eventfd_write(handle, 1);
	} while (result == -1 && errno == EINTR);
	(void)dispatch_assume_zero(result);
#else
#error "runloop support not implemented on this platform"
#endif
}

DISPATCH_NOINLINE
static void
_dispatch_runloop_queue_poke(dispatch_queue_t dq,
		pthread_priority_t pp, dispatch_wakeup_flags_t flags)
{
	// it's not useful to handle WAKEUP_FLUSH because mach_msg() will have
	// a release barrier and that when runloop queues stop being thread bound
	// they have a non optional wake-up to start being a "normal" queue
	// either in _dispatch_runloop_queue_xref_dispose,
	// or in _dispatch_queue_cleanup2() for the main thread.

	if (dq == &_dispatch_main_q) {
		dispatch_once_f(&_dispatch_main_q_handle_pred, dq,
				_dispatch_runloop_queue_handle_init);
	}
	_dispatch_queue_override_priority(dq, /* inout */ &pp, /* inout */ &flags);
	if (flags & DISPATCH_WAKEUP_OVERRIDING) {
		mach_port_t owner = DISPATCH_QUEUE_DRAIN_OWNER(dq);
		_dispatch_thread_override_start(owner, pp, dq);
		if (flags & DISPATCH_WAKEUP_WAS_OVERRIDDEN) {
			_dispatch_thread_override_end(owner, dq);
		}
	}
	_dispatch_runloop_queue_class_poke(dq);
	if (flags & DISPATCH_WAKEUP_CONSUME) {
		return _dispatch_release_tailcall(dq);
	}
}
#endif

DISPATCH_NOINLINE
static void
_dispatch_global_queue_poke_slow(dispatch_queue_t dq, unsigned int n)
{
	dispatch_root_queue_context_t qc = dq->do_ctxt;
	uint32_t i = n;
	int r;

	_dispatch_debug_root_queue(dq, __func__);
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
						_dispatch_worker_thread4, dq, &wh, &gen_cnt);
				(void)dispatch_assume_zero(r);
			} while (--i);
			return;
		}
#endif // DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
#if HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
		if (!dq->dq_priority) {
			r = pthread_workqueue_addthreads_np(qc->dgq_wq_priority,
					qc->dgq_wq_options, (int)i);
			(void)dispatch_assume_zero(r);
			return;
		}
#endif
#if HAVE_PTHREAD_WORKQUEUE_QOS
		r = _pthread_workqueue_addthreads((int)i, dq->dq_priority);
		(void)dispatch_assume_zero(r);
#endif
		return;
	}
#endif // HAVE_PTHREAD_WORKQUEUES
#if DISPATCH_USE_PTHREAD_POOL
	dispatch_pthread_root_queue_context_t pqc = qc->dgq_ctxt;
	if (fastpath(pqc->dpq_thread_mediator.do_vtable)) {
		while (dispatch_semaphore_signal(&pqc->dpq_thread_mediator)) {
			if (!--i) {
				return;
			}
		}
	}
	uint32_t j, t_count;
	// seq_cst with atomic store to tail <rdar://problem/16932833>
	t_count = os_atomic_load2o(qc, dgq_thread_pool_size, ordered);
	do {
		if (!t_count) {
			_dispatch_root_queue_debug("pthread pool is full for root queue: "
					"%p", dq);
			return;
		}
		j = i > t_count ? t_count : i;
	} while (!os_atomic_cmpxchgvw2o(qc, dgq_thread_pool_size, t_count,
			t_count - j, &t_count, acquire));

	pthread_attr_t *attr = &pqc->dpq_thread_attr;
	pthread_t tid, *pthr = &tid;
#if DISPATCH_USE_MGR_THREAD && DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES
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
	} while (--j);
#endif // DISPATCH_USE_PTHREAD_POOL
}

static inline void
_dispatch_global_queue_poke_n(dispatch_queue_t dq, unsigned int n)
{
	if (!_dispatch_queue_class_probe(dq)) {
		return;
	}
#if HAVE_PTHREAD_WORKQUEUES
	dispatch_root_queue_context_t qc = dq->do_ctxt;
	if (
#if DISPATCH_USE_PTHREAD_POOL
			(qc->dgq_kworkqueue != (void*)(~0ul)) &&
#endif
			!os_atomic_cmpxchg2o(qc, dgq_pending, 0, n, relaxed)) {
		_dispatch_root_queue_debug("worker thread request still pending for "
				"global queue: %p", dq);
		return;
	}
#endif // HAVE_PTHREAD_WORKQUEUES
	return 	_dispatch_global_queue_poke_slow(dq, n);
}

static inline void
_dispatch_global_queue_poke(dispatch_queue_t dq)
{
	return _dispatch_global_queue_poke_n(dq, 1);
}

DISPATCH_NOINLINE
void
_dispatch_queue_push_list_slow(dispatch_queue_t dq, unsigned int n)
{
	return _dispatch_global_queue_poke_n(dq, n);
}

#pragma mark -
#pragma mark dispatch_queue_drain

void
_dispatch_continuation_pop(dispatch_object_t dou, dispatch_queue_t dq,
		dispatch_invoke_flags_t flags)
{
	_dispatch_continuation_pop_inline(dou, dq, flags);
}

void
_dispatch_continuation_invoke(dispatch_object_t dou, voucher_t override_voucher,
		dispatch_invoke_flags_t flags)
{
	_dispatch_continuation_invoke_inline(dou, override_voucher, flags);
}

/*
 * Drain comes in 2 flavours (serial/concurrent) and 2 modes
 * (redirecting or not).
 *
 * Serial
 * ~~~~~~
 * Serial drain is about serial queues (width == 1). It doesn't support
 * the redirecting mode, which doesn't make sense, and treats all continuations
 * as barriers. Bookkeeping is minimal in serial flavour, most of the loop
 * is optimized away.
 *
 * Serial drain stops if the width of the queue grows to larger than 1.
 * Going through a serial drain prevents any recursive drain from being
 * redirecting.
 *
 * Concurrent
 * ~~~~~~~~~~
 * When in non-redirecting mode (meaning one of the target queues is serial),
 * non-barriers and barriers alike run in the context of the drain thread.
 * Slow non-barrier items are still all signaled so that they can make progress
 * toward the dispatch_sync() that will serialize them all .
 *
 * In redirecting mode, non-barrier work items are redirected downward.
 *
 * Concurrent drain stops if the width of the queue becomes 1, so that the
 * queue drain moves to the more efficient serial mode.
 */
DISPATCH_ALWAYS_INLINE
static dispatch_queue_t
_dispatch_queue_drain(dispatch_queue_t dq, dispatch_invoke_flags_t flags,
		uint64_t *owned_ptr, struct dispatch_object_s **dc_out,
		bool serial_drain)
{
	dispatch_queue_t orig_tq = dq->do_targetq;
	dispatch_thread_frame_s dtf;
	struct dispatch_object_s *dc = NULL, *next_dc;
	uint64_t owned = *owned_ptr;

	_dispatch_thread_frame_push(&dtf, dq);
	if (_dq_state_is_in_barrier(owned)) {
		// we really own `IN_BARRIER + dq->dq_width * WIDTH_INTERVAL`
		// but width can change while draining barrier work items, so we only
		// convert to `dq->dq_width * WIDTH_INTERVAL` when we drop `IN_BARRIER`
		owned = DISPATCH_QUEUE_IN_BARRIER;
	}

	while (dq->dq_items_tail) {
		dc = _dispatch_queue_head(dq);
		do {
			if (unlikely(DISPATCH_QUEUE_IS_SUSPENDED(dq))) {
				goto out;
			}
			if (unlikely(orig_tq != dq->do_targetq)) {
				goto out;
			}
			if (unlikely(serial_drain != (dq->dq_width == 1))) {
				goto out;
			}
			if (serial_drain || _dispatch_object_is_barrier(dc)) {
				if (!serial_drain && owned != DISPATCH_QUEUE_IN_BARRIER) {
					goto out;
				}
				next_dc = _dispatch_queue_next(dq, dc);
				if (_dispatch_object_is_slow_item(dc)) {
					owned = 0;
					goto out_with_deferred;
				}
			} else {
				if (owned == DISPATCH_QUEUE_IN_BARRIER) {
					// we just ran barrier work items, we have to make their
					// effect visible to other sync work items on other threads
					// that may start coming in after this point, hence the
					// release barrier
					os_atomic_and2o(dq, dq_state, ~owned, release);
					owned = dq->dq_width * DISPATCH_QUEUE_WIDTH_INTERVAL;
				} else if (unlikely(owned == 0)) {
					if (_dispatch_object_is_slow_item(dc)) {
						// sync "readers" don't observe the limit
						_dispatch_queue_reserve_sync_width(dq);
					} else if (!_dispatch_queue_try_acquire_async(dq)) {
						goto out_with_no_width;
					}
					owned = DISPATCH_QUEUE_WIDTH_INTERVAL;
				}

				next_dc = _dispatch_queue_next(dq, dc);
				if (_dispatch_object_is_slow_item(dc)) {
					owned -= DISPATCH_QUEUE_WIDTH_INTERVAL;
					_dispatch_continuation_slow_item_signal(dq, dc);
					continue;
				}

				if (flags & DISPATCH_INVOKE_REDIRECTING_DRAIN) {
					owned -= DISPATCH_QUEUE_WIDTH_INTERVAL;
					_dispatch_continuation_redirect(dq, dc);
					continue;
				}
			}

			_dispatch_continuation_pop_inline(dc, dq, flags);
			_dispatch_perfmon_workitem_inc();
			if (unlikely(dtf.dtf_deferred)) {
				goto out_with_deferred_compute_owned;
			}
		} while ((dc = next_dc));
	}

out:
	if (owned == DISPATCH_QUEUE_IN_BARRIER) {
		// if we're IN_BARRIER we really own the full width too
		owned += dq->dq_width * DISPATCH_QUEUE_WIDTH_INTERVAL;
	}
	if (dc) {
		owned = _dispatch_queue_adjust_owned(dq, owned, dc);
	}
	*owned_ptr = owned;
	_dispatch_thread_frame_pop(&dtf);
	return dc ? dq->do_targetq : NULL;

out_with_no_width:
	*owned_ptr = 0;
	_dispatch_thread_frame_pop(&dtf);
	return NULL;

out_with_deferred_compute_owned:
	if (serial_drain) {
		owned = DISPATCH_QUEUE_IN_BARRIER + DISPATCH_QUEUE_WIDTH_INTERVAL;
	} else {
		if (owned == DISPATCH_QUEUE_IN_BARRIER) {
			// if we're IN_BARRIER we really own the full width too
			owned += dq->dq_width * DISPATCH_QUEUE_WIDTH_INTERVAL;
		}
		if (next_dc) {
			owned = _dispatch_queue_adjust_owned(dq, owned, next_dc);
		}
	}
out_with_deferred:
	*owned_ptr = owned;
	if (unlikely(!dc_out)) {
		DISPATCH_INTERNAL_CRASH(dc,
				"Deferred continuation on source, mach channel or mgr");
	}
	*dc_out = dc;
	_dispatch_thread_frame_pop(&dtf);
	return dq->do_targetq;
}

DISPATCH_NOINLINE
static dispatch_queue_t
_dispatch_queue_concurrent_drain(dispatch_queue_t dq,
		dispatch_invoke_flags_t flags, uint64_t *owned,
		struct dispatch_object_s **dc_ptr)
{
	return _dispatch_queue_drain(dq, flags, owned, dc_ptr, false);
}

DISPATCH_NOINLINE
dispatch_queue_t
_dispatch_queue_serial_drain(dispatch_queue_t dq,
		dispatch_invoke_flags_t flags, uint64_t *owned,
		struct dispatch_object_s **dc_ptr)
{
	flags &= ~(dispatch_invoke_flags_t)DISPATCH_INVOKE_REDIRECTING_DRAIN;
	return _dispatch_queue_drain(dq, flags, owned, dc_ptr, true);
}

#if DISPATCH_COCOA_COMPAT
static void
_dispatch_main_queue_drain(void)
{
	dispatch_queue_t dq = &_dispatch_main_q;
	dispatch_thread_frame_s dtf;

	if (!dq->dq_items_tail) {
		return;
	}

	if (!fastpath(_dispatch_queue_is_thread_bound(dq))) {
		DISPATCH_CLIENT_CRASH(0, "_dispatch_main_queue_callback_4CF called"
				" after dispatch_main()");
	}
	mach_port_t owner = DISPATCH_QUEUE_DRAIN_OWNER(dq);
	if (slowpath(owner != _dispatch_tid_self())) {
		DISPATCH_CLIENT_CRASH(owner, "_dispatch_main_queue_callback_4CF called"
				" from the wrong thread");
	}

	dispatch_once_f(&_dispatch_main_q_handle_pred, dq,
			_dispatch_runloop_queue_handle_init);

	_dispatch_perfmon_start();
	// <rdar://problem/23256682> hide the frame chaining when CFRunLoop
	// drains the main runloop, as this should not be observable that way
	_dispatch_thread_frame_push_and_rebase(&dtf, dq, NULL);

	pthread_priority_t old_pri = _dispatch_get_priority();
	pthread_priority_t old_dp = _dispatch_set_defaultpriority(old_pri, NULL);
	voucher_t voucher = _voucher_copy();

	struct dispatch_object_s *dc, *next_dc, *tail;
	dc = os_mpsc_capture_snapshot(dq, dq_items, &tail);
	do {
		next_dc = os_mpsc_pop_snapshot_head(dc, tail, do_next);
		_dispatch_continuation_pop_inline(dc, dq, DISPATCH_INVOKE_NONE);
		_dispatch_perfmon_workitem_inc();
	} while ((dc = next_dc));

	// runloop based queues use their port for the queue PUBLISH pattern
	// so this raw call to dx_wakeup(0) is valid
	dx_wakeup(dq, 0, 0);
	_dispatch_voucher_debug("main queue restore", voucher);
	_dispatch_reset_defaultpriority(old_dp);
	_dispatch_reset_priority_and_voucher(old_pri, voucher);
	_dispatch_thread_frame_pop(&dtf);
	_dispatch_perfmon_end();
	_dispatch_force_cache_cleanup();
}

static bool
_dispatch_runloop_queue_drain_one(dispatch_queue_t dq)
{
	if (!dq->dq_items_tail) {
		return false;
	}
	dispatch_thread_frame_s dtf;
	_dispatch_perfmon_start();
	_dispatch_thread_frame_push(&dtf, dq);
	pthread_priority_t old_pri = _dispatch_get_priority();
	pthread_priority_t old_dp = _dispatch_set_defaultpriority(old_pri, NULL);
	voucher_t voucher = _voucher_copy();

	struct dispatch_object_s *dc, *next_dc;
	dc = _dispatch_queue_head(dq);
	next_dc = _dispatch_queue_next(dq, dc);
	_dispatch_continuation_pop_inline(dc, dq, DISPATCH_INVOKE_NONE);
	_dispatch_perfmon_workitem_inc();

	if (!next_dc) {
		// runloop based queues use their port for the queue PUBLISH pattern
		// so this raw call to dx_wakeup(0) is valid
		dx_wakeup(dq, 0, 0);
	}

	_dispatch_voucher_debug("runloop queue restore", voucher);
	_dispatch_reset_defaultpriority(old_dp);
	_dispatch_reset_priority_and_voucher(old_pri, voucher);
	_dispatch_thread_frame_pop(&dtf);
	_dispatch_perfmon_end();
	_dispatch_force_cache_cleanup();
	return next_dc;
}
#endif

DISPATCH_NOINLINE
void
_dispatch_try_lock_transfer_or_wakeup(dispatch_queue_t dq)
{
	dispatch_continuation_t dc_tmp, dc_start, dc_end;
	struct dispatch_object_s *dc = NULL;
	uint64_t dq_state, owned;
	size_t count = 0;

	owned  = DISPATCH_QUEUE_IN_BARRIER;
	owned += dq->dq_width * DISPATCH_QUEUE_WIDTH_INTERVAL;
attempt_running_slow_head:
	if (slowpath(dq->dq_items_tail) && !DISPATCH_QUEUE_IS_SUSPENDED(dq)) {
		dc = _dispatch_queue_head(dq);
		if (!_dispatch_object_is_slow_item(dc)) {
			// not a slow item, needs to wake up
		} else if (fastpath(dq->dq_width == 1) ||
				_dispatch_object_is_barrier(dc)) {
			// rdar://problem/8290662 "barrier/writer lock transfer"
			dc_start = dc_end = (dispatch_continuation_t)dc;
			owned = 0;
			count = 1;
			dc = _dispatch_queue_next(dq, dc);
		} else {
			// <rdar://problem/10164594> "reader lock transfer"
			// we must not signal semaphores immediately because our right
			// for dequeuing is granted through holding the full "barrier" width
			// which a signaled work item could relinquish out from our feet
			dc_start = (dispatch_continuation_t)dc;
			do {
				// no check on width here because concurrent queues
				// do not respect width for blocked readers, the thread
				// is already spent anyway
				dc_end = (dispatch_continuation_t)dc;
				owned -= DISPATCH_QUEUE_WIDTH_INTERVAL;
				count++;
				dc = _dispatch_queue_next(dq, dc);
			} while (dc && _dispatch_object_is_slow_non_barrier(dc));
		}

		if (count) {
			_dispatch_queue_drain_transfer_lock(dq, owned, dc_start);
			do {
				// signaled job will release the continuation
				dc_tmp = dc_start;
				dc_start = dc_start->do_next;
				_dispatch_continuation_slow_item_signal(dq, dc_tmp);
			} while (dc_tmp != dc_end);
			return;
		}
	}

	if (dc || dx_metatype(dq) != _DISPATCH_QUEUE_TYPE) {
		// <rdar://problem/23336992> the following wakeup is needed for sources
		// or mach channels: when ds_pending_data is set at the same time
		// as a trysync_f happens, lock transfer code above doesn't know about
		// ds_pending_data or the wakeup logic, but lock transfer is useless
		// for sources and mach channels in the first place.
		owned = _dispatch_queue_adjust_owned(dq, owned, dc);
		dq_state = _dispatch_queue_drain_unlock(dq, owned, NULL);
		return _dispatch_queue_try_wakeup(dq, dq_state, 0);
	} else if (!fastpath(_dispatch_queue_drain_try_unlock(dq, owned))) {
		// someone enqueued a slow item at the head
		// looping may be its last chance
		goto attempt_running_slow_head;
	}
}

void
_dispatch_mgr_queue_drain(void)
{
	const dispatch_invoke_flags_t flags = DISPATCH_INVOKE_MANAGER_DRAIN;
	dispatch_queue_t dq = &_dispatch_mgr_q;
	uint64_t owned = DISPATCH_QUEUE_SERIAL_DRAIN_OWNED;

	if (dq->dq_items_tail) {
		_dispatch_perfmon_start();
		if (slowpath(_dispatch_queue_serial_drain(dq, flags, &owned, NULL))) {
			DISPATCH_INTERNAL_CRASH(0, "Interrupted drain on manager queue");
		}
		_dispatch_voucher_debug("mgr queue clear", NULL);
		_voucher_clear();
		_dispatch_reset_defaultpriority_override();
		_dispatch_perfmon_end();
	}

#if DISPATCH_USE_KEVENT_WORKQUEUE
	if (!_dispatch_kevent_workqueue_enabled)
#endif
	{
		_dispatch_force_cache_cleanup();
	}
}

#pragma mark -
#pragma mark dispatch_queue_invoke

void
_dispatch_queue_drain_deferred_invoke(dispatch_queue_t dq,
		dispatch_invoke_flags_t flags, uint64_t to_unlock,
		struct dispatch_object_s *dc)
{
	if (_dispatch_object_is_slow_item(dc)) {
		dispatch_assert(to_unlock == 0);
		_dispatch_queue_drain_transfer_lock(dq, to_unlock, dc);
		_dispatch_continuation_slow_item_signal(dq, dc);
		return _dispatch_release_tailcall(dq);
	}

	bool should_defer_again = false, should_pend_queue = true;
	uint64_t old_state, new_state;

	if (_dispatch_get_current_queue()->do_targetq) {
		_dispatch_thread_frame_get_current()->dtf_deferred = dc;
		should_defer_again = true;
		should_pend_queue = false;
	}

	if (dq->dq_width > 1) {
		should_pend_queue = false;
	} else if (should_pend_queue) {
		dispatch_assert(to_unlock ==
				DISPATCH_QUEUE_WIDTH_INTERVAL + DISPATCH_QUEUE_IN_BARRIER);
		os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, release,{
			new_state = old_state;
			if (_dq_state_has_waiters(old_state) ||
					_dq_state_is_enqueued(old_state)) {
				os_atomic_rmw_loop_give_up(break);
			}
			new_state += DISPATCH_QUEUE_DRAIN_PENDED;
			new_state -= DISPATCH_QUEUE_IN_BARRIER;
			new_state -= DISPATCH_QUEUE_WIDTH_INTERVAL;
		});
		should_pend_queue = (new_state & DISPATCH_QUEUE_DRAIN_PENDED);
	}

	if (!should_pend_queue) {
		if (to_unlock & DISPATCH_QUEUE_IN_BARRIER) {
			_dispatch_try_lock_transfer_or_wakeup(dq);
			_dispatch_release(dq);
		} else if (to_unlock) {
			uint64_t dq_state = _dispatch_queue_drain_unlock(dq, to_unlock, NULL);
			_dispatch_queue_try_wakeup(dq, dq_state, DISPATCH_WAKEUP_CONSUME);
		} else {
			_dispatch_release(dq);
		}
		dq = NULL;
	}

	if (!should_defer_again) {
		dx_invoke(dc, flags & _DISPATCH_INVOKE_PROPAGATE_MASK);
	}

	if (dq) {
		uint32_t self = _dispatch_tid_self();
		os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, release,{
			new_state = old_state;
			if (!_dq_state_drain_pended(old_state) ||
					_dq_state_drain_owner(old_state) != self) {
				os_atomic_rmw_loop_give_up({
					// We may have been overridden, so inform the root queue
					_dispatch_set_defaultpriority_override();
					return _dispatch_release_tailcall(dq);
				});
			}
			new_state = DISPATCH_QUEUE_DRAIN_UNLOCK_PRESERVE_WAITERS_BIT(new_state);
		});
		if (_dq_state_has_override(old_state)) {
			// Ensure that the root queue sees that this thread was overridden.
			_dispatch_set_defaultpriority_override();
		}
		return dx_invoke(dq, flags | DISPATCH_INVOKE_STEALING);
	}
}

void
_dispatch_queue_finalize_activation(dispatch_queue_t dq)
{
	dispatch_queue_t tq = dq->do_targetq;
	_dispatch_queue_priority_inherit_from_target(dq, tq);
	_dispatch_queue_atomic_flags_set(tq, DQF_TARGETED);
	if (dq->dq_override_voucher == DISPATCH_NO_VOUCHER) {
		voucher_t v = tq->dq_override_voucher;
		if (v != DISPATCH_NO_VOUCHER) {
			if (v) _voucher_retain(v);
			dq->dq_override_voucher = v;
		}
	}
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_t
dispatch_queue_invoke2(dispatch_queue_t dq, dispatch_invoke_flags_t flags,
		uint64_t *owned, struct dispatch_object_s **dc_ptr)
{
	dispatch_queue_t otq = dq->do_targetq;
	dispatch_queue_t cq = _dispatch_queue_get_current();

	if (slowpath(cq != otq)) {
		return otq;
	}
	if (dq->dq_width == 1) {
		return _dispatch_queue_serial_drain(dq, flags, owned, dc_ptr);
	}
	return _dispatch_queue_concurrent_drain(dq, flags, owned, dc_ptr);
}

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
DISPATCH_NOINLINE
void
_dispatch_queue_invoke(dispatch_queue_t dq, dispatch_invoke_flags_t flags)
{
	_dispatch_queue_class_invoke(dq, flags, dispatch_queue_invoke2);
}

#pragma mark -
#pragma mark dispatch_queue_class_wakeup

#if HAVE_PTHREAD_WORKQUEUE_QOS
void
_dispatch_queue_override_invoke(dispatch_continuation_t dc,
		dispatch_invoke_flags_t flags)
{
	dispatch_queue_t old_rq = _dispatch_queue_get_current();
	dispatch_queue_t assumed_rq = dc->dc_other;
	voucher_t ov = DISPATCH_NO_VOUCHER;
	dispatch_object_t dou;

	dou._do = dc->dc_data;
	_dispatch_queue_set_current(assumed_rq);
	flags |= DISPATCH_INVOKE_OVERRIDING;
	if (dc_type(dc) == DISPATCH_CONTINUATION_TYPE(OVERRIDE_STEALING)) {
		flags |= DISPATCH_INVOKE_STEALING;
	} else {
		// balance the fake continuation push in
		// _dispatch_root_queue_push_override
		_dispatch_trace_continuation_pop(assumed_rq, dou._do);
	}
	_dispatch_continuation_pop_forwarded(dc, ov, DISPATCH_OBJ_CONSUME_BIT, {
		if (_dispatch_object_has_vtable(dou._do)) {
			dx_invoke(dou._do, flags);
		} else {
			_dispatch_continuation_invoke_inline(dou, ov, flags);
		}
	});
	_dispatch_queue_set_current(old_rq);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_need_global_root_queue_push_override(dispatch_queue_t rq,
		pthread_priority_t pp)
{
	pthread_priority_t rqp = rq->dq_priority & _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	bool defaultqueue = rq->dq_priority & _PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG;

	if (unlikely(!rqp)) return false;

	pp &= _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	return defaultqueue ? pp && pp != rqp : pp > rqp;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_need_global_root_queue_push_override_stealer(dispatch_queue_t rq,
		pthread_priority_t pp)
{
	pthread_priority_t rqp = rq->dq_priority & _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	bool defaultqueue = rq->dq_priority & _PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG;

	if (unlikely(!rqp)) return false;

	pp &= _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	return defaultqueue || pp > rqp;
}

DISPATCH_NOINLINE
static void
_dispatch_root_queue_push_override(dispatch_queue_t orig_rq,
		dispatch_object_t dou, pthread_priority_t pp)
{
	bool overcommit = orig_rq->dq_priority & _PTHREAD_PRIORITY_OVERCOMMIT_FLAG;
	dispatch_queue_t rq = _dispatch_get_root_queue_for_priority(pp, overcommit);
	dispatch_continuation_t dc = dou._dc;

	if (_dispatch_object_is_redirection(dc)) {
		// no double-wrap is needed, _dispatch_async_redirect_invoke will do
		// the right thing
		dc->dc_func = (void *)orig_rq;
	} else {
		dc = _dispatch_continuation_alloc();
		dc->do_vtable = DC_VTABLE(OVERRIDE_OWNING);
		// fake that we queued `dou` on `orig_rq` for introspection purposes
		_dispatch_trace_continuation_push(orig_rq, dou);
		dc->dc_ctxt = dc;
		dc->dc_other = orig_rq;
		dc->dc_data = dou._do;
		dc->dc_priority = DISPATCH_NO_PRIORITY;
		dc->dc_voucher = DISPATCH_NO_VOUCHER;
	}

	DISPATCH_COMPILER_CAN_ASSUME(dx_type(rq) == DISPATCH_QUEUE_GLOBAL_ROOT_TYPE);
	_dispatch_queue_push_inline(rq, dc, 0, 0);
}

DISPATCH_NOINLINE
static void
_dispatch_root_queue_push_override_stealer(dispatch_queue_t orig_rq,
		dispatch_queue_t dq, pthread_priority_t pp)
{
	bool overcommit = orig_rq->dq_priority & _PTHREAD_PRIORITY_OVERCOMMIT_FLAG;
	dispatch_queue_t rq = _dispatch_get_root_queue_for_priority(pp, overcommit);
	dispatch_continuation_t dc = _dispatch_continuation_alloc();

	dc->do_vtable = DC_VTABLE(OVERRIDE_STEALING);
	_dispatch_retain(dq);
	dc->dc_func = NULL;
	dc->dc_ctxt = dc;
	dc->dc_other = orig_rq;
	dc->dc_data = dq;
	dc->dc_priority = DISPATCH_NO_PRIORITY;
	dc->dc_voucher = DISPATCH_NO_VOUCHER;

	DISPATCH_COMPILER_CAN_ASSUME(dx_type(rq) == DISPATCH_QUEUE_GLOBAL_ROOT_TYPE);
	_dispatch_queue_push_inline(rq, dc, 0, 0);
}

DISPATCH_NOINLINE
static void
_dispatch_queue_class_wakeup_with_override(dispatch_queue_t dq,
		pthread_priority_t pp, dispatch_wakeup_flags_t flags, uint64_t dq_state)
{
	mach_port_t owner = _dq_state_drain_owner(dq_state);
	pthread_priority_t pp2;
	dispatch_queue_t tq;
	bool locked;

	if (owner) {
		int rc = _dispatch_wqthread_override_start_check_owner(owner, pp,
				&dq->dq_state_lock);
		// EPERM means the target of the override is not a work queue thread
		// and could be a thread bound queue such as the main queue.
		// When that happens we must get to that queue and wake it up if we
		// want the override to be appplied and take effect.
		if (rc != EPERM) {
			goto out;
		}
	}

	if (_dq_state_is_suspended(dq_state)) {
		goto out;
	}

	tq = dq->do_targetq;

	if (_dispatch_queue_has_immutable_target(dq)) {
		locked = false;
	} else if (_dispatch_is_in_root_queues_array(tq)) {
		// avoid locking when we recognize the target queue as a global root
		// queue it is gross, but is a very common case. The locking isn't
		// needed because these target queues cannot go away.
		locked = false;
	} else if (_dispatch_queue_sidelock_trylock(dq, pp)) {
		// <rdar://problem/17735825> to traverse the tq chain safely we must
		// lock it to ensure it cannot change
		locked = true;
		tq = dq->do_targetq;
		_dispatch_ktrace1(DISPATCH_PERF_mutable_target, dq);
	} else {
		//
		// Leading to being there, the current thread has:
		// 1. enqueued an object on `dq`
		// 2. raised the dq_override value of `dq`
		// 3. set the HAS_OVERRIDE bit and not seen an owner
		// 4. tried and failed to acquire the side lock
		//
		//
		// The side lock owner can only be one of three things:
		//
		// - The suspend/resume side count code. Besides being unlikely,
		//   it means that at this moment the queue is actually suspended,
		//   which transfers the responsibility of applying the override to
		//   the eventual dispatch_resume().
		//
		// - A dispatch_set_target_queue() call. The fact that we saw no `owner`
		//   means that the trysync it does wasn't being drained when (3)
		//   happened which can only be explained by one of these interleavings:
		//
		//    o `dq` became idle between when the object queued in (1) ran and
		//      the set_target_queue call and we were unlucky enough that our
		//      step (3) happened while this queue was idle. There is no reason
		//		to override anything anymore, the queue drained to completion
		//      while we were preempted, our job is done.
		//
		//    o `dq` is queued but not draining during (1-3), then when we try
		//      to lock at (4) the queue is now draining a set_target_queue.
		//      Since we set HAS_OVERRIDE with a release barrier, the effect of
		//      (2) was visible to the drainer when he acquired the drain lock,
		//      and that guy has applied our override. Our job is done.
		//
		// - Another instance of _dispatch_queue_class_wakeup_with_override(),
		//   which is fine because trylock leaves a hint that we failed our
		//   trylock, causing the tryunlock below to fail and reassess whether
		//   a better override needs to be applied.
		//
		_dispatch_ktrace1(DISPATCH_PERF_mutable_target, dq);
		goto out;
	}

apply_again:
	if (dx_type(tq) == DISPATCH_QUEUE_GLOBAL_ROOT_TYPE) {
		if (_dispatch_need_global_root_queue_push_override_stealer(tq, pp)) {
			_dispatch_root_queue_push_override_stealer(tq, dq, pp);
		}
	} else if (_dispatch_queue_need_override(tq, pp)) {
		dx_wakeup(tq, pp, DISPATCH_WAKEUP_OVERRIDING);
	}
	while (unlikely(locked && !_dispatch_queue_sidelock_tryunlock(dq))) {
		// rdar://problem/24081326
		//
		// Another instance of _dispatch_queue_class_wakeup_with_override()
		// tried to acquire the side lock while we were running, and could have
		// had a better override than ours to apply.
		//
		pp2 = dq->dq_override;
		if (pp2 > pp) {
			pp = pp2;
			// The other instance had a better priority than ours, override
			// our thread, and apply the override that wasn't applied to `dq`
			// because of us.
			goto apply_again;
		}
	}

out:
	if (flags & DISPATCH_WAKEUP_CONSUME) {
		return _dispatch_release_tailcall(dq);
	}
}
#endif // HAVE_PTHREAD_WORKQUEUE_QOS

DISPATCH_NOINLINE
void
_dispatch_queue_class_override_drainer(dispatch_queue_t dq,
		pthread_priority_t pp, dispatch_wakeup_flags_t flags)
{
#if HAVE_PTHREAD_WORKQUEUE_QOS
	uint64_t dq_state, value;

	//
	// Someone is trying to override the last work item of the queue.
	// Do not remember this override on the queue because we know the precise
	// duration the override is required for: until the current drain unlocks.
	//
	// That is why this function only tries to set HAS_OVERRIDE if we can
	// still observe a drainer, and doesn't need to set the DIRTY bit
	// because oq_override wasn't touched and there is no race to resolve
	//
	os_atomic_rmw_loop2o(dq, dq_state, dq_state, value, relaxed, {
		if (!_dq_state_drain_locked(dq_state)) {
			os_atomic_rmw_loop_give_up(break);
		}
		value = dq_state | DISPATCH_QUEUE_HAS_OVERRIDE;
	});
	if (_dq_state_drain_locked(dq_state)) {
		return _dispatch_queue_class_wakeup_with_override(dq, pp,
				flags, dq_state);
	}
#else
	(void)pp;
#endif // HAVE_PTHREAD_WORKQUEUE_QOS
	if (flags & DISPATCH_WAKEUP_CONSUME) {
		return _dispatch_release_tailcall(dq);
	}
}

#if DISPATCH_USE_KEVENT_WORKQUEUE
DISPATCH_NOINLINE
static void
_dispatch_trystash_to_deferred_items(dispatch_queue_t dq, dispatch_object_t dou,
		pthread_priority_t pp, dispatch_deferred_items_t ddi)
{
	dispatch_priority_t old_pp = ddi->ddi_stashed_pp;
	dispatch_queue_t old_dq = ddi->ddi_stashed_dq;
	struct dispatch_object_s *old_dou = ddi->ddi_stashed_dou;
	dispatch_priority_t rq_overcommit;

	rq_overcommit = dq->dq_priority & _PTHREAD_PRIORITY_OVERCOMMIT_FLAG;
	if (likely(!old_pp || rq_overcommit)) {
		ddi->ddi_stashed_dq = dq;
		ddi->ddi_stashed_dou = dou._do;
		ddi->ddi_stashed_pp = (dispatch_priority_t)pp | rq_overcommit |
				_PTHREAD_PRIORITY_PRIORITY_MASK;
		if (likely(!old_pp)) {
			return;
		}
		// push the previously stashed item
		pp = old_pp & _PTHREAD_PRIORITY_QOS_CLASS_MASK;
		dq = old_dq;
		dou._do = old_dou;
	}
	if (_dispatch_need_global_root_queue_push_override(dq, pp)) {
		return _dispatch_root_queue_push_override(dq, dou, pp);
	}
	// bit of cheating: we should really pass `pp` but we know that we are
	// pushing onto a global queue at this point, and we just checked that
	// `pp` doesn't matter.
	DISPATCH_COMPILER_CAN_ASSUME(dx_type(dq) == DISPATCH_QUEUE_GLOBAL_ROOT_TYPE);
	_dispatch_queue_push_inline(dq, dou, 0, 0);
}
#endif

DISPATCH_NOINLINE
static void
_dispatch_queue_push_slow(dispatch_queue_t dq, dispatch_object_t dou,
		pthread_priority_t pp)
{
	dispatch_once_f(&_dispatch_root_queues_pred, NULL,
			_dispatch_root_queues_init_once);
	_dispatch_queue_push(dq, dou, pp);
}

DISPATCH_NOINLINE
void
_dispatch_queue_push(dispatch_queue_t dq, dispatch_object_t dou,
		pthread_priority_t pp)
{
	_dispatch_assert_is_valid_qos_override(pp);
	if (dx_type(dq) == DISPATCH_QUEUE_GLOBAL_ROOT_TYPE) {
#if DISPATCH_USE_KEVENT_WORKQUEUE
		dispatch_deferred_items_t ddi = _dispatch_deferred_items_get();
		if (unlikely(ddi && !(ddi->ddi_stashed_pp &
				(dispatch_priority_t)_PTHREAD_PRIORITY_FLAGS_MASK))) {
			dispatch_assert(_dispatch_root_queues_pred == DLOCK_ONCE_DONE);
			return _dispatch_trystash_to_deferred_items(dq, dou, pp, ddi);
		}
#endif
#if HAVE_PTHREAD_WORKQUEUE_QOS
		// can't use dispatch_once_f() as it would create a frame
		if (unlikely(_dispatch_root_queues_pred != DLOCK_ONCE_DONE)) {
			return _dispatch_queue_push_slow(dq, dou, pp);
		}
		if (_dispatch_need_global_root_queue_push_override(dq, pp)) {
			return _dispatch_root_queue_push_override(dq, dou, pp);
		}
#endif
	}
	_dispatch_queue_push_inline(dq, dou, pp, 0);
}

DISPATCH_NOINLINE
static void
_dispatch_queue_class_wakeup_enqueue(dispatch_queue_t dq, pthread_priority_t pp,
		dispatch_wakeup_flags_t flags, dispatch_queue_wakeup_target_t target)
{
	dispatch_queue_t tq;

	if (flags & (DISPATCH_WAKEUP_OVERRIDING | DISPATCH_WAKEUP_WAS_OVERRIDDEN)) {
		// _dispatch_queue_drain_try_unlock may have reset the override while
		// we were becoming the enqueuer
		_dispatch_queue_reinstate_override_priority(dq, (dispatch_priority_t)pp);
	}
	if (!(flags & DISPATCH_WAKEUP_CONSUME)) {
		_dispatch_retain(dq);
	}
	if (target == DISPATCH_QUEUE_WAKEUP_TARGET) {
		// try_become_enqueuer has no acquire barrier, as the last block
		// of a queue asyncing to that queue is not an uncommon pattern
		// and in that case the acquire is completely useless
		//
		// so instead use a thread fence here when we will read the targetq
		// pointer because that is the only thing that really requires
		// that barrier.
		os_atomic_thread_fence(acquire);
		tq = dq->do_targetq;
	} else {
		dispatch_assert(target == DISPATCH_QUEUE_WAKEUP_MGR);
		tq = &_dispatch_mgr_q;
	}
	return _dispatch_queue_push(tq, dq, pp);
}

DISPATCH_NOINLINE
void
_dispatch_queue_class_wakeup(dispatch_queue_t dq, pthread_priority_t pp,
		dispatch_wakeup_flags_t flags, dispatch_queue_wakeup_target_t target)
{
	uint64_t old_state, new_state, bits = 0;

#if HAVE_PTHREAD_WORKQUEUE_QOS
	_dispatch_queue_override_priority(dq, /* inout */ &pp, /* inout */ &flags);
#endif

	if (flags & DISPATCH_WAKEUP_FLUSH) {
		bits = DISPATCH_QUEUE_DIRTY;
	}
	if (flags & DISPATCH_WAKEUP_OVERRIDING) {
		//
		// Setting the dirty bit here is about forcing callers of
		// _dispatch_queue_drain_try_unlock() to loop again when an override
		// has just been set to close the following race:
		//
		// Drainer (in drain_try_unlokc():
		//    override_reset();
		//    preempted....
		//
		// Enqueuer:
		//    atomic_or(oq_override, override, relaxed);
		//    atomic_or(dq_state, HAS_OVERRIDE, release);
		//
		// Drainer:
		//    ... resumes
		//    successful drain_unlock() and leaks `oq_override`
		//
		bits = DISPATCH_QUEUE_DIRTY | DISPATCH_QUEUE_HAS_OVERRIDE;
	}

	if (flags & DISPATCH_WAKEUP_SLOW_WAITER) {
		uint64_t pending_barrier_width =
				(dq->dq_width - 1) * DISPATCH_QUEUE_WIDTH_INTERVAL;
		uint64_t xor_owner_and_set_full_width_and_in_barrier =
				_dispatch_tid_self() | DISPATCH_QUEUE_WIDTH_FULL_BIT |
				DISPATCH_QUEUE_IN_BARRIER;

#ifdef DLOCK_NOWAITERS_BIT
		bits  |= DLOCK_NOWAITERS_BIT;
#else
		bits  |= DLOCK_WAITERS_BIT;
#endif
		flags ^= DISPATCH_WAKEUP_SLOW_WAITER;
		dispatch_assert(!(flags & DISPATCH_WAKEUP_CONSUME));

		os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, release, {
			new_state = old_state | bits;
			if (_dq_state_drain_pended(old_state)) {
				// same as DISPATCH_QUEUE_DRAIN_UNLOCK_PRESERVE_WAITERS_BIT
				// but we want to be more efficient wrt the WAITERS_BIT
				new_state &= ~DISPATCH_QUEUE_DRAIN_OWNER_MASK;
				new_state &= ~DISPATCH_QUEUE_DRAIN_PENDED;
			}
			if (unlikely(_dq_state_drain_locked(new_state))) {
#ifdef DLOCK_NOWAITERS_BIT
				new_state &= ~(uint64_t)DLOCK_NOWAITERS_BIT;
#endif
			} else if (unlikely(!_dq_state_is_runnable(new_state) ||
					!(flags & DISPATCH_WAKEUP_FLUSH))) {
				// either not runnable, or was not for the first item (26700358)
				// so we should not try to lock and handle overrides instead
			} else if (_dq_state_has_pending_barrier(old_state) ||
					new_state + pending_barrier_width <
					DISPATCH_QUEUE_WIDTH_FULL_BIT) {
				// see _dispatch_queue_drain_try_lock
				new_state &= DISPATCH_QUEUE_DRAIN_PRESERVED_BITS_MASK;
				new_state ^= xor_owner_and_set_full_width_and_in_barrier;
			} else {
				new_state |= DISPATCH_QUEUE_ENQUEUED;
			}
		});
		if ((old_state ^ new_state) & DISPATCH_QUEUE_IN_BARRIER) {
			return _dispatch_try_lock_transfer_or_wakeup(dq);
		}
	} else if (bits) {
		os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, release,{
			new_state = old_state | bits;
			if (likely(_dq_state_should_wakeup(old_state))) {
				new_state |= DISPATCH_QUEUE_ENQUEUED;
			}
		});
	} else {
		os_atomic_rmw_loop2o(dq, dq_state, old_state, new_state, relaxed,{
			new_state = old_state;
			if (likely(_dq_state_should_wakeup(old_state))) {
				new_state |= DISPATCH_QUEUE_ENQUEUED;
			} else {
				os_atomic_rmw_loop_give_up(break);
			}
		});
	}

	if ((old_state ^ new_state) & DISPATCH_QUEUE_ENQUEUED) {
		return _dispatch_queue_class_wakeup_enqueue(dq, pp, flags, target);
	}

#if HAVE_PTHREAD_WORKQUEUE_QOS
	if ((flags & DISPATCH_WAKEUP_OVERRIDING)
			&& target == DISPATCH_QUEUE_WAKEUP_TARGET) {
		return _dispatch_queue_class_wakeup_with_override(dq, pp,
				flags, new_state);
	}
#endif

	if (flags & DISPATCH_WAKEUP_CONSUME) {
		return _dispatch_release_tailcall(dq);
	}
}

#pragma mark -
#pragma mark dispatch_root_queue_drain

DISPATCH_NOINLINE
static bool
_dispatch_root_queue_drain_one_slow(dispatch_queue_t dq)
{
	dispatch_root_queue_context_t qc = dq->do_ctxt;
	struct dispatch_object_s *const mediator = (void *)~0ul;
	bool pending = false, available = true;
	unsigned int sleep_time = DISPATCH_CONTENTION_USLEEP_START;

	do {
		// Spin for a short while in case the contention is temporary -- e.g.
		// when starting up after dispatch_apply, or when executing a few
		// short continuations in a row.
		if (_dispatch_contention_wait_until(dq->dq_items_head != mediator)) {
			goto out;
		}
		// Since we have serious contention, we need to back off.
		if (!pending) {
			// Mark this queue as pending to avoid requests for further threads
			(void)os_atomic_inc2o(qc, dgq_pending, relaxed);
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
	available = false;
out:
	if (pending) {
		(void)os_atomic_dec2o(qc, dgq_pending, relaxed);
	}
	if (!available) {
		_dispatch_global_queue_poke(dq);
	}
	return available;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_root_queue_drain_one2(dispatch_queue_t dq)
{
	// Wait for queue head and tail to be both non-empty or both empty
	bool available; // <rdar://problem/15917893>
	_dispatch_wait_until((dq->dq_items_head != NULL) ==
			(available = (dq->dq_items_tail != NULL)));
	return available;
}

DISPATCH_ALWAYS_INLINE_NDEBUG
static inline struct dispatch_object_s *
_dispatch_root_queue_drain_one(dispatch_queue_t dq)
{
	struct dispatch_object_s *head, *next, *const mediator = (void *)~0ul;

start:
	// The mediator value acts both as a "lock" and a signal
	head = os_atomic_xchg2o(dq, dq_items_head, mediator, relaxed);

	if (slowpath(head == NULL)) {
		// The first xchg on the tail will tell the enqueueing thread that it
		// is safe to blindly write out to the head pointer. A cmpxchg honors
		// the algorithm.
		if (slowpath(!os_atomic_cmpxchg2o(dq, dq_items_head, mediator,
				NULL, relaxed))) {
			goto start;
		}
		if (slowpath(dq->dq_items_tail) && // <rdar://problem/14416349>
				_dispatch_root_queue_drain_one2(dq)) {
			goto start;
		}
		_dispatch_root_queue_debug("no work on global queue: %p", dq);
		return NULL;
	}

	if (slowpath(head == mediator)) {
		// This thread lost the race for ownership of the queue.
		if (fastpath(_dispatch_root_queue_drain_one_slow(dq))) {
			goto start;
		}
		return NULL;
	}

	// Restore the head pointer to a sane value before returning.
	// If 'next' is NULL, then this item _might_ be the last item.
	next = fastpath(head->do_next);

	if (slowpath(!next)) {
		os_atomic_store2o(dq, dq_items_head, NULL, relaxed);
		// 22708742: set tail to NULL with release, so that NULL write to head
		//           above doesn't clobber head from concurrent enqueuer
		if (os_atomic_cmpxchg2o(dq, dq_items_tail, head, NULL, release)) {
			// both head and tail are NULL now
			goto out;
		}
		// There must be a next item now.
		_dispatch_wait_until(next = head->do_next);
	}

	os_atomic_store2o(dq, dq_items_head, next, relaxed);
	_dispatch_global_queue_poke(dq);
out:
	return head;
}

void
_dispatch_root_queue_drain_deferred_item(dispatch_queue_t dq,
		struct dispatch_object_s *dou, pthread_priority_t pp)
{
	struct _dispatch_identity_s di;

	// fake that we queued `dou` on `dq` for introspection purposes
	_dispatch_trace_continuation_push(dq, dou);

	pp = _dispatch_priority_inherit_from_root_queue(pp, dq);
	_dispatch_queue_set_current(dq);
	_dispatch_root_queue_identity_assume(&di, pp);
#if DISPATCH_COCOA_COMPAT
	void *pool = _dispatch_last_resort_autorelease_pool_push();
#endif // DISPATCH_COCOA_COMPAT

	_dispatch_perfmon_start();
	_dispatch_continuation_pop_inline(dou, dq,
			DISPATCH_INVOKE_WORKER_DRAIN | DISPATCH_INVOKE_REDIRECTING_DRAIN);
	_dispatch_perfmon_workitem_inc();
	_dispatch_perfmon_end();

#if DISPATCH_COCOA_COMPAT
	_dispatch_last_resort_autorelease_pool_pop(pool);
#endif // DISPATCH_COCOA_COMPAT
	_dispatch_reset_defaultpriority(di.old_pp);
	_dispatch_queue_set_current(NULL);

	_dispatch_voucher_debug("root queue clear", NULL);
	_dispatch_reset_voucher(NULL, DISPATCH_THREAD_PARK);
}

DISPATCH_NOT_TAIL_CALLED // prevent tailcall (for Instrument DTrace probe)
static void
_dispatch_root_queue_drain(dispatch_queue_t dq, pthread_priority_t pri)
{
#if DISPATCH_DEBUG
	dispatch_queue_t cq;
	if (slowpath(cq = _dispatch_queue_get_current())) {
		DISPATCH_INTERNAL_CRASH(cq, "Premature thread recycling");
	}
#endif
	_dispatch_queue_set_current(dq);
	if (dq->dq_priority) pri = dq->dq_priority;
	pthread_priority_t old_dp = _dispatch_set_defaultpriority(pri, NULL);
#if DISPATCH_COCOA_COMPAT
	void *pool = _dispatch_last_resort_autorelease_pool_push();
#endif // DISPATCH_COCOA_COMPAT

	_dispatch_perfmon_start();
	struct dispatch_object_s *item;
	bool reset = false;
	while ((item = fastpath(_dispatch_root_queue_drain_one(dq)))) {
		if (reset) _dispatch_wqthread_override_reset();
		_dispatch_continuation_pop_inline(item, dq,
				DISPATCH_INVOKE_WORKER_DRAIN|DISPATCH_INVOKE_REDIRECTING_DRAIN);
		_dispatch_perfmon_workitem_inc();
		reset = _dispatch_reset_defaultpriority_override();
	}
	_dispatch_perfmon_end();

#if DISPATCH_COCOA_COMPAT
	_dispatch_last_resort_autorelease_pool_pop(pool);
#endif // DISPATCH_COCOA_COMPAT
	_dispatch_reset_defaultpriority(old_dp);
	_dispatch_queue_set_current(NULL);
}

#pragma mark -
#pragma mark dispatch_worker_thread

#if HAVE_PTHREAD_WORKQUEUES
static void
_dispatch_worker_thread4(void *context)
{
	dispatch_queue_t dq = context;
	dispatch_root_queue_context_t qc = dq->do_ctxt;

	_dispatch_introspection_thread_add();
	int pending = (int)os_atomic_dec2o(qc, dgq_pending, relaxed);
	dispatch_assert(pending >= 0);
	_dispatch_root_queue_drain(dq, _dispatch_get_priority());
	_dispatch_voucher_debug("root queue clear", NULL);
	_dispatch_reset_voucher(NULL, DISPATCH_THREAD_PARK);
}

#if HAVE_PTHREAD_WORKQUEUE_QOS
static void
_dispatch_worker_thread3(pthread_priority_t pp)
{
	bool overcommit = pp & _PTHREAD_PRIORITY_OVERCOMMIT_FLAG;
	dispatch_queue_t dq;
	pp &= _PTHREAD_PRIORITY_OVERCOMMIT_FLAG | ~_PTHREAD_PRIORITY_FLAGS_MASK;
	_dispatch_thread_setspecific(dispatch_priority_key, (void *)(uintptr_t)pp);
	dq = _dispatch_get_root_queue_for_priority(pp, overcommit);
	return _dispatch_worker_thread4(dq);
}
#endif // HAVE_PTHREAD_WORKQUEUE_QOS

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

	return _dispatch_worker_thread4(dq);
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

	if (pqc->dpq_observer_hooks.queue_will_execute) {
		_dispatch_set_pthread_root_queue_observer_hooks(
				&pqc->dpq_observer_hooks);
	}
	if (pqc->dpq_thread_configure) {
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

	const int64_t timeout = 5ull * NSEC_PER_SEC;
	pthread_priority_t old_pri = _dispatch_get_priority();
	do {
		_dispatch_root_queue_drain(dq, old_pri);
		_dispatch_reset_priority_and_voucher(old_pri, NULL);
	} while (dispatch_semaphore_wait(&pqc->dpq_thread_mediator,
			dispatch_time(0, timeout)) == 0);

	(void)os_atomic_inc2o(qc, dgq_thread_pool_size, release);
	_dispatch_global_queue_poke(dq);
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
		return DISPATCH_BAD_INPUT;
	}
	dqs = sizeof(struct dispatch_queue_s) - DISPATCH_QUEUE_CACHELINE_PAD;
	dq = _dispatch_alloc(DISPATCH_VTABLE(queue_runloop), dqs);
	_dispatch_queue_init(dq, DQF_THREAD_BOUND | DQF_CANNOT_TRYSYNC, 1, false);
	dq->do_targetq = _dispatch_get_root_queue(_DISPATCH_QOS_CLASS_DEFAULT,true);
	dq->dq_label = label ? label : "runloop-queue"; // no-copy contract
	_dispatch_runloop_queue_handle_init(dq);
	_dispatch_queue_set_bound_thread(dq);
	_dispatch_object_debug(dq, "%s", __func__);
	return _dispatch_introspection_queue_create(dq);
}

void
_dispatch_runloop_queue_xref_dispose(dispatch_queue_t dq)
{
	_dispatch_object_debug(dq, "%s", __func__);

	pthread_priority_t pp = _dispatch_queue_reset_override_priority(dq, true);
	_dispatch_queue_clear_bound_thread(dq);
	dx_wakeup(dq, pp, DISPATCH_WAKEUP_FLUSH);
	if (pp) _dispatch_thread_override_end(DISPATCH_QUEUE_DRAIN_OWNER(dq), dq);
}

void
_dispatch_runloop_queue_dispose(dispatch_queue_t dq)
{
	_dispatch_object_debug(dq, "%s", __func__);
	_dispatch_introspection_queue_dispose(dq);
	_dispatch_runloop_queue_handle_dispose(dq);
	_dispatch_queue_destroy(dq);
}

bool
_dispatch_runloop_root_queue_perform_4CF(dispatch_queue_t dq)
{
	if (slowpath(dq->do_vtable != DISPATCH_VTABLE(queue_runloop))) {
		DISPATCH_CLIENT_CRASH(dq->do_vtable, "Not a runloop queue");
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
		DISPATCH_CLIENT_CRASH(dq->do_vtable, "Not a runloop queue");
	}
	_dispatch_runloop_queue_wakeup(dq, 0, false);
}

dispatch_runloop_handle_t
_dispatch_runloop_root_queue_get_port_4CF(dispatch_queue_t dq)
{
	if (slowpath(dq->do_vtable != DISPATCH_VTABLE(queue_runloop))) {
		DISPATCH_CLIENT_CRASH(dq->do_vtable, "Not a runloop queue");
	}
	return _dispatch_runloop_queue_get_handle(dq);
}

static void
_dispatch_runloop_queue_handle_init(void *ctxt)
{
	dispatch_queue_t dq = (dispatch_queue_t)ctxt;
	dispatch_runloop_handle_t handle;

	_dispatch_fork_becomes_unsafe();

#if TARGET_OS_MAC
	mach_port_t mp;
	kern_return_t kr;
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
	handle = mp;
#elif defined(__linux__)
	int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fd == -1) {
		int err = errno;
		switch (err) {
		case EMFILE:
			DISPATCH_CLIENT_CRASH(err, "eventfd() failure: "
					"process is out of file descriptors");
			break;
		case ENFILE:
			DISPATCH_CLIENT_CRASH(err, "eventfd() failure: "
					"system is out of file descriptors");
			break;
		case ENOMEM:
			DISPATCH_CLIENT_CRASH(err, "eventfd() failure: "
					"kernel is out of memory");
			break;
		default:
			DISPATCH_INTERNAL_CRASH(err, "eventfd() failure");
			break;
		}
	}
	handle = fd;
#else
#error "runloop support not implemented on this platform"
#endif
	_dispatch_runloop_queue_set_handle(dq, handle);

	_dispatch_program_is_probably_callback_driven = true;
}

static void
_dispatch_runloop_queue_handle_dispose(dispatch_queue_t dq)
{
	dispatch_runloop_handle_t handle = _dispatch_runloop_queue_get_handle(dq);
	if (!_dispatch_runloop_handle_is_valid(handle)) {
		return;
	}
	dq->do_ctxt = NULL;
#if TARGET_OS_MAC
	mach_port_t mp = handle;
	kern_return_t kr = mach_port_deallocate(mach_task_self(), mp);
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
	kr = mach_port_mod_refs(mach_task_self(), mp, MACH_PORT_RIGHT_RECEIVE, -1);
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
#elif defined(__linux__)
	int rc = close(handle);
	(void)dispatch_assume_zero(rc);
#else
#error "runloop support not implemented on this platform"
#endif
}

#pragma mark -
#pragma mark dispatch_main_queue

dispatch_runloop_handle_t
_dispatch_get_main_queue_handle_4CF(void)
{
	dispatch_queue_t dq = &_dispatch_main_q;
	dispatch_once_f(&_dispatch_main_q_handle_pred, dq,
			_dispatch_runloop_queue_handle_init);
	return _dispatch_runloop_queue_get_handle(dq);
}

#if TARGET_OS_MAC
dispatch_runloop_handle_t
_dispatch_get_main_queue_port_4CF(void)
{
	return _dispatch_get_main_queue_handle_4CF();
}
#endif

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
_dispatch_main_queue_callback_4CF(
#if TARGET_OS_MAC
		mach_msg_header_t *_Null_unspecified msg
#else
		void *ignored
#endif
		DISPATCH_UNUSED)
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
	dispatch_once_f(&_dispatch_root_queues_pred, NULL,
		_dispatch_root_queues_init_once);

#if HAVE_PTHREAD_MAIN_NP
	if (pthread_main_np()) {
#endif
		_dispatch_object_debug(&_dispatch_main_q, "%s", __func__);
		_dispatch_program_is_probably_callback_driven = true;
		_dispatch_ktrace0(ARIADNE_ENTER_DISPATCH_MAIN_CODE);
#ifdef __linux__
		// On Linux, if the main thread calls pthread_exit, the process becomes a zombie.
		// To avoid that, just before calling pthread_exit we register a TSD destructor
		// that will call _dispatch_sig_thread -- thus capturing the main thread in sigsuspend.
		// This relies on an implementation detail (currently true in glibc) that TSD destructors
		// will be called in the order of creation to cause all the TSD cleanup functions to
		// run before the thread becomes trapped in sigsuspend.
		pthread_key_t dispatch_main_key;
		pthread_key_create(&dispatch_main_key, _dispatch_sig_thread);
		pthread_setspecific(dispatch_main_key, &dispatch_main_key);
#endif
		pthread_exit(NULL);
		DISPATCH_INTERNAL_CRASH(errno, "pthread_exit() returned");
#if HAVE_PTHREAD_MAIN_NP
	}
	DISPATCH_CLIENT_CRASH(0, "dispatch_main() must be called on the main thread");
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
	_dispatch_queue_clear_bound_thread(dq);

	// <rdar://problem/22623242>
	// Here is what happens when both this cleanup happens because of
	// dispatch_main() being called, and a concurrent enqueuer makes the queue
	// non empty.
	//
	// _dispatch_queue_cleanup2:
	//     atomic_and(dq_is_thread_bound, ~DQF_THREAD_BOUND, relaxed);
	//     maximal_barrier();
	//     if (load(dq_items_tail, seq_cst)) {
	//         // do the wake up the normal serial queue way
	//     } else {
	//         // do no wake up  <----
	//     }
	//
	// enqueuer:
	//     store(dq_items_tail, new_tail, release);
	//     if (load(dq_is_thread_bound, relaxed)) {
	//         // do the wake up the runloop way <----
	//     } else {
	//         // do the wake up the normal serial way
	//     }
	//
	// what would be bad is to take both paths marked <---- because the queue
	// wouldn't be woken up until the next time it's used (which may never
	// happen)
	//
	// An enqueuer that speculates the load of the old value of thread_bound
	// and then does the store may wake up the main queue the runloop way.
	// But then, the cleanup thread will see that store because the load
	// of dq_items_tail is sequentially consistent, and we have just thrown away
	// our pipeline.
	//
	// By the time cleanup2() is out of the maximally synchronizing barrier,
	// no other thread can speculate the wrong load anymore, and both cleanup2()
	// and a concurrent enqueuer would treat the queue in the standard non
	// thread bound way

	_dispatch_queue_atomic_flags_clear(dq,
			DQF_THREAD_BOUND | DQF_CANNOT_TRYSYNC);
	os_atomic_maximally_synchronizing_barrier();
	// no need to drop the override, the thread will die anyway
	// the barrier above includes an acquire, so it's ok to do this raw
	// call to dx_wakeup(0)
	dx_wakeup(dq, 0, 0);

	// overload the "probably" variable to mean that dispatch_main() or
	// similar non-POSIX API was called
	// this has to run before the DISPATCH_COCOA_COMPAT below
	// See dispatch_main for call to _dispatch_sig_thread on linux.
#ifndef __linux__
	if (_dispatch_program_is_probably_callback_driven) {
		_dispatch_barrier_async_detached_f(_dispatch_get_root_queue(
				_DISPATCH_QOS_CLASS_DEFAULT, true), NULL, _dispatch_sig_thread);
		sleep(1); // workaround 6778970
	}
#endif

#if DISPATCH_COCOA_COMPAT
	dispatch_once_f(&_dispatch_main_q_handle_pred, dq,
			_dispatch_runloop_queue_handle_init);
	_dispatch_runloop_queue_handle_dispose(dq);
#endif
}

static void
_dispatch_queue_cleanup(void *ctxt)
{
	if (ctxt == &_dispatch_main_q) {
		return _dispatch_queue_cleanup2();
	}
	// POSIX defines that destructors are only called if 'ctxt' is non-null
	DISPATCH_INTERNAL_CRASH(ctxt,
			"Premature thread exit while a dispatch queue is running");
}

static void
_dispatch_deferred_items_cleanup(void *ctxt)
{
	// POSIX defines that destructors are only called if 'ctxt' is non-null
	DISPATCH_INTERNAL_CRASH(ctxt,
			"Premature thread exit with unhandled deferred items");
}

static void
_dispatch_frame_cleanup(void *ctxt)
{
	// POSIX defines that destructors are only called if 'ctxt' is non-null
	DISPATCH_INTERNAL_CRASH(ctxt,
			"Premature thread exit while a dispatch frame is active");
}

static void
_dispatch_context_cleanup(void *ctxt)
{
	// POSIX defines that destructors are only called if 'ctxt' is non-null
	DISPATCH_INTERNAL_CRASH(ctxt,
			"Premature thread exit while a dispatch context is set");
}
