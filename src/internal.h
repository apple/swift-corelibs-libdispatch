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

#ifndef __DISPATCH_INTERNAL__
#define __DISPATCH_INTERNAL__

#include <config/config.h>

#define __DISPATCH_BUILDING_DISPATCH__
#define __DISPATCH_INDIRECT__

#ifdef __APPLE__
#include <Availability.h>
#include <TargetConditionals.h>
#endif


#if !defined(DISPATCH_MACH_SPI) && TARGET_OS_MAC
#define DISPATCH_MACH_SPI 1
#endif

#if !defined(USE_OBJC) && HAVE_OBJC
#define USE_OBJC 1
#endif

#if USE_OBJC && ((!TARGET_IPHONE_SIMULATOR && defined(__i386__)) || \
		(!TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED < 1080))
// Disable Objective-C support on platforms with legacy objc runtime
#undef USE_OBJC
#define USE_OBJC 0
#endif

#if USE_OBJC
#define OS_OBJECT_HAVE_OBJC_SUPPORT 1
#if __OBJC__
#define OS_OBJECT_USE_OBJC 1
#else
#define OS_OBJECT_USE_OBJC 0
#endif // __OBJC__
#else
#define OS_OBJECT_HAVE_OBJC_SUPPORT 0
#endif // USE_OBJC

#include <dispatch/dispatch.h>
#include <dispatch/base.h>


#include <os/object.h>
#include <dispatch/object.h>
#include <dispatch/time.h>
#include <dispatch/queue.h>
#include <dispatch/source.h>
#include <dispatch/group.h>
#include <dispatch/semaphore.h>
#include <dispatch/once.h>
#include <dispatch/data.h>
#if !TARGET_OS_WIN32
#include <dispatch/io.h>
#endif

#define DISPATCH_STRUCT_DECL(type, name, ...) \
	struct type __VA_ARGS__ name

// Visual Studio C++ does not support C99 designated initializers.
// This means that static declarations should be zero initialized and cannot
// be const since we must fill in the values during DLL initialization.
#if !TARGET_OS_WIN32
#define DISPATCH_STRUCT_INSTANCE(type, name, ...) \
struct type name = { \
__VA_ARGS__ \
}
#else
#define DISPATCH_STRUCT_INSTANCE(type, name, ...) \
struct type name = { 0 }
#endif

#if !TARGET_OS_WIN32
#define DISPATCH_CONST_STRUCT_DECL(type, name, ...) \
	const DISPATCH_STRUCT_DECL(type, name, __VA_ARGS__)

#define DISPATCH_CONST_STRUCT_INSTANCE(type, name, ...) \
	const DISPATCH_STRUCT_INSTANCE(type, name, __VA_ARGS__)
#else
#define DISPATCH_CONST_STRUCT_DECL(type, name, ...) \
	DISPATCH_STRUCT_DECL(type, name, __VA_ARGS__)

#define DISPATCH_CONST_STRUCT_INSTANCE(type, name, ...) \
	DISPATCH_STRUCT_INSTANCE(type, name, __VA_ARGS__)
#endif

/* private.h must be included last to avoid picking up installed headers. */
#include "object_private.h"
#include "queue_private.h"
#include "source_private.h"
#include "mach_private.h"
#include "data_private.h"
#if !TARGET_OS_WIN32
#include "io_private.h"
#endif
#include "benchmark.h"
#include "private.h"

/* SPI for Libsystem-internal use */
DISPATCH_EXPORT DISPATCH_NOTHROW void libdispatch_init(void);
#if !TARGET_OS_WIN32
DISPATCH_EXPORT DISPATCH_NOTHROW void dispatch_atfork_prepare(void);
DISPATCH_EXPORT DISPATCH_NOTHROW void dispatch_atfork_parent(void);
DISPATCH_EXPORT DISPATCH_NOTHROW void dispatch_atfork_child(void);
#endif

/* More #includes at EOF (dependent on the contents of internal.h) ... */

// Abort on uncaught exceptions thrown from client callouts rdar://8577499
#if !defined(DISPATCH_USE_CLIENT_CALLOUT)
#define DISPATCH_USE_CLIENT_CALLOUT 1
#endif

