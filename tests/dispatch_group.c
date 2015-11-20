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
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <libkern/OSAtomic.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000
#endif

#if TARGET_OS_EMBEDDED
#define LOOP_COUNT 50000
#else
#define LOOP_COUNT 200000
#endif

static void test_group_notify(void*);

dispatch_group_t
create_group(size_t count, int delay)
{
	size_t i;

	dispatch_group_t group = dispatch_group_create();

	for (i = 0; i < count; ++i) {
		dispatch_queue_t queue = dispatch_queue_create(NULL, NULL);
		assert(queue);

		dispatch_group_async(group, queue, ^{
			if (delay) {
				fprintf(stderr, "sleeping...\n");
				sleep(delay);
				fprintf(stderr, "done.\n");
			}
		});

		dispatch_release(queue);
		}
	return group;
}

static void
test_group(void *ctxt __attribute__((unused)))
{
	long res;
	dispatch_group_t group;

	group = create_group(100, 0);
	test_ptr_notnull("dispatch_group_async", group);

	dispatch_group_wait(group, DISPATCH_TIME_FOREVER);

	// should be OK to re-use a group
	dispatch_group_async(group, dispatch_get_global_queue(0, 0), ^{});
	dispatch_group_wait(group, DISPATCH_TIME_FOREVER);

	dispatch_release(group);
	group = NULL;

	group = create_group(3, 7);
	test_ptr_notnull("dispatch_group_async", group);

	res = dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 5ull * NSEC_PER_SEC));
	test_long("dispatch_group_wait", !res, 0);

	// retry after timeout (this time succeed)
	res = dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 5ull * NSEC_PER_SEC));
	test_long("dispatch_group_wait", res, 0);

	dispatch_release(group);
	group = NULL;

	group = create_group(100, 0);
	test_ptr_notnull("dispatch_group_async", group);

	dispatch_group_notify(group, dispatch_get_main_queue(), ^{
		dispatch_test_current("Notification Received", dispatch_get_main_queue());
		dispatch_async_f(dispatch_get_main_queue(), NULL, test_group_notify);
	});

	dispatch_release(group);
	group = NULL;
}

static long completed;

static void
test_group_notify2(long cycle, dispatch_group_t tested)
{
	static dispatch_queue_t rq, nq;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		rq = dispatch_queue_create("release", 0);
		dispatch_suspend(rq);
		nq = dispatch_queue_create("notify", 0);
	});
	dispatch_resume(rq);

	// n=4 works great for a 4CPU Mac Pro, this might work for a wider range of
	// systems.
	const int n = 1 + arc4random() % 8;
	dispatch_group_t group = dispatch_group_create();
	dispatch_queue_t qa[n];

	dispatch_group_enter(group);
	for (int i = 0; i < n; i++) {
		char buf[48];
		sprintf(buf, "T%ld-%d", cycle, i);
		qa[i] = dispatch_queue_create(buf, 0);
	}

	__block float eh = 0;
	for (int i = 0; i < n; i++) {
		dispatch_queue_t q = qa[i];
		dispatch_group_async(group, q, ^{
			// Seems to trigger a little more reliably with some work being
			// done in this block
			eh = sin(M_1_PI / cycle);
		});
	}
	dispatch_group_leave(group);

	dispatch_group_notify(group, nq, ^{
		completed = cycle;
		dispatch_group_leave(tested);
	});

	// Releasing qa's queues here seems to avoid the race, so we are arranging
	// for the current iteration's queues to be released on the next iteration.
	dispatch_sync(rq, ^{});
	dispatch_suspend(rq);
	for (int i = 0; i < n; i++) {
		dispatch_queue_t q = qa[i];
		dispatch_async(rq, ^{ dispatch_release(q); });
	}
	dispatch_release(group);
}

static void
test_group_notify(void *ctxt __attribute__((unused)))
{
	// <rdar://problem/11445820>
	dispatch_group_t tested = dispatch_group_create();
	test_ptr_notnull("dispatch_group_create", tested);
	long i;
	for (i = 0; i < LOOP_COUNT; i++) {
		if (!((i+1) % (LOOP_COUNT/10))) {
			fprintf(stderr, "#%ld\n", i+1);
		}
		dispatch_group_enter(tested);
		test_group_notify2(i, tested);
		if (dispatch_group_wait(tested, dispatch_time(DISPATCH_TIME_NOW,
				NSEC_PER_SEC))) {
			break;
		}
	}
	test_long("dispatch_group_notify", i, LOOP_COUNT);
	test_stop();
}

int
main(void)
{
	dispatch_test_start("Dispatch Group");
	dispatch_async_f(dispatch_get_main_queue(), NULL, test_group);
	dispatch_main();

	return 0;
}
