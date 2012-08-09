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

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_apply_invoke(void *ctxt)
{
	dispatch_apply_t da = ctxt;
	size_t const iter = da->da_iterations;
	typeof(da->da_func) const func = da->da_func;
	void *const da_ctxt = da->da_ctxt;
	size_t idx, done = 0;

	_dispatch_workitem_dec(); // this unit executes many items

	// Make nested dispatch_apply fall into serial case rdar://problem/9294578
	_dispatch_thread_setspecific(dispatch_apply_key, (void*)~0ul);
	// Striding is the responsibility of the caller.
	while (fastpath((idx = dispatch_atomic_inc2o(da, da_index) - 1) < iter)) {
		_dispatch_client_callout2(da_ctxt, idx, func);
		_dispatch_workitem_inc();
		done++;
	}
	_dispatch_thread_setspecific(dispatch_apply_key, NULL);

	dispatch_atomic_release_barrier();

	// The thread that finished the last workitem wakes up the (possibly waiting)
	// thread that called dispatch_apply. They could be one and the same.
	if (done && (dispatch_atomic_add2o(da, da_done, done) == iter)) {
		_dispatch_thread_semaphore_signal(da->da_sema);
	}

	if (dispatch_atomic_dec2o(da, da_thr_cnt) == 0) {
		_dispatch_continuation_free((dispatch_continuation_t)da);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_apply2(void *ctxt)
{
	_dispatch_apply_invoke(ctxt);
}

static void
_dispatch_apply3(void *ctxt)
{
	dispatch_apply_t da = ctxt;
	dispatch_queue_t old_dq = _dispatch_thread_getspecific(dispatch_queue_key);

	_dispatch_thread_setspecific(dispatch_queue_key, da->da_queue);
	_dispatch_apply_invoke(ctxt);
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);
}

static void
_dispatch_apply_serial(void *ctxt)
{
	dispatch_apply_t da = ctxt;
	size_t idx = 0;

	_dispatch_workitem_dec(); // this unit executes many items
	do {
		_dispatch_client_callout2(da->da_ctxt, idx, da->da_func);
		_dispatch_workitem_inc();
	} while (++idx < da->da_iterations);

	_dispatch_continuation_free((dispatch_continuation_t)da);
}

// 64 threads should be good enough for the short to mid term
#define DISPATCH_APPLY_MAX_CPUS 64

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
		next->do_vtable = (void *)DISPATCH_OBJ_ASYNC_BIT;
		next->dc_func = func;
		next->dc_ctxt = da;

		next->do_next = head;
		head = next;

		if (!tail) {
			tail = next;
		}
	}

	_dispatch_thread_semaphore_t sema = _dispatch_get_thread_semaphore();
	da->da_sema = sema;

	_dispatch_queue_push_list(dq, head, tail, continuation_cnt);
	// Call the first element directly
	_dispatch_apply2(da);
	_dispatch_workitem_inc();

	_dispatch_thread_semaphore_wait(sema);
	_dispatch_put_thread_semaphore(sema);

}

static void
_dispatch_apply_redirect(void *ctxt)
{
	dispatch_apply_t da = ctxt;
	uint32_t da_width = 2 * (da->da_thr_cnt - 1);
	dispatch_queue_t dq = da->da_queue, rq = dq, tq;

	do {
		uint32_t running = dispatch_atomic_add2o(rq, dq_running, da_width);
		uint32_t width = rq->dq_width;
		if (slowpath(running > width)) {
			uint32_t excess = width > 1 ? running - width : da_width;
			for (tq = dq; 1; tq = tq->do_targetq) {
				(void)dispatch_atomic_sub2o(tq, dq_running, excess);
				if (tq == rq) {
					break;
				}
			}
			da_width -= excess;
			if (slowpath(!da_width)) {
				return _dispatch_apply_serial(da);
			}
			da->da_thr_cnt -= excess / 2;
		}
		rq = rq->do_targetq;
	} while (slowpath(rq->do_targetq));
	_dispatch_apply_f2(rq, da, _dispatch_apply3);
	do {
		(void)dispatch_atomic_sub2o(dq, dq_running, da_width);
		dq = dq->do_targetq;
	} while (slowpath(dq->do_targetq));
}

DISPATCH_NOINLINE
void
dispatch_apply_f(size_t iterations, dispatch_queue_t dq, void *ctxt,
		void (*func)(void *, size_t))
{
	if (slowpath(iterations == 0)) {
		return;
	}

	dispatch_apply_t da = (typeof(da))_dispatch_continuation_alloc();

	da->da_func = func;
	da->da_ctxt = ctxt;
	da->da_iterations = iterations;
	da->da_index = 0;
	da->da_thr_cnt = _dispatch_hw_config.cc_max_active;
	da->da_done = 0;
	da->da_queue = NULL;

	if (da->da_thr_cnt > DISPATCH_APPLY_MAX_CPUS) {
		da->da_thr_cnt = DISPATCH_APPLY_MAX_CPUS;
	}
	if (iterations < da->da_thr_cnt) {
		da->da_thr_cnt = (uint32_t)iterations;
	}
	if (slowpath(dq->dq_width <= 2) || slowpath(da->da_thr_cnt <= 1) ||
			slowpath(_dispatch_thread_getspecific(dispatch_apply_key))) {
		return dispatch_sync_f(dq, da, _dispatch_apply_serial);
	}
	dispatch_queue_t old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	if (slowpath(dq->do_targetq)) {
		if (slowpath(dq == old_dq)) {
			return dispatch_sync_f(dq, da, _dispatch_apply_serial);
		} else {
			da->da_queue = dq;
			return dispatch_sync_f(dq, da, _dispatch_apply_redirect);
		}
	}
	dispatch_atomic_acquire_barrier();
	_dispatch_thread_setspecific(dispatch_queue_key, dq);
	_dispatch_apply_f2(dq, da, _dispatch_apply2);
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);
}

#ifdef __BLOCKS__
#if DISPATCH_COCOA_COMPAT
DISPATCH_NOINLINE
static void
_dispatch_apply_slow(size_t iterations, dispatch_queue_t dq,
		void (^work)(size_t))
{
	struct Block_basic *bb = (void *)_dispatch_Block_copy((void *)work);
	dispatch_apply_f(iterations, dq, bb, (void *)bb->Block_invoke);
	Block_release(bb);
}
#endif

void
dispatch_apply(size_t iterations, dispatch_queue_t dq, void (^work)(size_t))
{
#if DISPATCH_COCOA_COMPAT
	// Under GC, blocks transferred to other threads must be Block_copy()ed
	// rdar://problem/7455071
	if (dispatch_begin_thread_4GC) {
		return _dispatch_apply_slow(iterations, dq, work);
	}
#endif
	struct Block_basic *bb = (void *)work;
	dispatch_apply_f(iterations, dq, bb, (void *)bb->Block_invoke);
}
#endif

#if 0
#ifdef __BLOCKS__
void
dispatch_stride(size_t offset, size_t stride, size_t iterations,
		dispatch_queue_t dq, void (^work)(size_t))
{
	struct Block_basic *bb = (void *)work;
	dispatch_stride_f(offset, stride, iterations, dq, bb,
			(void *)bb->Block_invoke);
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
