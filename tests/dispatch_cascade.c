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
#include <unistd.h>
#include <stdlib.h>

#include <bsdtests.h>
#include "dispatch_test.h"

int done = 0;

#define QUEUES 80
dispatch_queue_t queues[QUEUES];

#define BLOCKS 10000
union {
	size_t index;
	char padding[64];
} indices[BLOCKS];

size_t iterations = QUEUES * BLOCKS * 0.25;

static void
noop(void *ctxt __attribute__((unused)))
{
	return;
}

static void
cleanup(void *ctxt __attribute__((unused)))
{
	size_t q;
	for (q = 0; q < QUEUES; ++q) {
		dispatch_sync_f(queues[q], NULL, noop);
		dispatch_release(queues[q]);
	}
	test_stop();
	exit(0);
}

void
histogram(void)
{
	size_t counts[QUEUES] = {};
	size_t maxcount = 0;

	size_t q;
	for (q = 0; q < QUEUES; ++q) {
		size_t i;
		for (i = 0; i < BLOCKS; ++i) {
			if (indices[i].index == q) {
				++counts[q];
			}
		}
	}

	for (q = 0; q < QUEUES; ++q) {
		if (counts[q] > maxcount) {
			maxcount = counts[q];
		}
	}

	printf("maxcount = %ld\n", maxcount);

	size_t x,y;
	for (y = 20; y > 0; --y) {
		for (x = 0; x < QUEUES; ++x) {
			double fraction = (double)counts[x] / (double)maxcount;
			double value = fraction * (double)20;
			printf("%s", (value > y) ? "*" : " ");
		}
		printf("\n");
	}
}

void
cascade(void* context)
{
	size_t idx, *idxptr = context;

	if (done) return;

	idx = *idxptr + 1;

	if (idx < QUEUES) {
		*idxptr = idx;
		dispatch_async_f(queues[idx], context, cascade);
	}

	if (__sync_sub_and_fetch(&iterations, 1) == 0) {
		done = 1;
		histogram();
		dispatch_async_f(dispatch_get_main_queue(), NULL, cleanup);
	}
}

int
main(int argc __attribute__((unused)), char* argv[] __attribute__((unused)))
{
	int i;

	dispatch_test_start("Dispatch Cascade");

	for (i = 0; i < QUEUES; ++i) {
		queues[i] = dispatch_queue_create(NULL, NULL);
	}

	for (i = 0; i < BLOCKS; ++i) {
		cascade(&indices[i].index);
	}

	dispatch_main();

	return 0;
}
