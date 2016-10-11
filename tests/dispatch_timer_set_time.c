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

#include <sys/time.h>
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
	dispatch_test_start("Dispatch Update Timer");

	dispatch_queue_t main_q = dispatch_get_main_queue();
	//test_ptr("dispatch_get_main_queue", main_q, dispatch_get_current_queue());

	__block int i = 0;
	struct timeval start_time;

	gettimeofday(&start_time, NULL);

	dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, main_q);
	test_ptr_notnull("dispatch_source_create", s);

	dispatch_source_set_timer(s, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, 0);

	dispatch_source_set_cancel_handler(s, ^{
		struct timeval end_time;
		gettimeofday(&end_time, NULL);
		// Make sure we actually managed to adjust the interval
		// duration.  Fifteen one second ticks would blow past
		// this.
		test_long_less_than("total duration", end_time.tv_sec - start_time.tv_sec, 10);
		test_stop();

		dispatch_release(s);
	});

	dispatch_source_set_event_handler(s, ^{
		fprintf(stderr, "%d\n", ++i);
		if (i >= 15) {
			dispatch_source_cancel(s);
		} else if (i == 1) {
			dispatch_source_set_timer(s, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC / 10, 0);
		}
	});
	test_ptr_notnull("dispatch_source_timer_create", s);

	dispatch_resume(s);
}

int
main(void) {
	test_timer();
	dispatch_main();

	return 0;
}
