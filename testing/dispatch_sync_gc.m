/*
 * Copyright (c) 2009-2011 Apple Inc. All rights reserved.
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
#import <Foundation/Foundation.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#if __OBJC_GC__
const size_t final = 50000, desclen = 538892;
#else
const size_t final = 1000, desclen = 8892;
#endif
NSAutoreleasePool *pool = nil;

static void
work(void* ctxt __attribute__((unused)))
{
	pool = [[NSAutoreleasePool alloc] init];
	NSMutableArray *a = [NSMutableArray array];
	dispatch_group_t g = dispatch_group_create();

	dispatch_group_async(g, dispatch_get_global_queue(0, 0), ^{
		NSUInteger i;
		for (i = 0; i < final; i++) {
			NSDecimalNumber *n = [NSDecimalNumber decimalNumberWithDecimal:
				   [[NSNumber numberWithInteger:i] decimalValue]];
			dispatch_sync(dispatch_get_main_queue(), ^{
				[a addObject:n];
			});
		}
	});
	dispatch_group_notify(g, dispatch_get_main_queue(), ^{
		test_long("count", [a count], final);
		test_long("description length", [[a description] length], desclen);
		[pool drain];
		test_stop_after_delay((void*)(intptr_t)1);
	});
	dispatch_release(g);
}

int
main(void)
{
	dispatch_test_start("Dispatch Sync GC"); // <rdar://problem/7458685>
	dispatch_async_f(dispatch_get_main_queue(), NULL, work);
	CFRunLoopRun();
	return 0;
}
