/*
 * Copyright (c) 2013-2014 Apple Inc. All rights reserved.
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

#include <os/base.h>
#include <os/object.h>
#if !defined(__DISPATCH_BUILDING_DISPATCH__)
#include <os/voucher_private.h>
#endif

#define OS_VOUCHER_ACTIVITY_SPI_VERSION 20150318

#if OS_VOUCHER_WEAK_IMPORT
#define OS_VOUCHER_EXPORT OS_EXPORT OS_WEAK_IMPORT
#else
#define OS_VOUCHER_EXPORT OS_EXPORT
#endif

__BEGIN_DECLS

#if OS_VOUCHER_ACTIVITY_SPI

/*!
 * @group Voucher Activity SPI
 * SPI intended for libtrace only
 */

/*!
 * @typedef voucher_activity_id_t
 *
 * @abstract
 * Opaque activity identifier.
 *
 * @discussion
 * Scalar value type, not reference counted.
 */
typedef uint64_t voucher_activity_id_t;

/*!
 * @enum voucher_activity_tracepoint_type_t
 *
 * @abstract
 * Types of tracepoints.
 */
OS_ENUM(voucher_activity_tracepoint_type, uint8_t,
	voucher_activity_tracepoint_type_release = (1u << 0),
	voucher_activity_tracepoint_type_debug = (1u << 1),
	voucher_activity_tracepoint_type_error = (1u << 6) | (1u << 0),
	voucher_activity_tracepoint_type_fault = (1u << 7) | (1u << 6) | (1u << 0),
);

/*!
 * @enum voucher_activity_flag_t
 *
 * @abstract
 * Flags to pass to voucher_activity_start/voucher_activity_start_with_location
 */
OS_ENUM(voucher_activity_flag, unsigned long,
	voucher_activity_flag_default = 0,
	voucher_activity_flag_force = 0x1,
	voucher_activity_flag_debug = 0x2,
	voucher_activity_flag_persist = 0x4,
	voucher_activity_flag_stream = 0x8,
);

/*!
 * @typedef voucher_activity_trace_id_t
 *
 * @abstract
 * Opaque tracepoint identifier.
 */
typedef uint64_t voucher_activity_trace_id_t;
static const uint8_t _voucher_activity_trace_id_type_shift = 40;
static const uint8_t _voucher_activity_trace_id_code_namespace_shift = 32;

/*!
 * @function voucher_activity_trace_id
 *
 * @abstract
 * Return tracepoint identifier for specified arguments.
 *
 * @param type
 * Tracepoint type from voucher_activity_tracepoint_type_t.
 *
 * @param code_namespace
 * Namespace of 'code' argument.
 *
 * @param code
 * Tracepoint code.
 *
 * @result
 * Tracepoint identifier.
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_INLINE OS_ALWAYS_INLINE
voucher_activity_trace_id_t
voucher_activity_trace_id(uint8_t type, uint8_t code_namespace, uint32_t code)
{
	return ((voucher_activity_trace_id_t)type <<
			_voucher_activity_trace_id_type_shift) |
			((voucher_activity_trace_id_t)code_namespace <<
			_voucher_activity_trace_id_code_namespace_shift) |
			(voucher_activity_trace_id_t)code;
}

/*!
 * @function voucher_activity_start
 *
 * @abstract
 * Creates a new activity identifier and marks the current thread as
 * participating in the activity.
 *
 * @discussion
 * As part of voucher transport, activities are automatically propagated by the
 * system to other threads and processes (across IPC).
 *
 * Activities persist as long as any threads in any process are marked as
 * participating. There may be many calls to voucher_activity_end()
 * corresponding to one call to voucher_activity_start().
 *
 * @param trace_id
 * Tracepoint identifier returned by voucher_activity_trace_id(), intended for
 * identification of the automatic tracepoint generated as part of creating the
 * new activity.
 *
 * @param flags
 * Pass voucher_activity_flag_force to indicate that existing activities
 * on the current thread should not be inherited and that a new toplevel
 * activity should be created.
 *
 * @result
 * A new activity identifier.
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_VOUCHER_EXPORT OS_WARN_RESULT OS_NOTHROW
voucher_activity_id_t
voucher_activity_start(voucher_activity_trace_id_t trace_id,
		voucher_activity_flag_t flags);

/*!
 * @function voucher_activity_start_with_location
 *
 * @abstract
 * Creates a new activity identifier and marks the current thread as
 * participating in the activity.
 *
 * @discussion
 * As part of voucher transport, activities are automatically propagated by the
 * system to other threads and processes (across IPC).
 *
 * Activities persist as long as any threads in any process are marked as
 * participating. There may be many calls to voucher_activity_end()
 * corresponding to one call to voucher_activity_start_with_location().
 *
 * @param trace_id
 * Tracepoint identifier returned by voucher_activity_trace_id(), intended for
 * identification of the automatic tracepoint generated as part of creating the
 * new activity.
 *
 * @param location
 * Location identifier for the automatic tracepoint generated as part of
 * creating the new activity.
 *
 * @param flags
 * Pass voucher_activity_flag_force to indicate that existing activities
 * on the current thread should not be inherited and that a new toplevel
 * activity should be created.
 *
 * @result
 * A new activity identifier.
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_VOUCHER_EXPORT OS_WARN_RESULT OS_NOTHROW
voucher_activity_id_t
voucher_activity_start_with_location(voucher_activity_trace_id_t trace_id,
		uint64_t location, voucher_activity_flag_t flags);

/*!
 * @function voucher_activity_end
 *
 * @abstract
 * Unmarks the current thread if it is marked as particpating in the activity
 * with the specified identifier.
 *
 * @discussion
 * Activities persist as long as any threads in any process are marked as
 * participating. There may be many calls to voucher_activity_end()
 * corresponding to one call to voucher_activity_start() or
 * voucher_activity_start_with_location().
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_VOUCHER_EXPORT OS_NOTHROW
void
voucher_activity_end(voucher_activity_id_t activity_id);

/*!
 * @function voucher_get_activities
 *
 * @abstract
 * Returns the list of activity identifiers that the current thread is marked
 * with.
 *
 * @param entries
 * Pointer to an array of activity identifiers to be filled in.
 *
 * @param count
 * Pointer to the requested number of activity identifiers.
 * On output will be filled with the number of activities that are available.
 *
 * @result
 * Number of activity identifiers written to 'entries'
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_VOUCHER_EXPORT OS_NOTHROW
unsigned int
voucher_get_activities(voucher_activity_id_t *entries, unsigned int *count);

/*!
 * @group Voucher Activity Trace SPI
 * SPI intended for libtrace only
 */

