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
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <libkern/OSAtomic.h>

#include <bsdtests.h>
#include "dispatch_test.h"

int
main(void)
{
	dispatch_test_start("Dispatch Apply");

	volatile __block int32_t count = 0;
	const int32_t final = 32;

	dispatch_queue_t queue = dispatch_get_global_queue(0, 0);
	test_ptr_notnull("dispatch_get_global_queue", queue);

	dispatch_apply(final, queue, ^(size_t i __attribute__((unused))) {
		OSAtomicIncrement32(&count);
	});
	test_long("count", count, final);

	count = 0; // rdar://problem/9294578
	dispatch_apply(final, queue, ^(size_t i __attribute__((unused))) {
		dispatch_apply(final, queue, ^(size_t ii __attribute__((unused))) {
			dispatch_apply(final, queue, ^(size_t iii __attribute__((unused))) {
				OSAtomicIncrement32(&count);
			});
		});
	});
	test_long("nested count", count, final * final * final);

	test_stop();

	return 0;
}
