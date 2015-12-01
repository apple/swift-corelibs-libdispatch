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

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <TargetConditionals.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#define COUNT	1000ul
#define LAPS	10ul

#if TARGET_OS_EMBEDDED
#define ACCEPTABLE_LATENCY 3000
#else
#define ACCEPTABLE_LATENCY 1000
#endif

static dispatch_queue_t queues[COUNT];
static size_t lap_count_down = LAPS;
static size_t count_down;
static uint64_t start;
static mach_timebase_info_data_t tbi;

static void do_test(void);

static void
collect(void *context __attribute__((unused)))
{
	uint64_t delta;
	long double math;
	size_t i;

	if (--count_down) {
		return;
	}

	delta = mach_absolute_time() - start;
	delta *= tbi.numer;
	delta /= tbi.denom;
	math = delta;
	math /= COUNT * COUNT * 2ul + COUNT * 2ul;

	printf("lap: %ld\n", lap_count_down);
	printf("count: %lu\n", COUNT);
	printf("delta: %llu ns\n", delta);
	printf("math: %Lf ns / lap\n", math);

	for (i = 0; i < COUNT; i++) {
		dispatch_release(queues[i]);
	}

	// our malloc could be a lot better,
	// this result is really a malloc torture test
	test_long_less_than("Latency" , (unsigned long)math, ACCEPTABLE_LATENCY);

	if (--lap_count_down) {
		return do_test();
	}

	// give the threads some time to settle before test_stop() runs "leaks"
	// ...also note, this is a total cheat.   dispatch_after lets this
	// thread go idle, so dispatch cleans up the continuations cache.
	// Doign the "old style" sleep left that stuff around and leaks
	// took a LONG TIME to complete.   Long enough that the test harness
	// decided to kill us.
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, test_stop_after_delay);
}

static void
pong(void *context)
{
	dispatch_queue_t this_q = context;
	size_t replies = (size_t)dispatch_get_context(this_q);

	dispatch_set_context(this_q, (void *)--replies);
	if (!replies) {
		//printf("collect from: %s\n", dispatch_queue_get_label(dispatch_get_current_queue()));
		dispatch_async_f(dispatch_get_main_queue(), NULL, collect);
	}
}

static void
ping(void *context)
{
	dispatch_queue_t reply_q = context;

	dispatch_async_f(reply_q, reply_q, pong);
}

static void
start_node(void *context)
{
	dispatch_queue_t this_q = context;
	size_t i;

	dispatch_set_context(this_q, (void *)COUNT);

	for (i = 0; i < COUNT; i++) {
		dispatch_async_f(queues[i], this_q, ping);
	}
}

void
do_test(void)
{
	dispatch_queue_t soup = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0);
	kern_return_t kr;
	char buf[1000];
	size_t i;

	count_down = COUNT;

	kr = mach_timebase_info(&tbi);
	assert(kr == 0);

	start = mach_absolute_time();

	for (i = 0; i < COUNT; i++) {
		snprintf(buf, sizeof(buf), "com.example.starfish-node#%ld", i);
		queues[i] = dispatch_queue_create(buf, NULL);
		dispatch_suspend(queues[i]);
		dispatch_set_target_queue(queues[i], soup);
	}

	for (i = 0; i < COUNT; i++) {
		dispatch_async_f(queues[i], queues[i], start_node);
	}

	for (i = 0; i < COUNT; i++) {
		dispatch_resume(queues[i]);
	}
}

int
main(void)
{
	dispatch_test_start("Dispatch Starfish");

	do_test();

	dispatch_main();
}