/*!
 * @function voucher_activity_get_namespace
 *
 * @abstract
 * Returns the namespace of the current activity.
 *
 * @result
 * The namespace of the current activity (if any).
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_VOUCHER_EXPORT OS_NOTHROW
uint8_t
voucher_activity_get_namespace(void);

/*!
 * @function voucher_activity_trace
 *
 * @abstract
 * Add a tracepoint to trace buffer of the current activity.
 *
 * @param trace_id
 * Tracepoint identifier returned by voucher_activity_trace_id()
 *
 * @param location
 * Tracepoint location.
 *
 * @param buffer
 * Pointer to packed buffer of tracepoint data.
 *
 * @param length
 * Length of data at 'buffer'.
 *
 * @result
 * Timestamp recorded in tracepoint or 0 if no tracepoint was recorded.
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_VOUCHER_EXPORT OS_NOTHROW
uint64_t
voucher_activity_trace(voucher_activity_trace_id_t trace_id, uint64_t location,
		void *buffer, size_t length);

/*!
 * @function voucher_activity_trace_strings
 *
 * @abstract
 * Add a tracepoint with strings data to trace buffer of the current activity.
 *
 * @param trace_id
 * Tracepoint identifier returned by voucher_activity_trace_id()
 *
 * @param location
 * Tracepoint location.
 *
 * @param buffer
 * Pointer to packed buffer of tracepoint data.
 *
 * @param length
 * Length of data at 'buffer'.
 *
 * @param strings
 * NULL-terminated array of strings data.
 *
 * @param string_lengths
 * Array of string lengths (required to have the same number of elements as the
 * 'strings' array): string_lengths[i] is the maximum number of characters to
 * copy from strings[i], excluding the NUL-terminator (may be smaller than the
 * length of the string present in strings[i]).
 *
 * @param total_strings_size
 * Total size of all strings data to be copied from strings array (including
 * all NUL-terminators).
 *
 * @result
 * Timestamp recorded in tracepoint or 0 if no tracepoint was recorded.
 */
__OSX_AVAILABLE_STARTING(__MAC_10_11,__IPHONE_9_0)
OS_VOUCHER_EXPORT OS_NOTHROW
uint64_t
voucher_activity_trace_strings(voucher_activity_trace_id_t trace_id,
		uint64_t location, void *buffer, size_t length, const char *strings[],
		size_t string_lengths[], size_t total_strings_size);

/*!
 * @function voucher_activity_trace_args
 *
 * @abstract
 * Add a tracepoint to trace buffer of the current activity, recording
 * specified arguments passed in registers.
 *
 * @param trace_id
 * Tracepoint identifier returned by voucher_activity_trace_id()
 *
 * @param location
 * Tracepoint location.
 *
 * @param arg1
 * Argument to be recorded in tracepoint data.
 *
 * @param arg2
 * Argument to be recorded in tracepoint data.
 *
 * @param arg3
 * Argument to be recorded in tracepoint data.
 *
 * @param arg4
 * Argument to be recorded in tracepoint data.
 *
 * @result
 * Timestamp recorded in tracepoint or 0 if no tracepoint was recorded.
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_VOUCHER_EXPORT OS_NOTHROW
uint64_t
voucher_activity_trace_args(voucher_activity_trace_id_t trace_id,
		uint64_t location, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
		uintptr_t arg4);

/*!
 * @group Voucher Activity Mode SPI
 * SPI intended for libtrace only
 */

