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

/*
 * IMPORTANT: This header file describes INTERNAL interfaces to libdispatch
 * which are subject to change in future releases of Mac OS X. Any applications
 * relying on these interfaces WILL break.
 */

#ifndef __DISPATCH_SHIMS_TIME__
#define __DISPATCH_SHIMS_TIME__

#if defined(__i386__) || defined(__x86_64__) || !defined(HAVE_MACH_ABSOLUTE_TIME)
// these architectures always return mach_absolute_time() in nanoseconds
#define _dispatch_convert_mach2nano(x) (x)
#define _dispatch_convert_nano2mach(x) (x)
#else
extern uint64_t _dispatch_convert_mach2nano(uint64_t val);
extern uint64_t _dispatch_convert_nano2mach(uint64_t val);
#endif

static inline uint64_t
_dispatch_absolute_time(void)
{
#ifndef HAVE_MACH_ABSOLUTE_TIME
	struct timespec ts;
	int ret;

#if HAVE_DECL_CLOCK_UPTIME
	ret = clock_gettime(CLOCK_UPTIME, &ts);
#elif HAVE_DECL_CLOCK_MONOTONIC
	ret = clock_gettime(CLOCK_MONOTONIC, &ts);
#else
#error "clock_gettime: no supported absolute time clock"
#endif
	(void)dispatch_assume_zero(ret);

	/* XXXRW: Some kind of overflow detection needed? */
	return (ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec);
#else
	return mach_absolute_time();
#endif
}

#endif /* __DISPATCH_SHIMS_TIME__ */
