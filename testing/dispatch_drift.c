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
#include <mach/mach_time.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <TargetConditionals.h>
#include <bsdtests.h>
#include "dispatch_test.h"

#if TARGET_OS_EMBEDDED
#define ACCEPTABLE_DRIFT 0.002
#else
#define ACCEPTABLE_DRIFT 0.001
#endif

int
main(int argc __attribute__((unused)), char* argv[] __attribute__((unused)))
{
	__block uint32_t count = 0;
	__block double last_jitter = 0;
	// 10 times a second
	uint64_t interval = 1000000000 / 10;
	double interval_d = interval / 1000000000.0;
	// for 25 seconds
	unsigned int target = 25 / interval_d;

	dispatch_test_start("Dispatch Timer Drift");

	dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	test_ptr_notnull("dispatch_source_create", t);

	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, interval), interval, 0);

	dispatch_source_set_event_handler(t, ^{
		struct timeval now_tv;
		static double first = 0;
		gettimeofday(&now_tv, NULL);
		double now = now_tv.tv_sec + now_tv.tv_usec / 1000000.0;

		if (count == 0) {
			// Because this is taken at 1st timer fire,
			// later jitter values may be negitave.
			// This doesn't effect the drift calculation.
			first = now;
		}
		double goal = first + interval_d * count;
		double jitter = goal - now;
		double drift = jitter - last_jitter;

		printf("%4d: jitter %f, drift %f\n", count, jitter, drift);

		if (target <= ++count) {
			if (drift < 0) {
				drift = -drift;
			}
			double acceptable_drift = ACCEPTABLE_DRIFT;
			test_double_less_than("drift", drift, acceptable_drift);
			test_stop();
		}
		last_jitter = jitter;
	});

	dispatch_resume(t);

	dispatch_main();
	return 0;
}
