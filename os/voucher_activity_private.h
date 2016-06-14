/*
 * Copyright (c) 2013-2015 Apple Inc. All rights reserved.
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

#ifndef __OS_VOUCHER_ACTIVITY_PRIVATE__
#define __OS_VOUCHER_ACTIVITY_PRIVATE__

#if OS_VOUCHER_ACTIVITY_SPI
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <firehose/tracepoint_private.h>
#endif
#ifndef __linux__
#include <os/base.h>
#endif
#include <os/object.h>
#include "voucher_private.h"

#define OS_VOUCHER_ACTIVITY_SPI_VERSION 20160329

#if OS_VOUCHER_WEAK_IMPORT
#define OS_VOUCHER_EXPORT OS_EXPORT OS_WEAK_IMPORT
#else
#define OS_VOUCHER_EXPORT OS_EXPORT
#endif

#define __VOUCHER_ACTIVITY_IGNORE_DEPRECATION_PUSH \
	_Pragma("clang diagnostic push") \
	_Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
#define __VOUCHER_ACTIVITY_IGNORE_DEPRECATION_POP \
	_Pragma("clang diagnostic pop")

__BEGIN_DECLS

/*!
 * @const VOUCHER_CURRENT
 * Shorthand for the currently adopted voucher
 *
 * This value can only be used as an argument to functions, and is never
 * actually returned. It looks enough like a tagged pointer object that ARC
 * won't crash if this is assigned to a temporary variable.
 */
#define VOUCHER_CURRENT		((OS_OBJECT_BRIDGE voucher_t)(void *)~2ul)

/*!
 * @function voucher_get_activity_id
 *
 * @abstract
 * Returns the activity_id associated with the specified voucher at the time
 * of the call.
 *
 * @discussion
 * When the passed voucher is VOUCHER_CURRENT this returns the current
 * activity ID.
 *
 * @param voucher
 * The specified voucher.
 *
 * @param parent_id
 * An out parameter to return the parent ID of the returned activity ID.
 *
 * @result
 * The current activity identifier, if any. When 0 is returned, parent_id will
 * also always be 0.
 */
__OSX_AVAILABLE(10.12) __IOS_AVAILABLE(10.0)
__TVOS_AVAILABLE(10.0) __WATCHOS_AVAILABLE(3.0)
OS_VOUCHER_EXPORT OS_NOTHROW
firehose_activity_id_t
voucher_get_activity_id(voucher_t voucher, firehose_activity_id_t *parent_id);

/*!
 * @function voucher_get_activity_id_and_creator
 *
 * @abstract
 * Returns the activity_id associated with the specified voucher at the time
 * of the call.
 *
 * @discussion
 * When the passed voucher is VOUCHER_CURRENT this returns the current
 * activity ID.
 *
 * @param voucher
 * The specified voucher.
 *
 * @param creator_pid
 * The unique pid of the process that created the returned activity ID if any.
 *
 * @param parent_id
 * An out parameter to return the parent ID of the returned activity ID.
 *
 * @result
 * The current activity identifier, if any. When 0 is returned, parent_id will
 * also always be 0.
 */
__OSX_AVAILABLE(10.12) __IOS_AVAILABLE(10.0)
__TVOS_AVAILABLE(10.0) __WATCHOS_AVAILABLE(3.0)
OS_VOUCHER_EXPORT OS_NOTHROW
firehose_activity_id_t
voucher_get_activity_id_and_creator(voucher_t voucher, uint64_t *creator_pid,
		firehose_activity_id_t *parent_id);

/*!
 * @function voucher_activity_create
 *
 * @abstract
 * Creates a voucher object with a new activity identifier.
 *
 * @discussion
 * As part of voucher transport, activities are automatically propagated by the
 * system to other threads and processes (across IPC).
 *
 * When a voucher with an activity identifier is applied to a thread, work
 * on that thread is done on behalf of this activity.
 *
 * @param trace_id
 * Tracepoint identifier returned by voucher_activity_trace_id(), intended for
 * identification of the automatic tracepoint generated as part of creating the
 * new activity.
 *
 * @param base
 * The base voucher used to create the activity. If the base voucher has an
 * activity identifier, then the created activity will be parented to that one.
 * If the passed in base has no activity identifier, the activity identifier
 * will be a top-level one, on behalf of the process that created the base
 * voucher.
 *
 * If base is VOUCHER_NONE, the activity is a top-level one, on behalf of the
 * current process.
 *
 * If base is VOUCHER_CURRENT, then the activity is naturally based on the
 * one currently applied to the current thread (the one voucher_copy() would
 * return).
 *
 * @param flags
 * See voucher_activity_flag_t documentation for effect.
 *
 * @param location
 * Location identifier for the automatic tracepoint generated as part of
 * creating the new activity.
 *
 * @result
 * A new voucher with an activity identifier.
 */
__OSX_AVAILABLE(10.12) __IOS_AVAILABLE(10.0)
__TVOS_AVAILABLE(10.0) __WATCHOS_AVAILABLE(3.0)
OS_VOUCHER_EXPORT OS_OBJECT_RETURNS_RETAINED OS_WARN_RESULT OS_NOTHROW
voucher_t
voucher_activity_create(firehose_tracepoint_id_t trace_id,
		voucher_t base, firehose_activity_flags_t flags, uint64_t location);

