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

#include <stdio.h>
#include <dispatch/dispatch.h>
#include <dispatch/private.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include <sys/types.h>
#ifdef __ANDROID__
#include <linux/sysctl.h>
#else
#include <sys/sysctl.h>
#endif /* __ANDROID__ */

#include <bsdtests.h>
#include "dispatch_test.h"

static volatile int done;

#ifdef DISPATCH_QUEUE_PRIORITY_BACKGROUND // <rdar://problem/7439794>
#define USE_BACKGROUND_PRIORITY 1
#else
#define DISPATCH_QUEUE_PRIORITY_BACKGROUND INT16_MIN
#endif

#define QUEUE_PRIORITY_PTHREAD INT_MAX

#if DISPATCH_API_VERSION < 20100518 // <rdar://problem/7790099>
#define DISPATCH_QUEUE_CONCURRENT NULL
#endif

#if TARGET_OS_EMBEDDED
#define LOOP_COUNT 5000000
const int importance = 24; // priority 55
#else
#define LOOP_COUNT 100000000
const int importance = 4; // priority 35
#endif

char *labels[] = { "BACKGROUND", "LOW", "DEFAULT", "HIGH", };
int levels[] = {
	DISPATCH_QUEUE_PRIORITY_BACKGROUND, DISPATCH_QUEUE_PRIORITY_LOW,
	DISPATCH_QUEUE_PRIORITY_DEFAULT, DISPATCH_QUEUE_PRIORITY_HIGH,
};
#define PRIORITIES (sizeof(levels)/sizeof(*levels))

static union {
	long count;
	char padding[64];
} counts[PRIORITIES];

static volatile long iterations;
static long total;
static size_t prio0, priorities = PRIORITIES;

int
n_blocks(void)
{
	static dispatch_once_t pred;
	static int n;
	dispatch_once(&pred, ^{
#ifdef __linux__
		n = sysconf(_SC_NPROCESSORS_CONF);
#else
		size_t l = sizeof(n);
		int rc = sysctlbyname("hw.ncpu", &n, &l, NULL, 0);
		assert(rc == 0);
#endif
		n *= 32;
	});
	return n;
}

void
histogram(void)
{
	long completed = 0;
	size_t x, y, i;
	printf("\n");
	for (y = prio0; y < prio0 + priorities; ++y) {
		printf("%s: %ld\n", labels[y], counts[y].count);
		completed += counts[y].count;

		double fraction = (double)counts[y].count / (double)n_blocks();
		double value = fraction * (double)80;
		for (x = 0; x < 80; ++x) {
			printf("%s", (value > x) ? "*" : " ");
		}
		printf("\n");
	}

	test_long("blocks completed", completed, total);
	for (i = prio0; i < prio0 + priorities; i++) {
		if (levels[i] == DISPATCH_QUEUE_PRIORITY_HIGH) {
			test_long_less_than_or_equal("high priority precedence",
					counts[i-2].count, counts[i].count);
		}
#if USE_BACKGROUND_PRIORITY
		if (levels[i] == DISPATCH_QUEUE_PRIORITY_BACKGROUND) {
			test_long_less_than_or_equal("background priority precedence",
					counts[i].count, counts[i+2].count);
		}
#endif
	}
}

void
cpubusy(void* context)
{
	if (done) return;
	size_t idx;
	for (idx = 0; idx < LOOP_COUNT; ++idx) {
		if (done) break;
	}

	volatile long *count = context;
	long iterdone = __sync_sub_and_fetch(&iterations, 1);

	if (iterdone >= 0) {
		__sync_add_and_fetch(count, 1);
		if (!iterdone) {
			__sync_add_and_fetch(&done, 1);
			usleep(100000);
			histogram();
			dispatch_time_t delay = DISPATCH_TIME_NOW;
			dispatch_after(delay, dispatch_get_main_queue(), ^{
				test_stop();
			});
		}
	}
}

void
submit_work(dispatch_queue_t queue, void* context)
{
	int i;

	for (i = n_blocks(); i; --i) {
		dispatch_async_f(queue, context, cpubusy);
	}

}

int
main(int argc __attribute__((unused)), char* argv[] __attribute__((unused)))
{
	dispatch_queue_t q[PRIORITIES];
	size_t i;

#if !USE_BACKGROUND_PRIORITY
	prio0++;
	priorities--;
#endif

	iterations = total = (priorities * n_blocks()) * 0.50;

#if USE_SET_TARGET_QUEUE
	dispatch_test_start("Dispatch Priority (Set Target Queue)");
#else
	dispatch_test_start("Dispatch Priority");
#endif

	for (i = prio0; i < prio0 + priorities; i++) {
		dispatch_queue_t rq = dispatch_get_global_queue(levels[i], 0);
#if USE_SET_TARGET_QUEUE
		q[i] = dispatch_queue_create(labels[i], DISPATCH_QUEUE_CONCURRENT);
		test_ptr_notnull("q[i]", q[i]);
		assert(q[i]);
		dispatch_suspend(q[i]);
		dispatch_set_target_queue(q[i], rq);
		if (DISPATCH_QUEUE_CONCURRENT != NULL) {
			dispatch_queue_set_width(q[i], LONG_MAX);
		}
		dispatch_release(rq);
#else
		q[i] = rq;
#endif
	}

	for (i = prio0; i < prio0 + priorities; i++) {
		submit_work(q[i], &counts[i].count);
	}

	for (i = prio0; i < prio0 + priorities; i++) {
		dispatch_resume(q[i]);
		dispatch_release(q[i]);
	}

	dispatch_main();

	return 0;
}
