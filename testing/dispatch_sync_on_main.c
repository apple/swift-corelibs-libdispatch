/*
 * Copyright (c) 2009-2011 Apple Inc. All rights reserved.
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
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>

#include <bsdtests.h>
#include "dispatch_test.h"

const int32_t final = 10;
int global_count = 0;

static void
work(void* ctxt __attribute__((unused)))
{
	if (global_count == INT_MAX) {
		test_stop();
	}
	printf("Firing timer on main %d\n", ++global_count);
	dispatch_after_f(dispatch_time(0, 100000*NSEC_PER_USEC),
			dispatch_get_main_queue(), NULL, work);
}

int
main(void)
{
	dispatch_test_start("Dispatch Sync on main"); // <rdar://problem/7181849>

	dispatch_queue_t dq = dispatch_queue_create("foo.bar", NULL);
	dispatch_async(dq, ^{
		dispatch_async_f(dispatch_get_main_queue(), NULL, work);
		int i;
		for (i=0; i<final; ++i) {
			dispatch_sync(dispatch_get_main_queue(), ^{
				printf("Calling sync %d\n", i);
				test_long("sync on main", pthread_main_np(), 1);
				if (i == final-1) {
					global_count = INT_MAX;
				}
			});
			const struct timespec t = {.tv_nsec = 50000*NSEC_PER_USEC};
			nanosleep(&t, NULL);
		}
	});
	dispatch_release(dq);

	CFRunLoopRun();
	return 0;
}