__OSX_AVAILABLE(10.12) __IOS_AVAILABLE(10.0)
__TVOS_AVAILABLE(10.0) __WATCHOS_AVAILABLE(3.0)
OS_VOUCHER_EXPORT OS_OBJECT_RETURNS_RETAINED OS_WARN_RESULT OS_NOTHROW
voucher_t
voucher_activity_create_with_location(firehose_tracepoint_id_t *trace_id,
		voucher_t base, firehose_activity_flags_t flags, uint64_t location);

/*!
 * @group Voucher Activity Trace SPI
 * SPI intended for libtrace only
 */

/*!
 * @function voucher_activity_flush
 *
 * @abstract
 * Force flushing the specified stream.
 *
 * @discussion
 * This maks all the buffers currently being written to as full, so that
 * their current content is pushed in a timely fashion.
 *
 * When this call returns, the actual flush may or may not yet have happened.
 *
 * @param stream
 * The stream to flush.
 */
__OSX_AVAILABLE(10.12) __IOS_AVAILABLE(10.0)
__TVOS_AVAILABLE(10.0) __WATCHOS_AVAILABLE(3.0)
OS_VOUCHER_EXPORT OS_NOTHROW
void
voucher_activity_flush(firehose_stream_t stream);

/*!
 * @function voucher_activity_trace
 *
 * @abstract
 * Add a tracepoint to the specified stream.
 *
 * @param stream
 * The stream to trace this entry into.
 *
 * @param trace_id
 * Tracepoint identifier returned by voucher_activity_trace_id()
 *
 * @param timestamp
 * The mach_approximate_time()/mach_absolute_time() value for this tracepoint.
 *
 * @param pubdata
 * Pointer to packed buffer of tracepoint data.
 *
 * @param publen
 * Length of data at 'pubdata'.
 */
__OSX_AVAILABLE(10.12) __IOS_AVAILABLE(10.0)
__TVOS_AVAILABLE(10.0) __WATCHOS_AVAILABLE(3.0)
OS_VOUCHER_EXPORT OS_NOTHROW OS_NONNULL4
firehose_tracepoint_id_t
voucher_activity_trace(firehose_stream_t stream,
		firehose_tracepoint_id_t trace_id, uint64_t timestamp,
		const void *pubdata, size_t publen);

/*!
 * @function voucher_activity_trace_with_private_strings
 *
 * @abstract
 * Add a tracepoint to the specified stream, with private data.
 *
 * @param stream
 * The stream to trace this entry into.
 *
 * @param trace_id
 * Tracepoint identifier returned by voucher_activity_trace_id()
 *
 * @param timestamp
 * The mach_approximate_time()/mach_absolute_time() value for this tracepoint.
 *
 * @param pubdata
 * Pointer to packed buffer of tracepoint data.
 *
 * @param publen
 * Length of data at 'pubdata'.
 *
 * @param privdata
 * Pointer to packed buffer of private tracepoint data.
 *
 * @param privlen
 * Length of data at 'privdata'.
 */
__OSX_AVAILABLE(10.12) __IOS_AVAILABLE(10.0)
__TVOS_AVAILABLE(10.0) __WATCHOS_AVAILABLE(3.0)
OS_VOUCHER_EXPORT OS_NOTHROW OS_NONNULL4 OS_NONNULL6
firehose_tracepoint_id_t
voucher_activity_trace_with_private_strings(firehose_stream_t stream,
		firehose_tracepoint_id_t trace_id, uint64_t timestamp,
		const void *pubdata, size_t publen,
		const void *privdata, size_t privlen);

typedef struct voucher_activity_hooks_s {
#define VOUCHER_ACTIVITY_HOOKS_VERSION     3
	long vah_version;
	// version 1
	mach_port_t (*vah_get_logd_port)(void);
	// version 2
	dispatch_mach_handler_function_t vah_debug_channel_handler;
	// version 3
	kern_return_t (*vah_get_reconnect_info)(mach_vm_address_t *, mach_vm_size_t *);
} *voucher_activity_hooks_t;

/*!
 * @function voucher_activity_initialize_4libtrace
 *
 * @abstract
 * Configure upcall hooks for libtrace.
 *
 * @param hooks
 * A pointer to a voucher_activity_hooks_s structure.
 */
__OSX_AVAILABLE(10.12) __IOS_AVAILABLE(10.0)
__TVOS_AVAILABLE(10.0) __WATCHOS_AVAILABLE(3.0)
OS_VOUCHER_EXPORT OS_NOTHROW OS_NONNULL_ALL
void
voucher_activity_initialize_4libtrace(voucher_activity_hooks_t hooks);

/*!
 * @function voucher_activity_get_metadata_buffer
 *
 * @abstract
 * Return address and length of buffer in the process trace memory area
 * reserved for libtrace metadata.
 *
 * @param length
 * Pointer to size_t variable, filled with length of metadata buffer.
 *
 * @result
 * Address of metadata buffer.
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_VOUCHER_EXPORT OS_WARN_RESULT OS_NOTHROW OS_NONNULL_ALL
void*
voucher_activity_get_metadata_buffer(size_t *length);

/*!
 * @function voucher_get_activity_id_4dyld
 *
 * @abstract
 * Return the current voucher activity ID. Available for the dyld client stub
 * only.
 */
__OSX_AVAILABLE(10.12) __IOS_AVAILABLE(10.0)
__TVOS_AVAILABLE(10.0) __WATCHOS_AVAILABLE(3.0)
OS_VOUCHER_EXPORT OS_WARN_RESULT OS_NOTHROW
firehose_activity_id_t
voucher_get_activity_id_4dyld(void);

__END_DECLS

#endif // OS_VOUCHER_ACTIVITY_SPI

#endif // __OS_VOUCHER_ACTIVITY_PRIVATE__