/* The "_debug" library build */
#ifndef DISPATCH_DEBUG
#define DISPATCH_DEBUG 0
#endif

#ifndef DISPATCH_PROFILE
#define DISPATCH_PROFILE 0
#endif

#if (!TARGET_OS_EMBEDDED || DISPATCH_DEBUG || DISPATCH_PROFILE) && \
		!defined(DISPATCH_USE_DTRACE)
#define DISPATCH_USE_DTRACE 1
#endif

#if ((!TARGET_OS_EMBEDDED && DISPATCH_INTROSPECTION) || DISPATCH_DEBUG || \
		DISPATCH_PROFILE) && !defined(DISPATCH_USE_DTRACE_INTROSPECTION)
#define DISPATCH_USE_DTRACE_INTROSPECTION 1
#endif

#if HAVE_LIBKERN_OSCROSSENDIAN_H
#include <libkern/OSCrossEndian.h>
#endif
#if HAVE_LIBKERN_OSATOMIC_H
#include <libkern/OSAtomic.h>
#endif
#if HAVE_MACH
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
#include <mach/mach_vm.h>
#include <mach/vm_map.h>
#endif /* HAVE_MACH */
#if HAVE_MALLOC_MALLOC_H
#include <malloc/malloc.h>
#endif

#include <sys/stat.h>

#if !TARGET_OS_WIN32 
#include <sys/event.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <netinet/in.h>
#else
#include "sys_queue.h"
#endif

#ifdef __BLOCKS__
#include <Block_private.h>
#include <Block.h>
#endif /* __BLOCKS__ */

#include <assert.h>
#include <errno.h>
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <limits.h>
#include <search.h>
#if USE_POSIX_SEM
#include <semaphore.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !TARGET_OS_WIN32 
#include <syslog.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif
#ifndef __has_include
#define __has_include(x) 0
#endif
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#if __GNUC__
#define DISPATCH_NOINLINE __attribute__((__noinline__))
#define DISPATCH_USED __attribute__((__used__))
#define DISPATCH_UNUSED __attribute__((__unused__))
#define DISPATCH_WEAK __attribute__((__weak__))
#define DISPATCH_OVERLOADABLE __attribute__((__overloadable__))
#if DISPATCH_DEBUG
#define DISPATCH_ALWAYS_INLINE_NDEBUG
#else
#define DISPATCH_ALWAYS_INLINE_NDEBUG __attribute__((__always_inline__))
#endif
#else	/* __GNUC__ */
#define DISPATCH_NOINLINE
#define DISPATCH_USED
#define DISPATCH_UNUSED
#define DISPATCH_WEAK
#define DISPATCH_ALWAYS_INLINE_NDEBUG
#endif	/* __GNUC__ */

#define DISPATCH_CONCAT(x,y) DISPATCH_CONCAT1(x,y)
#define DISPATCH_CONCAT1(x,y) x ## y

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
#if __GNUC__
#define fastpath(x) ((typeof(x))__builtin_expect((long)(x), ~0l))
#define slowpath(x) ((typeof(x))__builtin_expect((long)(x), 0l))
#else
#define fastpath(x) (x)
#define slowpath(x) (x)
#endif // __GNUC__

DISPATCH_NOINLINE
void _dispatch_bug(size_t line, long val);

#if HAVE_MACH
DISPATCH_NOINLINE
void _dispatch_bug_client(const char* msg);
DISPATCH_NOINLINE
void _dispatch_bug_mach_client(const char *msg, mach_msg_return_t kr);
DISPATCH_NOINLINE
void _dispatch_bug_kevent_client(const char* msg, const char* filter,
		const char *operation, int err);
#endif

DISPATCH_NOINLINE DISPATCH_NORETURN
void _dispatch_abort(size_t line, long val);

#if !defined(DISPATCH_USE_OS_TRACE) && DISPATCH_DEBUG
#if __has_include(<os/trace.h>)
#define DISPATCH_USE_OS_TRACE 1
#include <os/trace.h>
#endif
#endif // DISPATCH_USE_OS_TRACE

