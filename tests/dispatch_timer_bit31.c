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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include <dispatch/dispatch.h>

#include <bsdtests.h>
#include "dispatch_test.h"

void
test_timer(void)
{
	dispatch_test_start("Dispatch Source Timer, bit 31");

	dispatch_queue_t main_q = dispatch_get_main_queue();
	//test_ptr("dispatch_get_main_queue", main_q, dispatch_get_current_queue());

	struct timeval start_time;

	static dispatch_source_t s;
	s = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, main_q);
	test_ptr_notnull("dispatch_source_create", s);
	dispatch_source_set_timer(s, dispatch_time(DISPATCH_TIME_NOW, 0x80000000ull), 0x80000000ull, 0);
	gettimeofday(&start_time, NULL);

	dispatch_source_set_event_handler(s, ^{
		dispatch_source_cancel(s);
	});

	dispatch_source_set_cancel_handler(s, ^{
		struct timeval end_time;
		gettimeofday(&end_time, NULL);
		// check, s/b 2.0799... seconds, which is <4 seconds
		// when it could end on a bad boundry.
		test_long_less_than("needs to finish faster than 4 seconds", end_time.tv_sec - start_time.tv_sec, 4);
		// And it has to take at least two seconds...
		test_long_less_than("can't finish faster than 2 seconds", 1, end_time.tv_sec - start_time.tv_sec);
		test_stop();
	});

	dispatch_resume(s);
}

int
main(void)
{
	test_timer();
	dispatch_main();

	return 0;
}
