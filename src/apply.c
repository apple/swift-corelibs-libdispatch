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

typedef void (*dispatch_apply_function_t)(void *, size_t);
static char const * const _dispatch_apply_key = "apply";

#define DISPATCH_APPLY_INVOKE_REDIRECT 0x1
#define DISPATCH_APPLY_INVOKE_WAIT     0x2

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_apply_invoke2(void *ctxt, long invoke_flags)
{
	dispatch_apply_t da = (dispatch_apply_t)ctxt;
	size_t const iter = da->da_iterations;
	size_t idx, done = 0;

	idx = os_atomic_inc_orig2o(da, da_index, acquire);
	if (!fastpath(idx < iter)) goto out;

	// da_dc is only safe to access once the 'index lock' has been acquired
	dispatch_apply_function_t const func = (void *)da->da_dc->dc_func;
	void *const da_ctxt = da->da_dc->dc_ctxt;
	dispatch_queue_t dq = da->da_dc->dc_data;

	_dispatch_perfmon_workitem_dec(); // this unit executes many items

	// Handle nested dispatch_apply rdar://problem/9294578
	dispatch_thread_context_s apply_ctxt = {
		.dtc_key = _dispatch_apply_key,
		.dtc_apply_nesting = da->da_nested,
	};
	_dispatch_thread_context_push(&apply_ctxt);

	dispatch_thread_frame_s dtf;
	pthread_priority_t old_dp;
	if (invoke_flags & DISPATCH_APPLY_INVOKE_REDIRECT) {
		_dispatch_thread_frame_push(&dtf, dq);
		old_dp = _dispatch_set_defaultpriority(dq->dq_priority, NULL);
	}
	dispatch_invoke_flags_t flags = da->da_flags;

	// Striding is the responsibility of the caller.
	do {
		dispatch_invoke_with_autoreleasepool(flags, {
			_dispatch_client_callout2(da_ctxt, idx, func);
			_dispatch_perfmon_workitem_inc();
			done++;
			idx = os_atomic_inc_orig2o(da, da_index, relaxed);
		});
	} while (fastpath(idx < iter));

	if (invoke_flags & DISPATCH_APPLY_INVOKE_REDIRECT) {
		_dispatch_reset_defaultpriority(old_dp);
		_dispatch_thread_frame_pop(&dtf);
	}

	_dispatch_thread_context_pop(&apply_ctxt);

	// The thread that finished the last workitem wakes up the possibly waiting
	// thread that called dispatch_apply. They could be one and the same.
	if (!os_atomic_sub2o(da, da_todo, done, release)) {
		_dispatch_thread_event_signal(&da->da_event);
	}
out:
	if (invoke_flags & DISPATCH_APPLY_INVOKE_WAIT) {
		_dispatch_thread_event_wait(&da->da_event);
		_dispatch_thread_event_destroy(&da->da_event);
	}
	if (os_atomic_dec2o(da, da_thr_cnt, release) == 0) {
		_dispatch_continuation_free((dispatch_continuation_t)da);
	}
}

DISPATCH_NOINLINE
void
_dispatch_apply_invoke(void *ctxt)
{
	_dispatch_apply_invoke2(ctxt, 0);
}

DISPATCH_NOINLINE
static void
_dispatch_apply_invoke_and_wait(void *ctxt)
{
	_dispatch_apply_invoke2(ctxt, DISPATCH_APPLY_INVOKE_WAIT);
	_dispatch_perfmon_workitem_inc();
}

