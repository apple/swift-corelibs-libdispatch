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
#if HAVE_PTHREAD_WORKQUEUES
#include <pthread_workqueue.h>
#endif
#if HAVE_PTHREAD_NP_H
#include <pthread_np.h>
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
#define DISPATCH_ATOMIC_UP 1
#endif

#include "shims/atomic.h"
#include "shims/atomic_sfb.h"
#include "shims/tsd.h"
#include "shims/hw_config.h"
#include "shims/perfmon.h"

#include "shims/getprogname.h"
#include "shims/time.h"

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
