/*
 * Copyright (c) 2008-2010 Apple Inc. All rights reserved.
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

uint64_t
_dispatch_get_nanoseconds(void)
{
	struct timeval now;
	int r = gettimeofday(&now, NULL);
	dispatch_assert_zero(r);
	dispatch_assert(sizeof(NSEC_PER_SEC) == 8);
	dispatch_assert(sizeof(NSEC_PER_USEC) == 8);
	return now.tv_sec * NSEC_PER_SEC + now.tv_usec * NSEC_PER_USEC;
}

#if !(defined(__i386__) || defined(__x86_64__) || !HAVE_MACH_ABSOLUTE_TIME)
DISPATCH_CACHELINE_ALIGN _dispatch_host_time_data_s _dispatch_host_time_data;

void
_dispatch_get_host_time_init(void *context DISPATCH_UNUSED)
{
	mach_timebase_info_data_t tbi;
	(void)dispatch_assume_zero(mach_timebase_info(&tbi));
	_dispatch_host_time_data.frac = tbi.numer;
	_dispatch_host_time_data.frac /= tbi.denom;
	_dispatch_host_time_data.ratio_1_to_1 = (tbi.numer == tbi.denom);
}
#endif

dispatch_time_t
dispatch_time(dispatch_time_t inval, int64_t delta)
{
	if (inval == DISPATCH_TIME_FOREVER) {
		return DISPATCH_TIME_FOREVER;
	}
	if ((int64_t)inval < 0) {
		// wall clock
		if (delta >= 0) {
			if ((int64_t)(inval -= delta) >= 0) {
				return DISPATCH_TIME_FOREVER; // overflow
			}
			return inval;
		}
		if ((int64_t)(inval -= delta) >= -1) {
			// -1 is special == DISPATCH_TIME_FOREVER == forever
			return -2; // underflow
		}
		return inval;
	}
	// mach clock
	delta = _dispatch_time_nano2mach(delta);
	if (inval == 0) {
		inval = _dispatch_absolute_time();
	}
	if (delta >= 0) {
		if ((int64_t)(inval += delta) <= 0) {
			return DISPATCH_TIME_FOREVER; // overflow
		}
		return inval;
	}
	if ((int64_t)(inval += delta) < 1) {
		return 1; // underflow
	}
	return inval;
}

dispatch_time_t
dispatch_walltime(const struct timespec *inval, int64_t delta)
{
	int64_t nsec;

	if (inval) {
		nsec = inval->tv_sec * 1000000000ull + inval->tv_nsec;
	} else {
		nsec = _dispatch_get_nanoseconds();
	}

	nsec += delta;
	if (nsec <= 1) {
		// -1 is special == DISPATCH_TIME_FOREVER == forever
		return delta >= 0 ? DISPATCH_TIME_FOREVER : (uint64_t)-2ll;
	}

	return -nsec;
}

uint64_t
_dispatch_timeout(dispatch_time_t when)
{
	uint64_t now;

	if (when == DISPATCH_TIME_FOREVER) {
		return DISPATCH_TIME_FOREVER;
	}
	if (when == 0) {
		return 0;
	}
	if ((int64_t)when < 0) {
		when = -(int64_t)when;
		now = _dispatch_get_nanoseconds();
		return now >= when ? 0 : when - now;
	}
	now = _dispatch_absolute_time();
	return now >= when ? 0 : _dispatch_time_mach2nano(when - now);
}
