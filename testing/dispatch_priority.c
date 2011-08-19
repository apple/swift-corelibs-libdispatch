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
#include <dispatch/queue_private.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <TargetConditionals.h>
#include <sys/types.h>
#include <sys/sysctl.h>

#include <bsdtests.h>
#include "dispatch_test.h"

static volatile int done;

#ifdef DISPATCH_QUEUE_PRIORITY_BACKGROUND // <rdar://problem/7439794>

#define PRIORITIES 4
char *labels[PRIORITIES] = { "BACKGROUND", "LOW", "DEFAULT", "HIGH" };
int priorities[PRIORITIES] = { DISPATCH_QUEUE_PRIORITY_BACKGROUND, DISPATCH_QUEUE_PRIORITY_LOW, DISPATCH_QUEUE_PRIORITY_DEFAULT, DISPATCH_QUEUE_PRIORITY_HIGH };

#else

#define PRIORITIES 3
char *labels[PRIORITIES] = { "LOW", "DEFAULT", "HIGH" };
int priorities[PRIORITIES] = { DISPATCH_QUEUE_PRIORITY_LOW, DISPATCH_QUEUE_PRIORITY_DEFAULT, DISPATCH_QUEUE_PRIORITY_HIGH };

#endif

#if TARGET_OS_EMBEDDED
#define LOOP_COUNT 2000000
#else
#define LOOP_COUNT 100000000
#endif

static union {
	long count;
	char padding[64];
} counts[PRIORITIES];

#define ITERATIONS (long)(PRIORITIES * n_blocks() * 0.50)
static volatile long iterations;

int
n_blocks(void)
{
	static dispatch_once_t pred;
	static int n;
	dispatch_once(&pred, ^{
		size_t l = sizeof(n);
		int rc = sysctlbyname("hw.ncpu", &n, &l, NULL, 0);
		assert(rc == 0);
		n *= 32;
	});
	return n;
}

void
histogram(void)
{
	long total = 0;
	size_t x,y;
	for (y = 0; y < PRIORITIES; ++y) {
		printf("%s: %ld\n", labels[y], counts[y].count);
		total += counts[y].count;

		double fraction = (double)counts[y].count / (double)n_blocks();
		double value = fraction * (double)80;
		for (x = 0; x < 80; ++x) {
			printf("%s", (value > x) ? "*" : " ");
		}
		printf("\n");
	}

	test_long("blocks completed", total, ITERATIONS);
	test_long_less_than("high priority precedence", counts[0].count,
			counts[PRIORITIES-1].count);
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
			test_stop();
			exit(0);
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
	int i;

	iterations = ITERATIONS;

#if USE_SET_TARGET_QUEUE
	dispatch_test_start("Dispatch Priority (Set Target Queue)");
	for(i = 0; i < PRIORITIES; i++) {
#if DISPATCH_API_VERSION < 20100518 // <rdar://problem/7790099>
		q[i] = dispatch_queue_create(labels[i], NULL);
#else
		q[i] = dispatch_queue_create(labels[i], DISPATCH_QUEUE_CONCURRENT);
#endif
		test_ptr_notnull("q[i]", q[i]);
		assert(q[i]);
		dispatch_suspend(q[i]);
		dispatch_set_target_queue(q[i], dispatch_get_global_queue(priorities[i], 0));
#if DISPATCH_API_VERSION < 20100518 // <rdar://problem/7790099>
		dispatch_queue_set_width(q[i], LONG_MAX);
#endif
	}
#else
	dispatch_test_start("Dispatch Priority");
	for(i = 0; i < PRIORITIES; i++) {
		q[i] = dispatch_get_global_queue(priorities[i], 0);
	}
#endif

	for(i = 0; i < PRIORITIES; i++) {
		submit_work(q[i], &counts[i].count);
	}

#if USE_SET_TARGET_QUEUE
	for(i = 0; i < PRIORITIES; i++) {
		dispatch_resume(q[i]);
		dispatch_release(q[i]);
	}
#endif

	dispatch_main();

	return 0;
}
