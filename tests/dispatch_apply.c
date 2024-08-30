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

#include <dispatch/dispatch.h>
#include <stdio.h>
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <unistd.h>
#ifdef __ANDROID__
#include <linux/sysctl.h>
#else
#if !defined(__linux__)
#include <sys/sysctl.h>
#endif
#endif /* __ANDROID__ */
#endif
#include <stdlib.h>
#include <assert.h>
#include <stdatomic.h>
#include <sys/types.h>

#include <bsdtests.h>
#include "dispatch_test.h"

static atomic_int busy_threads_started, busy_threads_finished;

/*
 * Keep a thread busy, spinning on the CPU.
 */
static volatile int all_done = 0;

/* Fiddling with j in the middle and hitting this global will hopefully keep
 * the optimizer from cutting the whole thing out as dead code.
 */
static volatile unsigned int busythread_useless;
static void busythread(void *ignored)
{
	(void)ignored;
	/* prevent i and j been optimized out */
	volatile uint64_t i = 0, j = 0;

	__c11_atomic_fetch_add(&busy_threads_started, 1, memory_order_relaxed);

	while(!all_done)
	{
		if(i == 500000) { j -= busythread_useless; }
		j += i;
		i += 1;
	}
	(void)j;

	__c11_atomic_fetch_add(&busy_threads_finished, 1, memory_order_relaxed);
}

/*
 * Test that dispatch_apply can make progress and finish, even if there are
 * so many other running and unblocked workqueue threads that the apply's
 * helper threads never get a chance to come up.
 *
 * <rdar://problem/10718199> dispatch_apply should not block waiting on other
 * threads while calling thread is available
 */
static void test_apply_contended(dispatch_queue_t dq)
{
	uint32_t activecpu;
#if defined(__linux__) || defined(__OpenBSD__)
	activecpu = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_WIN32)
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	activecpu = si.dwNumberOfProcessors;
#else
	size_t s = sizeof(activecpu);
	sysctlbyname("hw.activecpu", &activecpu, &s, NULL, 0);
#endif
	int tIndex, n_threads = (int)activecpu;
	dispatch_group_t grp = dispatch_group_create();

	for(tIndex = 0; tIndex < n_threads; tIndex++) {
		dispatch_group_async_f(grp, dq, NULL, busythread);
	}

	// Spin until all the threads have actually started
	while(busy_threads_started < n_threads) {
		usleep(1);
	}

	
	__block atomic_int count = ATOMIC_VAR_INIT(0);
	const int32_t final = 32;

	int32_t before = busy_threads_started;
	dispatch_apply(final, dq, ^(size_t i __attribute__((unused))) {
		__c11_atomic_fetch_add(&count, 1, memory_order_relaxed);
	});
	int32_t after = busy_threads_finished;

	test_long("contended: threads started before apply", before, n_threads);
	test_long("contended: count", count, final);
	test_long("contended: threads finished before apply", after, 0);

	/* Release busy threads by setting all_done to 1 */
        all_done = 1;

	dispatch_group_wait(grp, DISPATCH_TIME_FOREVER);
	dispatch_release(grp);

}

int
main(void)
{
	dispatch_test_start("Dispatch Apply");

	__block atomic_int count = ATOMIC_VAR_INIT(0);
	const int32_t final = 32;

	dispatch_queue_t queue = dispatch_get_global_queue(0, 0);
	test_ptr_notnull("dispatch_get_global_queue", queue);

	dispatch_apply(final, queue, ^(size_t i __attribute__((unused))) {
		__c11_atomic_fetch_add(&count, 1, memory_order_relaxed);
	});
	test_long("count", count, final);

	count = 0; // rdar://problem/9294578
	dispatch_apply(final, queue, ^(size_t i __attribute__((unused))) {
		dispatch_apply(final, queue, ^(size_t ii __attribute__((unused))) {
			dispatch_apply(final, queue, ^(size_t iii __attribute__((unused))) {
				__c11_atomic_fetch_add(&count, 1, memory_order_relaxed);
			});
		});
	});
	test_long("nested count", count, final * final * final);

	test_apply_contended(queue);

	test_stop();

	return 0;
}
