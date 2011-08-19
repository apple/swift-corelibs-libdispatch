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

#include <dispatch/dispatch.h>
#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <libkern/OSAtomic.h>

#include <bsdtests.h>
#include "dispatch_test.h"

const int32_t final = 10;
static volatile int32_t count;

static void
work(void* ctxt __attribute__((unused)))
{
	int32_t c = OSAtomicIncrement32(&count);
	if (c < final-1) {
		dispatch_async_f(dispatch_get_main_queue(), NULL, work);
		CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{
			fprintf(stderr, "CFRunLoopPerformBlock %d\n", c);
			test_long_less_than("CFRunLoopPerformBlock", count, final);
		});
	}
}

int
main(void)
{
	dispatch_test_start("Dispatch CF main queue"); // <rdar://problem/7760523>
	dispatch_async_f(dispatch_get_main_queue(), NULL, work);
	dispatch_async_f(dispatch_get_main_queue(), NULL, work);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC),
			dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
		test_long("done", count, final);
		test_stop();
	});
	CFRunLoopRun();

	return 0;
}