/*!
 * @enum voucher_activity_mode_t
 *
 * @abstract
 * Voucher activity mode.
 *
 * @discussion
 * Configure at process start by setting the OS_ACTIVITY_MODE environment
 * variable.
 */
OS_ENUM(voucher_activity_mode, unsigned long,
	voucher_activity_mode_disable = 0,
	voucher_activity_mode_release = (1u << 0),
	voucher_activity_mode_debug = (1u << 1),
	voucher_activity_mode_stream = (1u << 2),
);

/*!
 * @function voucher_activity_get_mode
 *
 * @abstract
 * Return current mode of voucher activity subsystem.
 *
 * @result
 * Value from voucher_activity_mode_t enum.
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_VOUCHER_EXPORT OS_WARN_RESULT OS_NOTHROW
voucher_activity_mode_t
voucher_activity_get_mode(void);

/*!
 * @function voucher_activity_set_mode_4libtrace
 *
 * @abstract
 * Set the current mode of voucher activity subsystem.
 *
 * @param mode
 * The new mode.
 *
 * Note that the new mode will take effect soon, but not immediately.
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_VOUCHER_EXPORT OS_NOTHROW
void
voucher_activity_set_mode_4libtrace(voucher_activity_mode_t mode);

/*!
 * @group Voucher Activity Metadata SPI
 * SPI intended for libtrace only
 */

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

#endif // OS_VOUCHER_ACTIVITY_SPI

#if OS_VOUCHER_ACTIVITY_BUFFER_SPI

/*!
 * @group Voucher Activity Tracepoint SPI
 * SPI intended for diagnosticd only
 */

OS_ENUM(_voucher_activity_tracepoint_flag, uint16_t,
	_voucher_activity_trace_flag_buffer_empty = 0,
	_voucher_activity_trace_flag_tracepoint = (1u << 0),
	_voucher_activity_trace_flag_tracepoint_args = (1u << 1),
	_voucher_activity_trace_flag_tracepoint_strings = (1u << 2),
	_voucher_activity_trace_flag_wide_first = (1u << 6),
	_voucher_activity_trace_flag_wide_second = (1u << 6) | (1u << 7),
	_voucher_activity_trace_flag_start = (1u << 8),
	_voucher_activity_trace_flag_end = (1u << 8) | (1u << 9),
	_voucher_activity_trace_flag_libdispatch = (1u << 13),
	_voucher_activity_trace_flag_activity = (1u << 14),
	_voucher_activity_trace_flag_buffer_header = (1u << 15),
);

// for tracepoints with _voucher_activity_trace_flag_libdispatch
OS_ENUM(_voucher_activity_tracepoint_namespace, uint8_t,
	_voucher_activity_tracepoint_namespace_ipc = 0x1
);
OS_ENUM(_voucher_activity_tracepoint_code, uint32_t,
	_voucher_activity_tracepoint_namespace_ipc_send = 0x1,
	_voucher_activity_tracepoint_namespace_ipc_receive = 0x2,
);

typedef struct _voucher_activity_tracepoint_s {
	uint16_t vat_flags;		// voucher_activity_tracepoint_flag_t
	uint8_t  vat_type;		// voucher_activity_tracepoint_type_t
	uint8_t  vat_namespace;	// namespace for tracepoint code
	uint32_t vat_code;		// tracepoint code
	uint64_t vat_thread;	// pthread_t
	uint64_t vat_timestamp;	// absolute time
	uint64_t vat_location;	// tracepoint PC
	union {
		uint64_t vat_data[4];	// trace data
		struct {
			uint16_t vats_offset;	// offset to string data (from buffer end)
			uint8_t vats_data[30];	// trace data
		} vat_stroff;				// iff _vat_flag_tracepoint_strings present
	};
} *_voucher_activity_tracepoint_t;

/*!
 * @group Voucher Activity Buffer Internals
 * SPI intended for diagnosticd only
 * Layout of structs is subject to change without notice
 */

#include <sys/queue.h>
#include <atm/atm_types.h>
#include <os/lock_private.h>

static const size_t _voucher_activity_buffer_size = 4096;
static const size_t _voucher_activity_tracepoints_per_buffer =
		_voucher_activity_buffer_size /
		sizeof(struct _voucher_activity_tracepoint_s);
static const size_t _voucher_activity_buffer_header_size =
		sizeof(struct _voucher_activity_tracepoint_s);
