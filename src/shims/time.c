/*
 * Copyright (c) 2008-2009 Apple Inc. All rights reserved.
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

#include "internal.h"

// for architectures that don't always return mach_absolute_time() in nanoseconds
#if !(defined(__i386__) || defined(__x86_64__) || !defined(HAVE_MACH_ABSOLUTE_TIME))
_dispatch_host_time_data_s _dispatch_host_time_data;

void
_dispatch_get_host_time_init(void *context __attribute__((unused)))
{
	mach_timebase_info_data_t tbi;
	(void)dispatch_assume_zero(mach_timebase_info(&tbi));
	_dispatch_host_time_data.frac = tbi.numer;
	_dispatch_host_time_data.frac /= tbi.denom;
	_dispatch_host_time_data.ratio_1_to_1 = (tbi.numer == tbi.denom);
}
#endif
