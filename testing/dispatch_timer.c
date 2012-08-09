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

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <dispatch/dispatch.h>

#include <bsdtests.h>
#include "dispatch_test.h"

static bool finalized = false;

void
test_fin(void *cxt)
{
	test_ptr("finalizer ran", cxt, cxt);
	finalized = true;
	test_stop();
}

void
test_timer(void)
{
	dispatch_test_start("Dispatch Source Timer");

	const int stop_at = 3;

	dispatch_queue_t main_q = dispatch_get_main_queue();
	//test_ptr("dispatch_get_main_queue", main_q, dispatch_get_current_queue());

	uint64_t j;

	// create timers in two classes:
	//  * ones that should trigger before the test ends
	//  * ones that shouldn't trigger before the test ends
	for (j = 1; j <= 5; ++j)
	{
		dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(0, 0));
		test_ptr_notnull("dispatch_source_create", s);

		dispatch_source_set_timer(s, dispatch_time(DISPATCH_TIME_NOW, j * NSEC_PER_SEC + NSEC_PER_SEC / 10), DISPATCH_TIME_FOREVER, 0);

		dispatch_source_set_event_handler(s, ^{
			if (!finalized) {
				test_long_less_than("timer number", j, stop_at);
				fprintf(stderr, "timer[%lld]\n", j);
			}
			dispatch_release(s);
		});
		dispatch_resume(s);
	}

	__block int i = 0;

	dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, main_q);
	test_ptr_notnull("dispatch_source_create", s);

	dispatch_source_set_timer(s, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC, 0);

	dispatch_source_set_cancel_handler(s, ^{
		test_ptr_notnull("cancel handler run", s);
		dispatch_release(s);
	});

	dispatch_source_set_event_handler(s, ^{
		fprintf(stderr, "%d\n", ++i);
		if (i >= stop_at) {
			test_long("i", i, stop_at);
			dispatch_source_set_timer(s, dispatch_time(DISPATCH_TIME_NOW, 0), 0, 0);
			dispatch_source_cancel(s);
		}
	});

	dispatch_set_context(s, s);
	dispatch_set_finalizer_f(s, test_fin);

	dispatch_resume(s);
}

int
main(void)
{
	test_timer();
	dispatch_main();

	return 0;
}
