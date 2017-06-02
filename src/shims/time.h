/*
 * Copyright (c) 2008-2013 Apple Inc. All rights reserved.
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

#ifndef __DISPATCH_INDIRECT__
#error "Please #include <dispatch/dispatch.h> instead of this file directly."
#endif

#if TARGET_OS_WIN32
static inline unsigned int
sleep(unsigned int seconds)
{
	Sleep(seconds * 1000); // milliseconds
	return 0;
}
#endif

typedef enum {
	DISPATCH_CLOCK_WALL,
	DISPATCH_CLOCK_MACH,
#define DISPATCH_CLOCK_COUNT  (DISPATCH_CLOCK_MACH + 1)
} dispatch_clock_t;

void _dispatch_time_init(void);

#if defined(__i386__) || defined(__x86_64__) || !HAVE_MACH_ABSOLUTE_TIME
#define DISPATCH_TIME_UNIT_USES_NANOSECONDS 1
#else
#define DISPATCH_TIME_UNIT_USES_NANOSECONDS 0
#endif

#if DISPATCH_TIME_UNIT_USES_NANOSECONDS
// x86 currently implements mach time in nanoseconds
// this is NOT likely to change
DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dispatch_time_mach2nano(uint64_t machtime)
{
	return machtime;
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dispatch_time_nano2mach(uint64_t nsec)
{
	return nsec;
}
#else
#define DISPATCH_USE_HOST_TIME 1
extern uint64_t (*_dispatch_host_time_mach2nano)(uint64_t machtime);
extern uint64_t (*_dispatch_host_time_nano2mach)(uint64_t nsec);
static inline uint64_t
_dispatch_time_mach2nano(uint64_t machtime)
{
	return _dispatch_host_time_mach2nano(machtime);
}

static inline uint64_t
_dispatch_time_nano2mach(uint64_t nsec)
{
	return _dispatch_host_time_nano2mach(nsec);
}
#endif // DISPATCH_USE_HOST_TIME

/* XXXRW: Some kind of overflow detection needed? */
#define _dispatch_timespec_to_nano(ts) \
		((uint64_t)(ts).tv_sec * NSEC_PER_SEC + (uint64_t)(ts).tv_nsec)
#define _dispatch_timeval_to_nano(tv) \
		((uint64_t)(tv).tv_sec * NSEC_PER_SEC + \
				(uint64_t)(tv).tv_usec * NSEC_PER_USEC)

static inline uint64_t
_dispatch_get_nanoseconds(void)
{
	dispatch_static_assert(sizeof(NSEC_PER_SEC) == 8);
	dispatch_static_assert(sizeof(USEC_PER_SEC) == 8);

#if TARGET_OS_MAC
	return clock_gettime_nsec_np(CLOCK_REALTIME);
#elif HAVE_DECL_CLOCK_REALTIME
	struct timespec ts;
	dispatch_assume_zero(clock_gettime(CLOCK_REALTIME, &ts));
	return _dispatch_timespec_to_nano(ts);
#elif TARGET_OS_WIN32
	// FILETIME is 100-nanosecond intervals since January 1, 1601 (UTC).
	FILETIME ft;
	ULARGE_INTEGER li;
	GetSystemTimeAsFileTime(&ft);
	li.LowPart = ft.dwLowDateTime;
	li.HighPart = ft.dwHighDateTime;
	return li.QuadPart * 100ull;
#else
	struct timeval tv;
	dispatch_assert_zero(gettimeofday(&tv, NULL));
	return _dispatch_timeval_to_nano(tv);
#endif
}

static inline uint64_t
_dispatch_absolute_time(void)
{
#if HAVE_MACH_ABSOLUTE_TIME
	return mach_absolute_time();
#elif HAVE_DECL_CLOCK_UPTIME && !defined(__linux__)
	struct timespec ts;
	dispatch_assume_zero(clock_gettime(CLOCK_UPTIME, &ts));
	return _dispatch_timespec_to_nano(ts);
#elif HAVE_DECL_CLOCK_MONOTONIC
	struct timespec ts;
	dispatch_assume_zero(clock_gettime(CLOCK_MONOTONIC, &ts));
	return _dispatch_timespec_to_nano(ts);
#elif TARGET_OS_WIN32
	LARGE_INTEGER now;
	return QueryPerformanceCounter(&now) ? now.QuadPart : 0;
#else
#error platform needs to implement _dispatch_absolute_time()
#endif
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dispatch_approximate_time(void)
{
#if HAVE_MACH_APPROXIMATE_TIME
	return mach_approximate_time();
#elif HAVE_DECL_CLOCK_UPTIME_FAST && !defined(__linux__)
	struct timespec ts;
	dispatch_assume_zero(clock_gettime(CLOCK_UPTIME_FAST, &ts));
	return _dispatch_timespec_to_nano(ts);
#elif defined(__linux__)
	struct timespec ts;
	dispatch_assume_zero(clock_gettime(CLOCK_REALTIME_COARSE, &ts));
	return _dispatch_timespec_to_nano(ts);
#else
	return _dispatch_absolute_time();
#endif
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dispatch_time_now(dispatch_clock_t clock)
{
	switch (clock) {
	case DISPATCH_CLOCK_MACH:
		return _dispatch_absolute_time();
	case DISPATCH_CLOCK_WALL:
		return _dispatch_get_nanoseconds();
	}
	__builtin_unreachable();
}

typedef struct {
	uint64_t nows[DISPATCH_CLOCK_COUNT];
} dispatch_clock_now_cache_s, *dispatch_clock_now_cache_t;

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dispatch_time_now_cached(dispatch_clock_t clock,
		dispatch_clock_now_cache_t cache)
{
	if (likely(cache->nows[clock])) {
		return cache->nows[clock];
	}
	return cache->nows[clock] = _dispatch_time_now(clock);
}

#endif // __DISPATCH_SHIMS_TIME__
