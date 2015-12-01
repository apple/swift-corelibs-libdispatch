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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include <dispatch/dispatch.h>

#include <bsdtests.h>
#include "dispatch_test.h"

int
main(void)
{
	dispatch_test_start("Dispatch Source Timeout"); // <rdar://problem/8015967>

	uint64_t mini_interval = 100ull; // 100 ns
	uint64_t long_interval = 2000000000ull; // 2 secs

	dispatch_queue_t timer_queue = dispatch_queue_create("timer_queue", NULL);

	__block int value_to_be_changed = 5;
	__block int fired = 0;

	dispatch_source_t mini_timer, long_timer;
	mini_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, timer_queue);
	dispatch_source_set_event_handler(mini_timer, ^{
		printf("Firing mini-timer %d\n", ++fired);
		printf("Suspending mini-timer queue\n");
		dispatch_suspend(timer_queue);
	});
	dispatch_source_set_timer(mini_timer, DISPATCH_TIME_NOW, mini_interval, 0);

	long_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(0, 0));
	dispatch_source_set_event_handler(long_timer, ^{
		printf("Firing long timer\n");
		value_to_be_changed = 10;
		dispatch_source_cancel(long_timer);
	});
	dispatch_source_set_timer(long_timer,
		dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC) ,
		long_interval, 0);

	dispatch_resume(mini_timer);
	dispatch_resume(long_timer);
	sleep(6);
	test_long("Checking final value", value_to_be_changed, 10);
	test_long("Mini-timer fired", fired, 1);
	dispatch_source_cancel(mini_timer);
	dispatch_release(mini_timer);
	dispatch_resume(timer_queue);
	dispatch_release(timer_queue);
	dispatch_release(long_timer);
	test_stop();
	return 0;
}
