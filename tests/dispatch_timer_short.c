/*
 * Copyright (c) 2010-2011 Apple Inc. All rights reserved.
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

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <mach/mach_time.h>
#include <libkern/OSAtomic.h>

#include <dispatch/dispatch.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#define delay (1ull * NSEC_PER_SEC)
#define interval (5ull * NSEC_PER_USEC)

#define N 100000

static dispatch_source_t t[N];
static dispatch_queue_t q;
static dispatch_group_t g;

static volatile int32_t count;
static mach_timebase_info_data_t tbi;
static uint64_t start, last;

#define elapsed_ms(x) (((now-(x))*tbi.numer/tbi.denom)/(1000ull*NSEC_PER_USEC))

void
test_fin(void *cxt)
{
	fprintf(stderr, "Called back every %llu us on average\n",
			(delay/count)/NSEC_PER_USEC);
	test_long_less_than("Frequency", 1, ceil((double)delay/(count*interval)));
	int i;
	for (i = 0; i < N; i++) {
		dispatch_source_cancel(t[i]);
		dispatch_release(t[i]);
	}
	dispatch_resume(q);
	dispatch_release(q);
	dispatch_release(g);
	test_ptr("finalizer ran", cxt, cxt);
	test_stop();
}

void
test_short_timer(void)
{
	// Add a large number of timers with suspended target queue in front of
	// the timer being measured <rdar://problem/7401353>
	g = dispatch_group_create();
	q = dispatch_queue_create("q", NULL);
	int i;
	for (i = 0; i < N; i++) {
		t[i] = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
		dispatch_source_set_timer(t[i], DISPATCH_TIME_NOW, interval, 0);
		dispatch_group_enter(g);
		dispatch_source_set_registration_handler(t[i], ^{
			dispatch_suspend(t[i]);
			dispatch_group_leave(g);
		});
		dispatch_resume(t[i]);
	}
	// Wait for registration & configuration of all timers
	dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
	dispatch_suspend(q);
	for (i = 0; i < N; i++) {
		dispatch_resume(t[i]);
	}

	dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,
			0, 0, dispatch_get_global_queue(0, 0));
	test_ptr_notnull("dispatch_source_create", s);
	dispatch_source_set_timer(s, DISPATCH_TIME_NOW, interval, 0);
	dispatch_source_set_event_handler(s, ^{
		uint64_t now = mach_absolute_time();
		if (!count) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay),
					dispatch_get_global_queue(0, 0), ^{
				dispatch_source_cancel(s);
				dispatch_release(s);
			});
			fprintf(stderr, "First timer callback  (after %4llu ms)\n",
					elapsed_ms(start));
		}
		OSAtomicIncrement32(&count);
		if (elapsed_ms(last) >= 100) {
			fprintf(stderr, "%5d timer callbacks (after %4llu ms)\n", count,
					elapsed_ms(start));
			last = now;
		}
	});
	dispatch_set_context(s, s);
	dispatch_set_finalizer_f(s, test_fin);
	fprintf(stderr, "Scheduling %llu us timer\n", interval/NSEC_PER_USEC);
	start = last = mach_absolute_time();
	dispatch_resume(s);
}

int
main(void)
{
	dispatch_test_start("Dispatch Short Timer"); // <rdar://problem/7765184>
	mach_timebase_info(&tbi);
	test_short_timer();
	dispatch_main();
	return 0;
}
