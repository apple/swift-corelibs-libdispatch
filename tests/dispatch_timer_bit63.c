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
	dispatch_test_start("Dispatch Source Timer, bit 63");

	//uint64_t interval = 0xffffffffffffffffull;
	uint64_t interval = 0x8000000000000001ull;

	dispatch_queue_t mainq = dispatch_get_main_queue();

	__block int i = 0;
	struct timeval start_time;

	gettimeofday(&start_time, NULL);

	static dispatch_source_t ds;
	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, mainq);
	assert(ds);
	dispatch_source_set_event_handler(ds, ^{
		assert(i < 1);
		printf("%d\n", i++);
	});
	dispatch_source_set_timer(ds, DISPATCH_TIME_NOW, interval, 0);
	dispatch_resume(ds);

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1*NSEC_PER_SEC),
		dispatch_get_main_queue(), ^{
		test_stop();
	});
}

int
main(void)
{
	test_timer();
	dispatch_main();

	return 0;
}
