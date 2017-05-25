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

#ifndef __DISPATCH_OS_SHIMS__
#define __DISPATCH_OS_SHIMS__

#include <pthread.h>
#ifdef __linux__
#include "shims/linux_stubs.h"
#endif

#ifdef __ANDROID__
#include "shims/android_stubs.h"
#endif

#include "shims/priority.h"

#if HAVE_PTHREAD_WORKQUEUES
#if __has_include(<pthread/workqueue_private.h>)
#include <pthread/workqueue_private.h>
#else
#include <pthread_workqueue.h>
#endif
#ifndef WORKQ_FEATURE_MAINTENANCE
#define WORKQ_FEATURE_MAINTENANCE 0x10
#endif
#endif // HAVE_PTHREAD_WORKQUEUES

#if DISPATCH_USE_INTERNAL_WORKQUEUE
#include "event/workqueue_internal.h"
#endif

#if HAVE_PTHREAD_NP_H
#include <pthread_np.h>
#endif

#if __has_include(<pthread/private.h>)
#include <pthread/private.h>
#endif

#if !HAVE_DECL_FD_COPY
#define FD_COPY(f, t) (void)(*(t) = *(f))
#endif

#if TARGET_OS_WIN32
#define bzero(ptr,len) memset((ptr), 0, (len))
#define snprintf _snprintf

inline size_t strlcpy(char *dst, const char *src, size_t size) {
	int res = strlen(dst) + strlen(src) + 1;
	if (size > 0) {
		size_t n = size - 1;
		strncpy(dst, src, n);
		dst[n] = 0;
	}
	return res;
}
#endif // TARGET_OS_WIN32

#if PTHREAD_WORKQUEUE_SPI_VERSION < 20140716
static inline int
_pthread_workqueue_override_start_direct(mach_port_t thread,
		pthread_priority_t priority)
{
	(void)thread; (void)priority;
	return 0;
}
#endif // PTHREAD_WORKQUEUE_SPI_VERSION < 20140716

#if PTHREAD_WORKQUEUE_SPI_VERSION < 20150319
static inline int
_pthread_workqueue_override_start_direct_check_owner(mach_port_t thread,
		pthread_priority_t priority, mach_port_t *ulock_addr)
{
	(void)ulock_addr;
	return _pthread_workqueue_override_start_direct(thread, priority);
}
#endif // PTHREAD_WORKQUEUE_SPI_VERSION < 20150319

#if PTHREAD_WORKQUEUE_SPI_VERSION < 20140707
static inline int
_pthread_override_qos_class_start_direct(mach_port_t thread,
		pthread_priority_t priority)
{
	(void)thread; (void)priority;
	return 0;
}

static inline int
_pthread_override_qos_class_end_direct(mach_port_t thread)
{
	(void)thread;
	return 0;
}
#endif // PTHREAD_WORKQUEUE_SPI_VERSION < 20140707

#if PTHREAD_WORKQUEUE_SPI_VERSION < 20150325
static inline int
_pthread_qos_override_start_direct(mach_port_t thread,
		pthread_priority_t priority, void *resource)
{
	(void)resource;
	return _pthread_override_qos_class_start_direct(thread, priority);
}

static inline int
_pthread_qos_override_end_direct(mach_port_t thread, void *resource)
{
	(void)resource;
	return _pthread_override_qos_class_end_direct(thread);
}
#endif // PTHREAD_WORKQUEUE_SPI_VERSION < 20150325

#if PTHREAD_WORKQUEUE_SPI_VERSION < 20160427
#define _PTHREAD_SET_SELF_WQ_KEVENT_UNBIND 0
#endif

#if PTHREAD_WORKQUEUE_SPI_VERSION < 20160427
static inline bool
_pthread_workqueue_should_narrow(pthread_priority_t priority)
{
	(void)priority;
	return false;
}
#endif

#if !HAVE_NORETURN_BUILTIN_TRAP
/*
 * XXXRW: Work-around for possible clang bug in which __builtin_trap() is not
 * marked noreturn, leading to a build error as dispatch_main() *is* marked
 * noreturn. Mask by marking __builtin_trap() as noreturn locally.
 */
DISPATCH_NORETURN
void __builtin_trap(void);
#endif

#if DISPATCH_HW_CONFIG_UP
#define OS_ATOMIC_UP 1
#else
#define OS_ATOMIC_UP 0
#endif


#ifndef __OS_INTERNAL_ATOMIC__
#include "shims/atomic.h"
#endif
#define DISPATCH_ATOMIC64_ALIGN  __attribute__((aligned(8)))

#include "shims/atomic_sfb.h"
#include "shims/tsd.h"
#include "shims/yield.h"
#include "shims/lock.h"

#include "shims/hw_config.h"
#include "shims/perfmon.h"

#include "shims/getprogname.h"
#include "shims/time.h"

#if __has_include(<os/overflow.h>)
#include <os/overflow.h>
#elif __has_builtin(__builtin_add_overflow)
#define os_add_overflow(a, b, c) __builtin_add_overflow(a, b, c)
#define os_sub_overflow(a, b, c) __builtin_sub_overflow(a, b, c)
#define os_mul_overflow(a, b, c) __builtin_mul_overflow(a, b, c)
#else
#error unsupported compiler
#endif

#ifndef os_mul_and_add_overflow
#define os_mul_and_add_overflow(a, x, b, res) __extension__({ \
	__typeof(*(res)) _tmp; \
	bool _s, _t; \
	_s = os_mul_overflow((a), (x), &_tmp); \
	_t = os_add_overflow((b), _tmp, (res)); \
	_s | _t; \
})
#endif


#if __has_feature(c_static_assert)
#define __dispatch_is_array(x) \
	_Static_assert(!__builtin_types_compatible_p(typeof((x)[0]) *, typeof(x)), \
				#x " isn't an array")
#define countof(x) \
	({ __dispatch_is_array(x); sizeof(x) / sizeof((x)[0]); })
#else
#define countof(x) (sizeof(x) / sizeof(x[0]))
#endif

DISPATCH_ALWAYS_INLINE
static inline void *
_dispatch_mempcpy(void *ptr, const void *data, size_t len)
{
	memcpy(ptr, data, len);
	return (char *)ptr + len;
}
#define _dispatch_memappend(ptr, e) \
	_dispatch_mempcpy(ptr, e, sizeof(*(e)))

#ifdef __APPLE__
// Clear the stack before calling long-running thread-handler functions that
// never return (and don't take arguments), to facilitate leak detection and
// provide cleaner backtraces. <rdar://problem/9050566>
#define _dispatch_clear_stack(s) do { \
		void *a[(s)/sizeof(void*) ? (s)/sizeof(void*) : 1]; \
		a[0] = pthread_get_stackaddr_np(pthread_self()); \
		bzero((void*)&a[1], (size_t)(a[0] - (void*)&a[1])); \
	} while (0)
#else
#define _dispatch_clear_stack(s)
#endif

#endif
