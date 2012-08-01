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
#include <libkern/OSAtomic.h>

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
	OSSpinLock sl = OS_SPINLOCK_INIT, *l = &sl;

	dispatch_apply(final, dispatch_get_global_queue(0, 0), ^(size_t i){
		NSDecimalNumber *n = [NSDecimalNumber decimalNumberWithDecimal:
				[[NSNumber numberWithInteger:i] decimalValue]];
		OSSpinLockLock(l);
		[a addObject:n];
		OSSpinLockUnlock(l);
	});
	test_long("count", [a count], final);
	test_long("description length", [[a description] length], desclen);
	a = nil;
	[pool drain];
	test_stop_after_delay((void*)(intptr_t)1);
}

int
main(void)
{
	dispatch_test_start("Dispatch Apply GC"); // <rdar://problem/7455071>
	dispatch_async_f(dispatch_get_main_queue(), (void*)(intptr_t)1, work);
	CFRunLoopRun();
	return 0;
}
