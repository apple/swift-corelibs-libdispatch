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

#ifdef __linux__
// for asprintf
#define _GNU_SOURCE 1
#endif
#include <dispatch/dispatch.h>
#include <dispatch/private.h>
#include <stdio.h>
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <unistd.h>
#endif
#include <stdlib.h>
#include <assert.h>
#include <stdatomic.h>

#include <bsdtests.h>
#include "dispatch_test.h"

atomic_int count = ATOMIC_VAR_INIT(0);
const int32_t final = 32;

int
main(void)
{
	dispatch_test_start("Dispatch Overcommit");

	int i;
	for (i = 0; i < final; ++i) {
		char* name;
		(void)asprintf(&name, "test.overcommit.%d", i);

		dispatch_queue_t queue = dispatch_queue_create(name, NULL);
		test_ptr_notnull("dispatch_queue_create", queue);
		free(name);
		dispatch_set_target_queue(queue, dispatch_get_global_queue(0, DISPATCH_QUEUE_OVERCOMMIT));

		dispatch_async(queue, ^{
			__c11_atomic_fetch_add(&count, 1, memory_order_relaxed);
			if (count == final) {
				test_long("count", count, final);
				test_stop();
			} else {
				while (1); // spin
			}
		});
	}

	dispatch_main();

	return 0;
}
