/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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

#ifndef __DISPATCH_SWIFT_CONCURRENCY_PRIVATE__
#define __DISPATCH_SWIFT_CONCURRENCY_PRIVATE__

#include <dispatch/dispatch.h>

#ifndef __BEGIN_DECLS
#if defined(__cplusplus)
#define	__BEGIN_DECLS extern "C" {
#define	__END_DECLS }
#else
#define	__BEGIN_DECLS
#define	__END_DECLS
#endif
#endif

#include <pthread/private.h>
#include <mach/mach.h>

/*
 * IMPORTANT: This header file describes PRIVATE interfaces between libdispatch
 * and the swift concurrency runtime which are subject to change in future
 * releases of Mac OS X. No other client can use or rely on these interfaces.
 */

/*
 * The encoding of a dispatch_tid_t and a dispatch_lock_t for Apple platforms
 * only.
 *
 * This is the PRIVATE representation of a lock and thread that is understood by
 * just dispatch, the kernel and the Swift concurrency runtime and is used to
 * track a thread that is executing a swift job - be it a task or an actor.
 *
 * THIS SHOULD NOT BE USED BY ANYONE THAT IS NOT THE SWIFT CONCURRENCY RUNTIME.
 */
DISPATCH_ASSUME_NONNULL_BEGIN
__BEGIN_DECLS

typedef mach_port_t dispatch_tid_t;
typedef uint32_t dispatch_lock_t;

#if !defined(__DISPATCH_BUILDING_DISPATCH__)
#define DLOCK_OWNER_MASK			((dispatch_lock_t)0xfffffffc)
// These bottom two bits are free and can be used by the Concurrency runtime for
// tracking other information
#define DLOCK_FREE_BITS_MASK		((dispatch_lock_t)0x00000003)

#define DLOCK_OWNER_NULL			((dispatch_tid_t)MACH_PORT_NULL)
#define dispatch_tid_self()			((dispatch_tid_t)_pthread_mach_thread_self_direct())

DISPATCH_ALWAYS_INLINE
static inline dispatch_tid_t
dispatch_lock_owner(dispatch_lock_t lock_value)
{
	if (lock_value & DLOCK_OWNER_MASK) {
		return lock_value | DLOCK_FREE_BITS_MASK;
	}
	return DLOCK_OWNER_NULL;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_lock_t
dispatch_lock_value_from_tid(dispatch_tid_t tid)
{
	return tid & DLOCK_OWNER_MASK;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_lock_t
dispatch_lock_value_for_self(void)
{
	return dispatch_lock_value_from_tid(dispatch_tid_self());
}

DISPATCH_ALWAYS_INLINE
static inline bool
dispatch_lock_is_locked(dispatch_lock_t lock_value)
{
	// equivalent to dispatch_lock_owner(lock_value) == 0
	return (lock_value & DLOCK_OWNER_MASK) != 0;
}

DISPATCH_ALWAYS_INLINE
static inline bool
dispatch_lock_is_locked_by(dispatch_lock_t lock_value, dispatch_tid_t tid)
{
	// equivalent to dispatch_lock_owner(lock_value) == tid
	return ((lock_value ^ tid) & DLOCK_OWNER_MASK) == 0;
}

DISPATCH_ALWAYS_INLINE
static inline bool
dispatch_lock_is_locked_by_self(dispatch_lock_t lock_value)
{
	// equivalent to dispatch_lock_owner(lock_value) == self_tid
	return ((lock_value ^ dispatch_tid_self()) & DLOCK_OWNER_MASK) == 0;
}
#else
// This file is exporting the encoding of dispatch_lock and a subset of the
// functions vended by shims/lock.h for the use of Swift concurrency. We will be
// using the internal functions for dispatch's own internal usage.
#endif


/*!
 * @typedef dispatch_thread_override_info_s
 *
 * @abstract
 * Structure for returning information about the thread's current override
 * status
 */
typedef struct dispatch_thread_override_info {
	uint32_t can_override:1,
		unused:31;
	qos_class_t override_qos_floor;
} dispatch_thread_override_info_s;

/*!
 * @function dispatch_thread_get_current_override_qos_floor
 *
 * @abstract
 * Returns information about whether the current thread can be overridden and if
 * so, what it's current override floor is.
 */
SPI_AVAILABLE(macos(13.0), ios(16.0))
DISPATCH_EXPORT
dispatch_thread_override_info_s
dispatch_thread_get_current_override_qos_floor(void);

/*!
 * @function dispatch_thread_override_self
 *
 * @abstract
 * If the current thread can be overridden, this function applies the input QoS
 * as an override to itself.
 *
 * @param override_qos
 * The override to apply to the current thread
 */
SPI_AVAILABLE(macos(13.0), ios(16.0))
DISPATCH_EXPORT
int
dispatch_thread_override_self(qos_class_t override_qos);

/*!
 * @function dispatch_lock_override_start_with_debounce
 *
 * @abstract
 * This function applies a dispatch workqueue override of the specified QoS on
 * the thread if the lock owner tracked in the swift object's atomic state
 * matches the expected thread. Otherwise, it returns -1 with an errno set.
 *
 * @param lock_addr
 * The address to the lock used to synchronize the swift object. This lock
 * should follow the encoding of the dispatch_lock mentioned above.
 *
 * @param expected_thread
 * The thread which is expected to be the owner of the swift object. If this
 * does not match the thread which is tracked in (*lock_addr)
 *
 * @param override_to_apply
 * The QoS override to apply on the thread
 */
SPI_AVAILABLE(macos(13.0), ios(16.0))
DISPATCH_EXPORT DISPATCH_NONNULL1
int
dispatch_lock_override_start_with_debounce(dispatch_lock_t *lock_addr,
	dispatch_tid_t expected_thread, qos_class_t override_to_apply);

/*!
 * @function dispatch_lock_override_end
 *
 * @abstract
 * This function asynchronously removes the qos override that has been applied
 * on the calling thread from a previous call to
 * dispatch_lock_override_start_with_debounce.
 *
 * @param override_to_end
 * The QoS override on the current thread.
 */
SPI_AVAILABLE(macos(13.0), ios(16.0))
DISPATCH_EXPORT
int
dispatch_lock_override_end(qos_class_t override_to_end);

__END_DECLS
DISPATCH_ASSUME_NONNULL_END
#endif
