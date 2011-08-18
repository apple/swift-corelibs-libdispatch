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

// We'd use __attribute__((aligned(x))), but it does not atually increase the
// alignment of stack variables. All we really need is the stack usage of the
// local thread to be sufficiently away to avoid cache-line contention with the
// busy 'da_index' variable.
//
// NOTE: 'char' arrays cause GCC to insert buffer overflow detection logic
struct dispatch_apply_s {
	long _da_pad0[DISPATCH_CACHELINE_SIZE / sizeof(long)];
	void (*da_func)(void *, size_t);
	void *da_ctxt;
	size_t da_iterations;
	size_t da_index;
	uint32_t da_thr_cnt;
	_dispatch_thread_semaphore_t da_sema;
	dispatch_queue_t da_queue;
	long _da_pad1[DISPATCH_CACHELINE_SIZE / sizeof(long)];
};

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_apply_invoke(void *ctxt)
{
	struct dispatch_apply_s *da = ctxt;
	size_t const iter = da->da_iterations;
	typeof(da->da_func) const func = da->da_func;
	void *const da_ctxt = da->da_ctxt;
	size_t idx;

	_dispatch_workitem_dec(); // this unit executes many items

	// Make nested dispatch_apply fall into serial case rdar://problem/9294578
	_dispatch_thread_setspecific(dispatch_apply_key, (void*)~0ul);
	// Striding is the responsibility of the caller.
	while (fastpath((idx = dispatch_atomic_inc2o(da, da_index) - 1) < iter)) {
		_dispatch_client_callout2(da_ctxt, idx, func);
		_dispatch_workitem_inc();
	}
	_dispatch_thread_setspecific(dispatch_apply_key, NULL);

	dispatch_atomic_release_barrier();
	if (dispatch_atomic_dec2o(da, da_thr_cnt) == 0) {
		_dispatch_thread_semaphore_signal(da->da_sema);
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
	struct dispatch_apply_s *da = ctxt;
	dispatch_queue_t old_dq = _dispatch_thread_getspecific(dispatch_queue_key);

	_dispatch_thread_setspecific(dispatch_queue_key, da->da_queue);
	_dispatch_apply_invoke(ctxt);
	_dispatch_thread_setspecific(dispatch_queue_key, old_dq);
}

static void
_dispatch_apply_serial(void *ctxt)
{
	struct dispatch_apply_s *da = ctxt;
	size_t idx = 0;

	_dispatch_workitem_dec(); // this unit executes many items
	do {
		_dispatch_client_callout2(da->da_ctxt, idx, da->da_func);
		_dispatch_workitem_inc();
	} while (++idx < da->da_iterations);
}

// 256 threads should be good enough for the short to mid term
#define DISPATCH_APPLY_MAX_CPUS 256

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_apply_f2(dispatch_queue_t dq, struct dispatch_apply_s *da,
		dispatch_function_t func)
{
	struct dispatch_apply_dc_s {
		DISPATCH_CONTINUATION_HEADER(dispatch_apply_dc_s);
	} da_dc[DISPATCH_APPLY_MAX_CPUS];
	size_t i;

	for (i = 0; i < da->da_thr_cnt - 1; i++) {
		da_dc[i].do_vtable = NULL;
		da_dc[i].do_next = &da_dc[i + 1];
		da_dc[i].dc_func = func;
		da_dc[i].dc_ctxt = da;
	}

	da->da_sema = _dispatch_get_thread_semaphore();

	_dispatch_queue_push_list(dq, (void *)&da_dc[0],
			(void *)&da_dc[da->da_thr_cnt - 2]);
	// Call the first element directly
	_dispatch_apply2(da);
	_dispatch_workitem_inc();

	_dispatch_thread_semaphore_wait(da->da_sema);
	_dispatch_put_thread_semaphore(da->da_sema);
}

static void
_dispatch_apply_redirect(void *ctxt)
{
	struct dispatch_apply_s *da = ctxt;
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
	struct dispatch_apply_s da;

	da.da_func = func;
	da.da_ctxt = ctxt;
	da.da_iterations = iterations;
	da.da_index = 0;
	da.da_thr_cnt = _dispatch_hw_config.cc_max_active;
	da.da_queue = NULL;

	if (da.da_thr_cnt > DISPATCH_APPLY_MAX_CPUS) {
		da.da_thr_cnt = DISPATCH_APPLY_MAX_CPUS;
	}
	if (slowpath(iterations == 0)) {
		return;
	}
	if (iterations < da.da_thr_cnt) {
		da.da_thr_cnt = (uint32_t)iterations;
	}
	if (slowpath(dq->dq_width <= 2) || slowpath(da.da_thr_cnt <= 1) ||
			slowpath(_dispatch_thread_getspecific(dispatch_apply_key))) {
		return dispatch_sync_f(dq, &da, _dispatch_apply_serial);
	}
	dispatch_queue_t old_dq = _dispatch_thread_getspecific(dispatch_queue_key);
	if (slowpath(dq->do_targetq)) {
		if (slowpath(dq == old_dq)) {
			return dispatch_sync_f(dq, &da, _dispatch_apply_serial);
		} else {
			da.da_queue = dq;
			return dispatch_sync_f(dq, &da, _dispatch_apply_redirect);
		}
	}
	dispatch_atomic_acquire_barrier();
	_dispatch_thread_setspecific(dispatch_queue_key, dq);
	_dispatch_apply_f2(dq, &da, _dispatch_apply2);
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
