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

#include <dispatch/dispatch.h>

#include <bsdtests.h>
#include "dispatch_test.h"

dispatch_source_t tweedledee;
dispatch_source_t tweedledum;

void
fini(void *cxt)
{
	test_ptr_notnull("finalizer ran", cxt);
	if (cxt == tweedledum) {
		test_stop();
	}
}

void
test_timer(void)
{
	dispatch_test_start("Dispatch Suspend Timer");

	dispatch_queue_t main_q = dispatch_get_main_queue();
	test_ptr("dispatch_get_main_queue", main_q, dispatch_get_current_queue());

	__block int i = 0, i_prime = 0;
	__block int j = 0;

	tweedledee = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, main_q);
	test_ptr_notnull("dispatch_source_timer_create", tweedledee);

	dispatch_source_set_timer(tweedledee, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, 0);

	dispatch_source_set_cancel_handler(tweedledee, ^{
		dispatch_release(tweedledee);
	});

	dispatch_source_set_event_handler(tweedledee, ^{
		i_prime += dispatch_source_get_data(tweedledee);
		fprintf(stderr, "tweedledee %d (%d)\n", ++i, i_prime);
		if (i == 10) {
			dispatch_source_cancel(tweedledee);
		}
	});

	dispatch_set_context(tweedledee, tweedledee);
	dispatch_set_finalizer_f(tweedledee, fini);
	dispatch_resume(tweedledee);

	tweedledum = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, main_q);
	test_ptr_notnull("dispatch_source_timer_create", tweedledum);

	dispatch_source_set_timer(tweedledum, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC + NSEC_PER_SEC / 2), 3 * NSEC_PER_SEC, 0);

	dispatch_source_set_cancel_handler(tweedledum, ^{
		dispatch_release(tweedledum);
	});

	dispatch_source_set_event_handler(tweedledum, ^{
		switch(++j) {
			case 1:
				fprintf(stderr, "suspending timer for 3 seconds\n");
				dispatch_suspend(tweedledee);
				break;
			case 2:
				fprintf(stderr, "resuming timer\n");
				test_long("tweedledee tick count", i, 3);
				test_long("tweedledee virtual tick count", i_prime, 3);
				dispatch_resume(tweedledee);
				break;
			default:
				test_long("tweedledee tick count", i, 7);
				test_long("tweedledee virtual tick count", i_prime, 9);
				dispatch_source_cancel(tweedledum);
				break;
		}
	});

	dispatch_set_context(tweedledum, tweedledum);
	dispatch_set_finalizer_f(tweedledum, fini);
	dispatch_resume(tweedledum);
}

int
main(void)
{
	test_timer();
	dispatch_main();

	return 0;
}
