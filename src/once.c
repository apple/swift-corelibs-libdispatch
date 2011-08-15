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

#undef dispatch_once
#undef dispatch_once_f


struct _dispatch_once_waiter_s {
	volatile struct _dispatch_once_waiter_s *volatile dow_next;
	_dispatch_thread_semaphore_t dow_sema;
};

#define DISPATCH_ONCE_DONE ((struct _dispatch_once_waiter_s *)~0l)

#ifdef __BLOCKS__
void
dispatch_once(dispatch_once_t *val, dispatch_block_t block)
{
	struct Block_basic *bb = (void *)block;

	dispatch_once_f(val, block, (void *)bb->Block_invoke);
}
#endif

DISPATCH_NOINLINE
void
dispatch_once_f(dispatch_once_t *val, void *ctxt, dispatch_function_t func)
{
	struct _dispatch_once_waiter_s * volatile *vval =
			(struct _dispatch_once_waiter_s**)val;
	struct _dispatch_once_waiter_s dow = { NULL, 0 };
	struct _dispatch_once_waiter_s *tail, *tmp;
	_dispatch_thread_semaphore_t sema;

	if (dispatch_atomic_cmpxchg(vval, NULL, &dow)) {
		dispatch_atomic_acquire_barrier();
		_dispatch_client_callout(ctxt, func);

		// The next barrier must be long and strong.
		//
		// The scenario: SMP systems with weakly ordered memory models
		// and aggressive out-of-order instruction execution.
		//
		// The problem:
		//
		// The dispatch_once*() wrapper macro causes the callee's
		// instruction stream to look like this (pseudo-RISC):
		//
		//      load r5, pred-addr
		//      cmpi r5, -1
		//      beq  1f
		//      call dispatch_once*()
		//      1f:
		//      load r6, data-addr
		//
		// May be re-ordered like so:
		//
		//      load r6, data-addr
		//      load r5, pred-addr
		//      cmpi r5, -1
		//      beq  1f
		//      call dispatch_once*()
		//      1f:
		//
		// Normally, a barrier on the read side is used to workaround
		// the weakly ordered memory model. But barriers are expensive
		// and we only need to synchronize once! After func(ctxt)
		// completes, the predicate will be marked as "done" and the
		// branch predictor will correctly skip the call to
		// dispatch_once*().
		//
		// A far faster alternative solution: Defeat the speculative
		// read-ahead of peer CPUs.
		//
		// Modern architectures will throw away speculative results
		// once a branch mis-prediction occurs. Therefore, if we can
		// ensure that the predicate is not marked as being complete
		// until long after the last store by func(ctxt), then we have
		// defeated the read-ahead of peer CPUs.
		//
		// In other words, the last "store" by func(ctxt) must complete
		// and then N cycles must elapse before ~0l is stored to *val.
		// The value of N is whatever is sufficient to defeat the
		// read-ahead mechanism of peer CPUs.
		//
		// On some CPUs, the most fully synchronizing instruction might
		// need to be issued.

		dispatch_atomic_maximally_synchronizing_barrier();
		//dispatch_atomic_release_barrier(); // assumed contained in above
		tmp = dispatch_atomic_xchg(vval, DISPATCH_ONCE_DONE);
		tail = &dow;
		while (tail != tmp) {
			while (!tmp->dow_next) {
				_dispatch_hardware_pause();
			}
			sema = tmp->dow_sema;
			tmp = (struct _dispatch_once_waiter_s*)tmp->dow_next;
			_dispatch_thread_semaphore_signal(sema);
		}
	} else {
		dow.dow_sema = _dispatch_get_thread_semaphore();
		for (;;) {
			tmp = *vval;
			if (tmp == DISPATCH_ONCE_DONE) {
				break;
			}
			dispatch_atomic_store_barrier();
			if (dispatch_atomic_cmpxchg(vval, tmp, &dow)) {
				dow.dow_next = tmp;
				_dispatch_thread_semaphore_wait(dow.dow_sema);
			}
		}
		_dispatch_put_thread_semaphore(dow.dow_sema);
	}
}