static const size_t _voucher_activity_strings_header_size = 0; // TODO

typedef uint8_t _voucher_activity_buffer_t[_voucher_activity_buffer_size];

static const size_t _voucher_activity_buffers_per_heap = 512;
typedef unsigned long _voucher_activity_bitmap_base_t;
static const size_t _voucher_activity_bits_per_bitmap_base_t =
		8 * sizeof(_voucher_activity_bitmap_base_t);
static const size_t _voucher_activity_bitmaps_per_heap =
		_voucher_activity_buffers_per_heap /
		_voucher_activity_bits_per_bitmap_base_t;
typedef _voucher_activity_bitmap_base_t
		_voucher_activity_bitmap_t[_voucher_activity_bitmaps_per_heap]
		__attribute__((__aligned__(64)));

struct _voucher_activity_self_metadata_s {
	struct _voucher_activity_metadata_opaque_s *vasm_baseaddr;
	_voucher_activity_bitmap_t volatile vam_buffer_bitmap;
};

typedef struct _voucher_activity_metadata_opaque_s {
	_voucher_activity_buffer_t vam_client_metadata;
	union {
		struct _voucher_activity_self_metadata_s vam_self_metadata;
		_voucher_activity_buffer_t vam_self_metadata_opaque;
	};
} *_voucher_activity_metadata_opaque_t;

typedef os_lock_handoff_s _voucher_activity_lock_s;

OS_ENUM(_voucher_activity_buffer_atomic_flags, uint8_t,
	_voucher_activity_buffer_full = (1u << 0),
	_voucher_activity_buffer_pushing = (1u << 1),
);

typedef union {
	uint64_t vabp_atomic_pos;
	struct {
		uint16_t vabp_refcnt;
		uint8_t vabp_flags;
		uint8_t vabp_unused;
		uint16_t vabp_next_tracepoint_idx;
		uint16_t vabp_string_offset; // offset from the _end_ of the buffer
	} vabp_pos;
} _voucher_activity_buffer_position_u;

// must match layout of _voucher_activity_tracepoint_s
typedef struct _voucher_activity_buffer_header_s {
	uint16_t vabh_flags;	// _voucher_activity_trace_flag_buffer_header
	uint8_t  vat_type;
	uint8_t  vat_namespace;
	uint32_t vat_code;
	uint64_t vat_thread;
	uint64_t vat_timestamp;
	uint64_t vat_location;
	voucher_activity_id_t vabh_activity_id;
	_voucher_activity_buffer_position_u volatile vabh_pos;
	TAILQ_ENTRY(_voucher_activity_buffer_header_s) vabh_list;
} *_voucher_activity_buffer_header_t;

/*!
 * @enum _voucher_activity_buffer_hook_reason
 *
 * @constant _voucher_activity_buffer_hook_reason_full
 * Specified activity buffer is full.
 * Will be reported reused or freed later.
 *
 * @constant _voucher_activity_buffer_hook_reason_reuse
 * Specified activity buffer is about to be reused.
 * Was previously reported as full.
 *
 * @constant _voucher_activity_buffer_hook_reason_free
 * Specified activity buffer is about to be freed.
 * May have been previously reported as full or may be only partially filled.
 */
typedef enum _voucher_activity_buffer_hook_reason {
	_voucher_activity_buffer_hook_reason_full = 0x1,
	_voucher_activity_buffer_hook_reason_reuse = 0x2,
	_voucher_activity_buffer_hook_reason_free = 0x4,
} _voucher_activity_buffer_hook_reason;

/*!
 * @typedef _voucher_activity_buffer_hook_t
 *
 * @abstract
 * A function pointer called when an activity buffer is full or being freed.
 * NOTE: callbacks occur under an activity-wide handoff lock and work done
 * inside the callback function must not block or otherwise cause that lock to
 * be held for a extended period of time.
 *
 * @param reason
 * Reason for callback.
 *
 * @param buffer
 * Pointer to activity buffer.
 */
typedef void (*_voucher_activity_buffer_hook_t)(
		_voucher_activity_buffer_hook_reason reason,
		_voucher_activity_buffer_header_t buffer);

/*!
 * @function voucher_activity_buffer_hook_install_4libtrace
 *
 * @abstract
 * Install activity buffer hook callback function.
 * Must be called from the libtrace initializer, and at most once.
 *
 * @param hook
 * Hook function to install.
 */
__OSX_AVAILABLE_STARTING(__MAC_10_11,__IPHONE_9_0)
OS_VOUCHER_EXPORT OS_NOTHROW
void
voucher_activity_buffer_hook_install_4libtrace(
		_voucher_activity_buffer_hook_t hook);

#endif // OS_VOUCHER_ACTIVITY_BUFFER_SPI

__END_DECLS

#endif // __OS_VOUCHER_ACTIVITY_PRIVATE__
