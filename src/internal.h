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

#ifndef __DISPATCH_INTERNAL__
#define __DISPATCH_INTERNAL__

#define __DISPATCH_BUILDING_DISPATCH__
#define __DISPATCH_INDIRECT__
#include "dispatch.h"
#include "base.h"
#include "time.h"
#include "queue.h"
#include "object.h"
#include "source.h"
#include "group.h"
#include "semaphore.h"
#include "once.h"
#include "benchmark.h"

/* private.h uses #include_next and must be included last to avoid picking
 * up installed headers. */
#include "queue_private.h"
#include "source_private.h"
#include "private.h"
#include "legacy.h"
/* More #includes at EOF (dependent on the contents of internal.h) ... */

/* The "_debug" library build */
#ifndef DISPATCH_DEBUG
#define DISPATCH_DEBUG 0
#endif


#include <libkern/OSCrossEndian.h>
#include <libkern/OSAtomic.h>
#include <mach/boolean.h>
#include <mach/clock_types.h>
#include <mach/clock.h>
#include <mach/exception.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_host.h>
#include <mach/mach_interface.h>
#include <mach/mach_time.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <mach/mig_errors.h>
#include <mach/host_info.h>
#include <mach/notify.h>
#include <malloc/malloc.h>
#include <sys/event.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syslimits.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __BLOCKS__
#include <Block_private.h>
#include <Block.h>
#endif /* __BLOCKS__ */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <search.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define DISPATCH_NOINLINE	__attribute__((noinline))

// workaround 6368156
#ifdef NSEC_PER_SEC
#undef NSEC_PER_SEC
#endif
#ifdef USEC_PER_SEC
#undef USEC_PER_SEC
#endif
#ifdef NSEC_PER_USEC
#undef NSEC_PER_USEC
#endif
#define NSEC_PER_SEC 1000000000ull
#define USEC_PER_SEC 1000000ull
#define NSEC_PER_USEC 1000ull

/* I wish we had __builtin_expect_range() */
#define fastpath(x)	((typeof(x))__builtin_expect((long)(x), ~0l))
#define slowpath(x)	((typeof(x))__builtin_expect((long)(x), 0l))

void _dispatch_bug(size_t line, long val) __attribute__((__noinline__));
void _dispatch_abort(size_t line, long val) __attribute__((__noinline__,__noreturn__));
void _dispatch_log(const char *msg, ...) __attribute__((__noinline__,__format__(printf,1,2)));
void _dispatch_logv(const char *msg, va_list) __attribute__((__noinline__,__format__(printf,1,0)));

/*
 * For reporting bugs within libdispatch when using the "_debug" version of the library.
 */
#define dispatch_assert(e)	do {	\
		if (__builtin_constant_p(e)) {	\
			char __compile_time_assert__[(bool)(e) ? 1 : -1] __attribute__((unused));	\
		} else {	\
			typeof(e) _e = fastpath(e); /* always eval 'e' */	\
			if (DISPATCH_DEBUG && !_e) {	\
				_dispatch_abort(__LINE__, (long)_e);	\
			}	\
		}	\
	} while (0)
/* A lot of API return zero upon success and not-zero on fail. Let's capture and log the non-zero value */
#define dispatch_assert_zero(e)	do {	\
		if (__builtin_constant_p(e)) {	\
			char __compile_time_assert__[(bool)(!(e)) ? 1 : -1] __attribute__((unused));	\
		} else {	\
			typeof(e) _e = slowpath(e); /* always eval 'e' */	\
			if (DISPATCH_DEBUG && _e) {	\
				_dispatch_abort(__LINE__, (long)_e);	\
			}	\
		}	\
	} while (0)

/*
 * For reporting bugs or impedance mismatches between libdispatch and external subsystems.
 * These do NOT abort(), and are always compiled into the product.
 *
 * In particular, we wrap all system-calls with assume() macros.
 */
#define dispatch_assume(e)	({	\
		typeof(e) _e = fastpath(e); /* always eval 'e' */	\
		if (!_e) {	\
			if (__builtin_constant_p(e)) {	\
				char __compile_time_assert__[(e) ? 1 : -1];	\
				(void)__compile_time_assert__;	\
			}	\
			_dispatch_bug(__LINE__, (long)_e);	\
		}	\
		_e;	\
	})
/* A lot of API return zero upon success and not-zero on fail. Let's capture and log the non-zero value */
#define dispatch_assume_zero(e)	({	\
		typeof(e) _e = slowpath(e); /* always eval 'e' */	\
		if (_e) {	\
			if (__builtin_constant_p(e)) {	\
				char __compile_time_assert__[(e) ? -1 : 1];	\
				(void)__compile_time_assert__;	\
			}	\
			_dispatch_bug(__LINE__, (long)_e);	\
		}	\
		_e;	\
	})