#if DISPATCH_USE_OS_TRACE
#define _dispatch_log(msg, ...) os_trace("libdispatch", msg, ## __VA_ARGS__)
#else
DISPATCH_NOINLINE __attribute__((__format__(__printf__,1,2)))
void _dispatch_log(const char *msg, ...);
#endif // DISPATCH_USE_OS_TRACE

#define dsnprintf(...) \
		({ int _r = snprintf(__VA_ARGS__); _r < 0 ? 0u : (size_t)_r; })

/*
 * For reporting bugs within libdispatch when using the "_debug" version of the
 * library.
 */
#if __GNUC__
#define dispatch_assert(e) do { \
		if (__builtin_constant_p(e)) { \
			char __compile_time_assert__[(bool)(e) ? 1 : -1] DISPATCH_UNUSED; \
		} else { \
			typeof(e) _e = fastpath(e); /* always eval 'e' */ \
			if (DISPATCH_DEBUG && !_e) { \
				_dispatch_abort(__LINE__, (long)_e); \
			} \
		} \
	} while (0)
#else
static inline void _dispatch_assert(long e, long line) {
	if (DISPATCH_DEBUG && !e) _dispatch_abort(line, e);
}
#define dispatch_assert(e) _dispatch_assert((long)(e), __LINE__)
#endif	/* __GNUC__ */

#if __GNUC__
/*
 * A lot of API return zero upon success and not-zero on fail. Let's capture
 * and log the non-zero value
 */
#define dispatch_assert_zero(e) do { \
		if (__builtin_constant_p(e)) { \
			char __compile_time_assert__[(bool)(e) ? -1 : 1] DISPATCH_UNUSED; \
		} else { \
			typeof(e) _e = slowpath(e); /* always eval 'e' */ \
			if (DISPATCH_DEBUG && _e) { \
				_dispatch_abort(__LINE__, (long)_e); \
			} \
		} \
	} while (0)
#else
static inline void _dispatch_assert_zero(long e, long line) {
	if (DISPATCH_DEBUG && e) _dispatch_abort(line, e);
}
#define dispatch_assert_zero(e) _dispatch_assert((long)(e), __LINE__) 
#endif	/* __GNUC__ */

/*
 * For reporting bugs or impedance mismatches between libdispatch and external
 * subsystems. These do NOT abort(), and are always compiled into the product.
 *
 * In particular, we wrap all system-calls with assume() macros.
 */
#if __GNUC__
#define dispatch_assume(e) ({ \
		typeof(e) _e = fastpath(e); /* always eval 'e' */ \
		if (!_e) { \
			if (__builtin_constant_p(e)) { \
				char __compile_time_assert__[(bool)(e) ? 1 : -1]; \
				(void)__compile_time_assert__; \
			} \
			_dispatch_bug(__LINE__, (long)_e); \
		} \
		_e; \
	})
#else
static inline long _dispatch_assume(long e, long line) {
	if (!e) _dispatch_bug(line, e);
	return e;
}
#define dispatch_assume(e) _dispatch_assume((long)(e), __LINE__)
#endif	/* __GNUC__ */

/*
 * A lot of API return zero upon success and not-zero on fail. Let's capture
 * and log the non-zero value
 */
#if __GNUC__
#define dispatch_assume_zero(e) ({ \
		typeof(e) _e = slowpath(e); /* always eval 'e' */ \
		if (_e) { \
			if (__builtin_constant_p(e)) { \
				char __compile_time_assert__[(bool)(e) ? -1 : 1]; \
				(void)__compile_time_assert__; \
			} \
			_dispatch_bug(__LINE__, (long)_e); \
		} \
		_e; \
	})
#else
static inline long _dispatch_assume_zero(long e, long line) {
	if (e) _dispatch_bug(line, e);
	return e;
}
#define dispatch_assume_zero(e) _dispatch_assume_zero((long)(e), __LINE__)
#endif	/* __GNUC__ */

/*
 * For reporting bugs in clients when using the "_debug" version of the library.
 */
#if __GNUC__
#define dispatch_debug_assert(e, msg, args...) do { \
		if (__builtin_constant_p(e)) { \
			char __compile_time_assert__[(bool)(e) ? 1 : -1] DISPATCH_UNUSED; \
		} else { \
			typeof(e) _e = fastpath(e); /* always eval 'e' */ \
			if (DISPATCH_DEBUG && !_e) { \
				_dispatch_log("%s() 0x%lx: " msg, __func__, (long)_e, ##args); \
				abort(); \
			} \
		} \
	} while (0)