DISPATCH_NOINLINE
void
_dispatch_apply_redirect_invoke(void *ctxt)
{
	_dispatch_apply_invoke2(ctxt, DISPATCH_APPLY_INVOKE_REDIRECT);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_invoke_flags_t
_dispatch_apply_autorelease_frequency(dispatch_queue_t dq)
{
	dispatch_invoke_flags_t qaf = 0;

	while (dq && !qaf) {
		qaf = _dispatch_queue_autorelease_frequency(dq);
		dq = slowpath(dq->do_targetq);
	}
	return qaf;
}

DISPATCH_NOINLINE
static void
_dispatch_apply_serial(void *ctxt)
{
	dispatch_apply_t da = (dispatch_apply_t)ctxt;
	dispatch_continuation_t dc = da->da_dc;
	size_t const iter = da->da_iterations;
	dispatch_invoke_flags_t flags;
	size_t idx = 0;

	_dispatch_perfmon_workitem_dec(); // this unit executes many items
	flags = _dispatch_apply_autorelease_frequency(dc->dc_data);
	do {
		dispatch_invoke_with_autoreleasepool(flags, {
			_dispatch_client_callout2(dc->dc_ctxt, idx, (void*)dc->dc_func);
			_dispatch_perfmon_workitem_inc();
		});
	} while (++idx < iter);

	_dispatch_continuation_free((dispatch_continuation_t)da);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_apply_f2(dispatch_queue_t dq, dispatch_apply_t da,
		dispatch_function_t func)
{
	uint32_t i = 0;
	dispatch_continuation_t head = NULL, tail = NULL;

	// The current thread does not need a continuation
	uint32_t continuation_cnt = da->da_thr_cnt - 1;

	dispatch_assert(continuation_cnt);

	for (i = 0; i < continuation_cnt; i++) {
		dispatch_continuation_t next = _dispatch_continuation_alloc();
		uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT;

		_dispatch_continuation_init_f(next, dq, da, func, 0, 0, dc_flags);
		next->do_next = head;
		head = next;

		if (!tail) {
			tail = next;
		}
	}

	_dispatch_thread_event_init(&da->da_event);

	_dispatch_queue_push_list(dq, head, tail, head->dc_priority,
			continuation_cnt);
	// Call the first element directly
	_dispatch_apply_invoke_and_wait(da);
}

DISPATCH_NOINLINE
static void
_dispatch_apply_redirect(void *ctxt)
{
	dispatch_apply_t da = (dispatch_apply_t)ctxt;
	uint32_t da_width = da->da_thr_cnt - 1;
	dispatch_queue_t dq = da->da_dc->dc_data, rq = dq, tq;

	do {
		uint32_t width = _dispatch_queue_try_reserve_apply_width(rq, da_width);

		if (slowpath(da_width > width)) {
			uint32_t excess = da_width - width;
			for (tq = dq; tq != rq; tq = tq->do_targetq) {
				_dispatch_queue_relinquish_width(tq, excess);
			}
			da_width -= excess;
			if (slowpath(!da_width)) {
				return _dispatch_apply_serial(da);
			}
			da->da_thr_cnt -= excess;
		}
		if (!da->da_flags) {
			// find first queue in descending target queue order that has
			// an autorelease frequency set, and use that as the frequency for
			// this continuation.
			da->da_flags = _dispatch_queue_autorelease_frequency(dq);
		}
		rq = rq->do_targetq;
	} while (slowpath(rq->do_targetq));
	_dispatch_apply_f2(rq, da, _dispatch_apply_redirect_invoke);
	do {
		_dispatch_queue_relinquish_width(dq, da_width);
		dq = dq->do_targetq;
	} while (slowpath(dq->do_targetq));
}

#define DISPATCH_APPLY_MAX UINT16_MAX // must be < sqrt(SIZE_MAX)

DISPATCH_NOINLINE
void
dispatch_apply_f(size_t iterations, dispatch_queue_t dq, void *ctxt,
		void (*func)(void *, size_t))
{
	if (slowpath(iterations == 0)) {
		return;
	}
	uint32_t thr_cnt = dispatch_hw_config(active_cpus);
	dispatch_thread_context_t dtctxt = _dispatch_thread_context_find(_dispatch_apply_key);
	size_t nested = dtctxt ? dtctxt->dtc_apply_nesting : 0;
	dispatch_queue_t old_dq = _dispatch_queue_get_current();

	if (!slowpath(nested)) {
		nested = iterations;
	} else {
		thr_cnt = nested < thr_cnt ? thr_cnt / nested : 1;
		nested = nested < DISPATCH_APPLY_MAX && iterations < DISPATCH_APPLY_MAX
				? nested * iterations : DISPATCH_APPLY_MAX;
	}
	if (iterations < thr_cnt) {
		thr_cnt = (uint32_t)iterations;
	}
	if (slowpath(dq == DISPATCH_APPLY_CURRENT_ROOT_QUEUE)) {
		dq = old_dq ? old_dq : _dispatch_get_root_queue(
				_DISPATCH_QOS_CLASS_DEFAULT, false);
		while (slowpath(dq->do_targetq)) {
			dq = dq->do_targetq;
		}
	}
	struct dispatch_continuation_s dc = {
		.dc_func = (void*)func,
		.dc_ctxt = ctxt,
		.dc_data = dq,
	};
	dispatch_apply_t da = (typeof(da))_dispatch_continuation_alloc();
	da->da_index = 0;
	da->da_todo = iterations;
	da->da_iterations = iterations;
	da->da_nested = nested;
	da->da_thr_cnt = thr_cnt;
	da->da_dc = &dc;
	da->da_flags = 0;

	if (slowpath(dq->dq_width == 1) || slowpath(thr_cnt <= 1)) {
		return dispatch_sync_f(dq, da, _dispatch_apply_serial);
	}
	if (slowpath(dq->do_targetq)) {
		if (slowpath(dq == old_dq)) {
			return dispatch_sync_f(dq, da, _dispatch_apply_serial);
		} else {
			return dispatch_sync_f(dq, da, _dispatch_apply_redirect);
		}
	}

	dispatch_thread_frame_s dtf;
	_dispatch_thread_frame_push(&dtf, dq);
	_dispatch_apply_f2(dq, da, _dispatch_apply_invoke);
	_dispatch_thread_frame_pop(&dtf);
}

#ifdef __BLOCKS__
void
dispatch_apply(size_t iterations, dispatch_queue_t dq, void (^work)(size_t))
{
	dispatch_apply_f(iterations, dq, work,
			(dispatch_apply_function_t)_dispatch_Block_invoke(work));
}
#endif

#if 0
#ifdef __BLOCKS__
void
dispatch_stride(size_t offset, size_t stride, size_t iterations,
		dispatch_queue_t dq, void (^work)(size_t))
{
	dispatch_stride_f(offset, stride, iterations, dq, work,
			(dispatch_apply_function_t)_dispatch_Block_invoke(work));
}
#endif

DISPATCH_NOINLINE
void
dispatch_stride_f(size_t offset, size_t stride, size_t iterations,
		dispatch_queue_t dq, void *ctxt, void (*func)(void *, size_t))
{
	if (stride == 0) {
		stride = 1;
	}
	dispatch_apply(iterations / stride, queue, ^(size_t idx) {
		size_t i = idx * stride + offset;
		size_t stop = i + stride;
		do {
			func(ctxt, i++);
		} while (i < stop);
	});

	dispatch_sync(queue, ^{
		size_t i;
		for (i = iterations - (iterations % stride); i < iterations; i++) {
			func(ctxt, i + offset);
		}
	});
}
#endif