/*
 * For reporting bugs in clients when using the "_debug" version of the library.
 */
#define dispatch_debug_assert(e, msg, args...)	do {	\
		if (__builtin_constant_p(e)) {	\
			char __compile_time_assert__[(bool)(e) ? 1 : -1] __attribute__((unused));	\
		} else {	\
			typeof(e) _e = fastpath(e); /* always eval 'e' */	\
			if (DISPATCH_DEBUG && !_e) {	\
				_dispatch_log("%s() 0x%lx: " msg, __func__, (long)_e, ##args);	\
				abort();	\
			}	\
		}	\
	} while (0)



#ifdef __BLOCKS__
dispatch_block_t _dispatch_Block_copy(dispatch_block_t block);
void _dispatch_call_block_and_release(void *block);
void _dispatch_call_block_and_release2(void *block, void *ctxt);
#endif /* __BLOCKS__ */

void dummy_function(void);
long dummy_function_r0(void);


/* Make sure the debug statments don't get too stale */
#define _dispatch_debug(x, args...)	\
({	\
	if (DISPATCH_DEBUG) {	\
		_dispatch_log("libdispatch: %u\t%p\t" x, __LINE__, _dispatch_thread_self(), ##args);	\
	}	\
})


#if DISPATCH_DEBUG
void dispatch_debug_kevents(struct kevent* kev, size_t count, const char* str);
#else
#define dispatch_debug_kevents(x, y, z)
#endif

uint64_t _dispatch_get_nanoseconds(void);

void _dispatch_source_drain_kevent(struct kevent *);

dispatch_source_t
_dispatch_source_create2(dispatch_source_t ds,
	dispatch_source_attr_t attr,
	void *context,
	dispatch_source_handler_function_t handler);

void _dispatch_update_kq(const struct kevent *);
void _dispatch_run_timers(void);
// Returns howsoon with updated time value, or NULL if no timers active.
struct timespec *_dispatch_get_next_timer_fire(struct timespec *howsoon);

dispatch_semaphore_t _dispatch_get_thread_semaphore(void);
void _dispatch_put_thread_semaphore(dispatch_semaphore_t);

bool _dispatch_source_testcancel(dispatch_source_t);

uint64_t _dispatch_timeout(dispatch_time_t when);

__private_extern__ bool _dispatch_safe_fork;

__private_extern__ struct _dispatch_hw_config_s {
	uint32_t cc_max_active;
	uint32_t cc_max_logical;
	uint32_t cc_max_physical;
} _dispatch_hw_config;

/* #includes dependent on internal.h */
#include "object_internal.h"
#include "hw_shims.h"
#include "os_shims.h"
#include "queue_internal.h"
#include "semaphore_internal.h"
#include "source_internal.h"

// MIG_REPLY_MISMATCH means either:
// 1) A signal handler is NOT using async-safe API. See the sigaction(2) man page for more info.
// 2) A hand crafted call to mach_msg*() screwed up. Use MIG.
#define DISPATCH_VERIFY_MIG(x) do {	\
		if ((x) == MIG_REPLY_MISMATCH) {	\
			__crashreporter_info__ = "MIG_REPLY_MISMATCH";	\
			_dispatch_hardware_crash();	\
		}	\
	} while (0)

#if defined(__x86_64__) || defined(__i386__)
// total hack to ensure that return register of a function is not trashed
#define DISPATCH_CRASH(x)	do {	\
		asm("mov	%1, %0" : "=m" (__crashreporter_info__) : "c" ("BUG IN LIBDISPATCH: " x));	\
		_dispatch_hardware_crash();	\
	} while (0)

#define DISPATCH_CLIENT_CRASH(x)	do {	\
		asm("mov	%1, %0" : "=m" (__crashreporter_info__) : "c" ("BUG IN CLIENT OF LIBDISPATCH: " x));	\
		_dispatch_hardware_crash();	\
	} while (0)

#else

#define DISPATCH_CRASH(x)	do {	\
		__crashreporter_info__ = "BUG IN LIBDISPATCH: " x;	\
		_dispatch_hardware_crash();	\
	} while (0)

#define DISPATCH_CLIENT_CRASH(x)	do {	\
		__crashreporter_info__ = "BUG IN CLIENT OF LIBDISPATCH: " x;	\
		_dispatch_hardware_crash();	\
	} while (0)

#endif


#endif /* __DISPATCH_INTERNAL__ */