#else
#define dispatch_debug_assert(e, msg, args...) do { \
	long _e = (long)fastpath(e); /* always eval 'e' */ \
	if (DISPATCH_DEBUG && !_e) { \
		_dispatch_log("%s() 0x%lx: " msg, __FUNCTION__, _e, ##args); \
		abort(); \
	} \
} while (0)
#endif	/* __GNUC__ */

/* Make sure the debug statments don't get too stale */
#define _dispatch_debug(x, args...) do { \
	if (DISPATCH_DEBUG) { \
		_dispatch_log("%u\t%p\t" x, __LINE__, \
				(void *)_dispatch_thread_self(), ##args); \
	} \
} while (0)

#if DISPATCH_DEBUG
#if HAVE_MACH
DISPATCH_NOINLINE DISPATCH_USED
void dispatch_debug_machport(mach_port_t name, const char* str);
#endif
#endif

#if DISPATCH_DEBUG
/* This is the private version of the deprecated dispatch_debug() */
DISPATCH_NONNULL2 DISPATCH_NOTHROW
__attribute__((__format__(printf,2,3)))
void
_dispatch_object_debug(dispatch_object_t object, const char *message, ...);
#else
#define _dispatch_object_debug(object, message, ...)
#endif // DISPATCH_DEBUG

#if DISPATCH_USE_CLIENT_CALLOUT

DISPATCH_NOTHROW void
_dispatch_client_callout(void *ctxt, dispatch_function_t f);
DISPATCH_NOTHROW void
_dispatch_client_callout2(void *ctxt, size_t i, void (*f)(void *, size_t));
DISPATCH_NOTHROW bool
_dispatch_client_callout3(void *ctxt, dispatch_data_t region, size_t offset,
		const void *buffer, size_t size, dispatch_data_applier_function_t f);
DISPATCH_NOTHROW void
_dispatch_client_callout4(void *ctxt, dispatch_mach_reason_t reason,
		dispatch_mach_msg_t dmsg, mach_error_t error,
		dispatch_mach_handler_function_t f);

#else // !DISPATCH_USE_CLIENT_CALLOUT

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_client_callout(void *ctxt, dispatch_function_t f)
{
	return f(ctxt);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_client_callout2(void *ctxt, size_t i, void (*f)(void *, size_t))
{
	return f(ctxt, i);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_client_callout3(void *ctxt, dispatch_data_t region, size_t offset,
		const void *buffer, size_t size, dispatch_data_applier_function_t f)
{
	return f(ctxt, region, offset, buffer, size);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_client_callout4(void *ctxt, dispatch_mach_reason_t reason,
		dispatch_mach_msg_t dmsg, mach_error_t error,
		dispatch_mach_handler_function_t f);
{
	return f(ctxt, reason, dmsg, error);
}

#endif // !DISPATCH_USE_CLIENT_CALLOUT

#ifdef __BLOCKS__
#define _dispatch_Block_invoke(bb) \
		((dispatch_function_t)((struct Block_layout *)bb)->invoke)
DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_client_callout_block(dispatch_block_t b)
{
	return _dispatch_client_callout(b, _dispatch_Block_invoke(b));
}

#if __GNUC__
dispatch_block_t _dispatch_Block_copy(dispatch_block_t block);
#define _dispatch_Block_copy(x) ((typeof(x))_dispatch_Block_copy(x))
#else
dispatch_block_t _dispatch_Block_copy(const void *block);
#endif

void _dispatch_call_block_and_release(void *block);
#endif /* __BLOCKS__ */

void _dispatch_temporary_resource_shortage(void);
void *_dispatch_calloc(size_t num_items, size_t size);
void _dispatch_vtable_init(void);
char *_dispatch_get_build(void);

uint64_t _dispatch_timeout(dispatch_time_t when);

extern bool _dispatch_safe_fork, _dispatch_child_of_unsafe_fork;

extern struct _dispatch_hw_config_s {
	uint32_t cc_max_active;
	uint32_t cc_max_logical;
	uint32_t cc_max_physical;
} _dispatch_hw_config;

#if !defined(DISPATCH_USE_OS_SEMAPHORE_CACHE) && !(TARGET_IPHONE_SIMULATOR && \
		IPHONE_SIMULATOR_HOST_MIN_VERSION_REQUIRED < 1090)
#if __has_include(<os/semaphore_private.h>)
#define DISPATCH_USE_OS_SEMAPHORE_CACHE 1
#include <os/semaphore_private.h>
#endif
#endif

/* #includes dependent on internal.h */
#include "shims.h"

// Older Mac OS X and iOS Simulator fallbacks

#if HAVE_PTHREAD_WORKQUEUES
#ifndef WORKQ_BG_PRIOQUEUE
#define WORKQ_BG_PRIOQUEUE 3
#endif
#ifndef WORKQ_ADDTHREADS_OPTION_OVERCOMMIT
#define WORKQ_ADDTHREADS_OPTION_OVERCOMMIT 0x00000001
#endif
#if TARGET_IPHONE_SIMULATOR && IPHONE_SIMULATOR_HOST_MIN_VERSION_REQUIRED < 1070
#ifndef DISPATCH_NO_BG_PRIORITY
#define DISPATCH_NO_BG_PRIORITY 1
#endif
#endif
#if TARGET_IPHONE_SIMULATOR && IPHONE_SIMULATOR_HOST_MIN_VERSION_REQUIRED < 1080
#ifndef DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK
#define DISPATCH_USE_LEGACY_WORKQUEUE_FALLBACK 1
#endif
#endif
#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED < 1080
#undef HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP
#define HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP 0
#endif
#endif // HAVE_PTHREAD_WORKQUEUES

#if HAVE_MACH
#if !defined(MACH_NOTIFY_SEND_POSSIBLE) || (TARGET_IPHONE_SIMULATOR && \
		IPHONE_SIMULATOR_HOST_MIN_VERSION_REQUIRED < 1070)
#undef MACH_NOTIFY_SEND_POSSIBLE
#define MACH_NOTIFY_SEND_POSSIBLE MACH_NOTIFY_DEAD_NAME
#endif
#endif // HAVE_MACH

#ifdef EVFILT_VM
#ifndef DISPATCH_USE_VM_PRESSURE
#define DISPATCH_USE_VM_PRESSURE 1
#endif
#endif // EVFILT_VM

#ifdef EVFILT_MEMORYSTATUS
#ifndef DISPATCH_USE_MEMORYSTATUS
#define DISPATCH_USE_MEMORYSTATUS 1
#endif
#endif // EVFILT_MEMORYSTATUS

#if TARGET_IPHONE_SIMULATOR && IPHONE_SIMULATOR_HOST_MIN_VERSION_REQUIRED < 1070
#undef DISPATCH_USE_VM_PRESSURE_SOURCE
#define DISPATCH_USE_VM_PRESSURE_SOURCE 0
#endif // TARGET_IPHONE_SIMULATOR
#if TARGET_OS_EMBEDDED
#if !defined(DISPATCH_USE_VM_PRESSURE_SOURCE) && DISPATCH_USE_VM_PRESSURE
#define DISPATCH_USE_VM_PRESSURE_SOURCE 1
#endif
#else // !TARGET_OS_EMBEDDED
#if !defined(DISPATCH_USE_MEMORYSTATUS_SOURCE) && DISPATCH_USE_MEMORYSTATUS
#define DISPATCH_USE_MEMORYSTATUS_SOURCE 1
#elif !defined(DISPATCH_USE_VM_PRESSURE_SOURCE) && DISPATCH_USE_VM_PRESSURE
#define DISPATCH_USE_VM_PRESSURE_SOURCE 1
#endif
#endif // TARGET_OS_EMBEDDED

#if !defined(NOTE_LEEWAY) || (TARGET_IPHONE_SIMULATOR && \
		IPHONE_SIMULATOR_HOST_MIN_VERSION_REQUIRED < 1090)
#undef NOTE_LEEWAY
#define NOTE_LEEWAY 0
#undef NOTE_CRITICAL
#define NOTE_CRITICAL 0
#undef NOTE_BACKGROUND
#define NOTE_BACKGROUND 0
#endif // NOTE_LEEWAY

#if HAVE_DECL_NOTE_REAP
#if defined(NOTE_REAP) && defined(__APPLE__)
#undef NOTE_REAP
#define NOTE_REAP 0x10000000 // <rdar://problem/13338526>
#endif
#endif // HAVE_DECL_NOTE_REAP

#if defined(F_SETNOSIGPIPE) && defined(F_GETNOSIGPIPE)
#if TARGET_IPHONE_SIMULATOR && IPHONE_SIMULATOR_HOST_MIN_VERSION_REQUIRED < 1070
#undef DISPATCH_USE_SETNOSIGPIPE
#define DISPATCH_USE_SETNOSIGPIPE 0
#endif
#ifndef DISPATCH_USE_SETNOSIGPIPE
#define DISPATCH_USE_SETNOSIGPIPE 1
#endif
#endif // F_SETNOSIGPIPE


#if HAVE_LIBPROC_INTERNAL_H
#include <libproc.h>
#include <libproc_internal.h>
#if TARGET_IPHONE_SIMULATOR && IPHONE_SIMULATOR_HOST_MIN_VERSION_REQUIRED < 1090
#undef DISPATCH_USE_IMPORTANCE_ASSERTION
#define DISPATCH_USE_IMPORTANCE_ASSERTION 0
#endif
#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED < 1090
#undef DISPATCH_USE_IMPORTANCE_ASSERTION
#define DISPATCH_USE_IMPORTANCE_ASSERTION 0
#endif
#ifndef DISPATCH_USE_IMPORTANCE_ASSERTION
#define DISPATCH_USE_IMPORTANCE_ASSERTION 1
#endif
#endif // HAVE_LIBPROC_INTERNAL_H

#if HAVE_SYS_GUARDED_H
#include <sys/guarded.h>
#if TARGET_IPHONE_SIMULATOR && IPHONE_SIMULATOR_HOST_MIN_VERSION_REQUIRED < 1090
#undef DISPATCH_USE_GUARDED_FD
#define DISPATCH_USE_GUARDED_FD 0
#endif
#ifndef DISPATCH_USE_GUARDED_FD
#define DISPATCH_USE_GUARDED_FD 1
#endif
// change_fdguard_np() requires GUARD_DUP <rdar://problem/11814513>
#if DISPATCH_USE_GUARDED_FD && RDAR_11814513
#define DISPATCH_USE_GUARDED_FD_CHANGE_FDGUARD 1
#endif
#endif // HAVE_SYS_GUARDED_H


#define _dispatch_hardware_crash()	__builtin_trap()

#define _dispatch_set_crash_log_message(x)

#if HAVE_MACH
// MIG_REPLY_MISMATCH means either:
// 1) A signal handler is NOT using async-safe API. See the sigaction(2) man
//    page for more info.
// 2) A hand crafted call to mach_msg*() screwed up. Use MIG.
#define DISPATCH_VERIFY_MIG(x) do { \
		if ((x) == MIG_REPLY_MISMATCH) { \
			_dispatch_set_crash_log_message("MIG_REPLY_MISMATCH"); \
			_dispatch_hardware_crash(); \
		} \
	} while (0)
#endif

#define DISPATCH_CRASH(x) do { \
		_dispatch_set_crash_log_message("BUG IN LIBDISPATCH: " x); \
		_dispatch_hardware_crash(); \
	} while (0)

#define DISPATCH_CLIENT_CRASH(x) do { \
		_dispatch_set_crash_log_message("BUG IN CLIENT OF LIBDISPATCH: " x); \
		_dispatch_hardware_crash(); \
	} while (0)

#define _OS_OBJECT_CLIENT_CRASH(x) do { \
		_dispatch_set_crash_log_message("API MISUSE: " x); \
		_dispatch_hardware_crash(); \
	} while (0)

/* #includes dependent on internal.h */
#include "object_internal.h"
#include "semaphore_internal.h"
#include "introspection_internal.h"
#include "queue_internal.h"
#include "source_internal.h"
#include "data_internal.h"
#if !TARGET_OS_WIN32
#include "io_internal.h"
#endif
#include "trace.h"

#endif /* __DISPATCH_INTERNAL__ */
