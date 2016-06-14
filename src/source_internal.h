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

#ifndef __DISPATCH_SOURCE_INTERNAL__
#define __DISPATCH_SOURCE_INTERNAL__

#ifndef __DISPATCH_INDIRECT__
#error "Please #include <dispatch/dispatch.h> instead of this file directly."
#include <dispatch/base.h> // for HeaderDoc
#endif

#define DISPATCH_EVFILT_TIMER		(-EVFILT_SYSCOUNT - 1)
#define DISPATCH_EVFILT_CUSTOM_ADD	(-EVFILT_SYSCOUNT - 2)
#define DISPATCH_EVFILT_CUSTOM_OR	(-EVFILT_SYSCOUNT - 3)
#define DISPATCH_EVFILT_MACH_NOTIFICATION	(-EVFILT_SYSCOUNT - 4)
#define DISPATCH_EVFILT_SYSCOUNT	( EVFILT_SYSCOUNT + 4)

#if HAVE_MACH
// NOTE: dispatch_source_mach_send_flags_t and dispatch_source_mach_recv_flags_t
//       bit values must not overlap as they share the same kevent fflags !

/*!
 * @enum dispatch_source_mach_send_flags_t
 *
 * @constant DISPATCH_MACH_SEND_DELETED
 * Port-deleted notification. Disabled for source registration.
 */
enum {
	DISPATCH_MACH_SEND_DELETED = 0x4,
};
/*!
 * @enum dispatch_source_mach_recv_flags_t
 *
 * @constant DISPATCH_MACH_RECV_MESSAGE
 * Receive right has pending messages
 *
 * @constant DISPATCH_MACH_RECV_MESSAGE_DIRECT
 * Receive messages from receive right directly via kevent64()
 *
 * @constant DISPATCH_MACH_RECV_NO_SENDERS
 * Receive right has no more senders. TODO <rdar://problem/8132399>
 */
enum {
	DISPATCH_MACH_RECV_MESSAGE = 0x2,
	DISPATCH_MACH_RECV_MESSAGE_DIRECT = 0x10,
	DISPATCH_MACH_RECV_MESSAGE_DIRECT_ONCE = 0x20,
	DISPATCH_MACH_RECV_NO_SENDERS = 0x40,
};
#endif // HAVE_MACH

enum {
	/* DISPATCH_TIMER_STRICT 0x1 */
	/* DISPATCH_TIMER_BACKGROUND = 0x2, */
	DISPATCH_TIMER_WALL_CLOCK = 0x4,
	DISPATCH_TIMER_INTERVAL = 0x8,
	DISPATCH_TIMER_WITH_AGGREGATE = 0x10,
	/* DISPATCH_INTERVAL_UI_ANIMATION = 0x20 */
	DISPATCH_TIMER_AFTER = 0x40,
};

#define DISPATCH_TIMER_QOS_NORMAL 0u
#define DISPATCH_TIMER_QOS_CRITICAL 1u
#define DISPATCH_TIMER_QOS_BACKGROUND 2u
#define DISPATCH_TIMER_QOS_COUNT (DISPATCH_TIMER_QOS_BACKGROUND + 1)
#define DISPATCH_TIMER_QOS(tidx) (((uintptr_t)(tidx) >> 1) & 0x3ul)

#define DISPATCH_TIMER_KIND_WALL 0u
#define DISPATCH_TIMER_KIND_MACH 1u
#define DISPATCH_TIMER_KIND_COUNT (DISPATCH_TIMER_KIND_MACH + 1)
#define DISPATCH_TIMER_KIND(tidx) ((uintptr_t)(tidx) & 0x1ul)

#define DISPATCH_TIMER_INDEX(kind, qos) ((qos) << 1 | (kind))
#define DISPATCH_TIMER_INDEX_DISARM \
		DISPATCH_TIMER_INDEX(0, DISPATCH_TIMER_QOS_COUNT)
#define DISPATCH_TIMER_INDEX_COUNT (DISPATCH_TIMER_INDEX_DISARM + 1)
#define DISPATCH_TIMER_IDENT(flags) ({ unsigned long f = (flags); \
		DISPATCH_TIMER_INDEX(f & DISPATCH_TIMER_WALL_CLOCK ? \
		DISPATCH_TIMER_KIND_WALL : DISPATCH_TIMER_KIND_MACH, \
		f & DISPATCH_TIMER_STRICT ? DISPATCH_TIMER_QOS_CRITICAL : \
		f & DISPATCH_TIMER_BACKGROUND ? DISPATCH_TIMER_QOS_BACKGROUND : \
		DISPATCH_TIMER_QOS_NORMAL); })

struct dispatch_kevent_s {
	TAILQ_ENTRY(dispatch_kevent_s) dk_list;
	TAILQ_HEAD(, dispatch_source_refs_s) dk_sources;
	_dispatch_kevent_qos_s dk_kevent;
};

typedef struct dispatch_kevent_s *dispatch_kevent_t;

typedef typeof(((dispatch_kevent_t)NULL)->dk_kevent.udata) _dispatch_kevent_qos_udata_t;

#define DISPATCH_KEV_CUSTOM_ADD ((dispatch_kevent_t)DISPATCH_EVFILT_CUSTOM_ADD)
#define DISPATCH_KEV_CUSTOM_OR  ((dispatch_kevent_t)DISPATCH_EVFILT_CUSTOM_OR)

struct dispatch_source_type_s {
	_dispatch_kevent_qos_s ke;
	uint64_t mask;
	void (*init)(dispatch_source_t ds, dispatch_source_type_t type,
			uintptr_t handle, unsigned long mask, dispatch_queue_t q);
};

struct dispatch_timer_source_s {
	uint64_t target;
	uint64_t deadline;
	uint64_t last_fire;
	uint64_t interval;
	uint64_t leeway;
	unsigned long flags; // dispatch_timer_flags_t
	unsigned long missed;
};

enum {
	DS_EVENT_HANDLER = 0,
	DS_CANCEL_HANDLER,
	DS_REGISTN_HANDLER,
};

// Source state which may contain references to the source object
// Separately allocated so that 'leaks' can see sources <rdar://problem/9050566>
typedef struct dispatch_source_refs_s {
	TAILQ_ENTRY(dispatch_source_refs_s) dr_list;
	uintptr_t dr_source_wref; // "weak" backref to dispatch_source_t
	dispatch_continuation_t volatile ds_handler[3];
} *dispatch_source_refs_t;

typedef struct dispatch_timer_source_refs_s {
	struct dispatch_source_refs_s _ds_refs;
	struct dispatch_timer_source_s _ds_timer;
	TAILQ_ENTRY(dispatch_timer_source_refs_s) dt_list;
} *dispatch_timer_source_refs_t;

typedef struct dispatch_timer_source_aggregate_refs_s {
	struct dispatch_timer_source_refs_s _dsa_refs;
	TAILQ_ENTRY(dispatch_timer_source_aggregate_refs_s) dra_list;
	TAILQ_ENTRY(dispatch_timer_source_aggregate_refs_s) dta_list;
} *dispatch_timer_source_aggregate_refs_t;

#define _dispatch_ptr2wref(ptr) (~(uintptr_t)(ptr))
#define _dispatch_wref2ptr(ref) ((void*)~(ref))
#define _dispatch_source_from_refs(dr) \
		((dispatch_source_t)_dispatch_wref2ptr((dr)->dr_source_wref))
#define ds_timer(dr) \
		(((dispatch_timer_source_refs_t)(dr))->_ds_timer)
#define ds_timer_aggregate(ds) \
		((dispatch_timer_aggregate_t)((ds)->dq_specific_q))

DISPATCH_ALWAYS_INLINE
static inline unsigned int
_dispatch_source_timer_idx(dispatch_source_refs_t dr)
{
	return DISPATCH_TIMER_IDENT(ds_timer(dr).flags);
}

#define _DISPATCH_SOURCE_HEADER(refs) \
	DISPATCH_QUEUE_HEADER(refs); \
	/* LP64: fills 32bit hole in QUEUE_HEADER */ \
	unsigned int \
		ds_is_level:1, \
		ds_is_adder:1, \
		ds_is_installed:1, \
		ds_is_direct_kevent:1, \
		ds_is_custom_source:1, \
		ds_needs_rearm:1, \
		ds_is_timer:1, \
		ds_vmpressure_override:1, \
		ds_memorypressure_override:1, \
		dm_handler_is_block:1, \
		dm_connect_handler_called:1, \
		dm_cancel_handler_called:1; \
	dispatch_kevent_t ds_dkev; \
	dispatch_##refs##_refs_t ds_refs; \
	unsigned long ds_pending_data_mask;

#define DISPATCH_SOURCE_HEADER(refs) \
	struct dispatch_source_s _as_ds[0]; \
	_DISPATCH_SOURCE_HEADER(refs)

DISPATCH_CLASS_DECL_BARE(source);
_OS_OBJECT_CLASS_IMPLEMENTS_PROTOCOL(dispatch_source, dispatch_object);

#if DISPATCH_PURE_C
struct dispatch_source_s {
	_DISPATCH_SOURCE_HEADER(source);
	unsigned long ds_ident_hack;
	unsigned long ds_data;
	unsigned long ds_pending_data;
} DISPATCH_QUEUE_ALIGN;
#endif

#if HAVE_MACH
// Mach channel state which may contain references to the channel object
// layout must match dispatch_source_refs_s
struct dispatch_mach_refs_s {
	TAILQ_ENTRY(dispatch_mach_refs_s) dr_list;
	uintptr_t dr_source_wref; // "weak" backref to dispatch_mach_t
	dispatch_mach_handler_function_t dm_handler_func;
	void *dm_handler_ctxt;
};
typedef struct dispatch_mach_refs_s *dispatch_mach_refs_t;

struct dispatch_mach_reply_refs_s {
	TAILQ_ENTRY(dispatch_mach_reply_refs_s) dr_list;
	uintptr_t dr_source_wref; // "weak" backref to dispatch_mach_t
	dispatch_kevent_t dmr_dkev;
	void *dmr_ctxt;
	mach_port_t dmr_reply;
	dispatch_priority_t dmr_priority;
	voucher_t dmr_voucher;
	TAILQ_ENTRY(dispatch_mach_reply_refs_s) dmr_list;
};
typedef struct dispatch_mach_reply_refs_s *dispatch_mach_reply_refs_t;

#define _DISPATCH_MACH_STATE_UNUSED_MASK_2       0xff00000000000000ull
#define DISPATCH_MACH_STATE_OVERRIDE_MASK        0x00ffff0000000000ull
#define _DISPATCH_MACH_STATE_UNUSED_MASK_1       0x000000f000000000ull
#define DISPATCH_MACH_STATE_DIRTY                0x0000000800000000ull
#define DISPATCH_MACH_STATE_RECEIVED_OVERRIDE    0x0000000400000000ull
#define _DISPATCH_MACH_STATE_UNUSED_MASK_0       0x0000000200000000ull
#define DISPATCH_MACH_STATE_PENDING_BARRIER      0x0000000100000000ull
#define DISPATCH_MACH_STATE_UNLOCK_MASK          0x00000000ffffffffull

struct dispatch_mach_send_refs_s {
	TAILQ_ENTRY(dispatch_mach_send_refs_s) dr_list;
	uintptr_t dr_source_wref; // "weak" backref to dispatch_mach_t
	dispatch_mach_msg_t dm_checkin;
	TAILQ_HEAD(, dispatch_mach_reply_refs_s) dm_replies;
	dispatch_unfair_lock_s dm_replies_lock;
#define DISPATCH_MACH_DISCONNECT_MAGIC_BASE (0x80000000)
#define DISPATCH_MACH_NEVER_INSTALLED (DISPATCH_MACH_DISCONNECT_MAGIC_BASE + 0)
#define DISPATCH_MACH_NEVER_CONNECTED (DISPATCH_MACH_DISCONNECT_MAGIC_BASE + 1)
	uint32_t volatile dm_disconnect_cnt;
	union {
		uint64_t volatile dm_state;
		DISPATCH_STRUCT_LITTLE_ENDIAN_2(
			dispatch_unfair_lock_s dm_state_lock,
			uint32_t dm_state_bits
		);
	};
	unsigned int dm_needs_mgr:1;
	struct dispatch_object_s *volatile dm_tail;
	struct dispatch_object_s *volatile dm_head;
	mach_port_t dm_send, dm_checkin_port;
};
typedef struct dispatch_mach_send_refs_s *dispatch_mach_send_refs_t;

DISPATCH_CLASS_DECL(mach);
#if DISPATCH_PURE_C
struct dispatch_mach_s {
	DISPATCH_SOURCE_HEADER(mach);
	dispatch_kevent_t dm_dkev;
	dispatch_mach_send_refs_t dm_refs;
} DISPATCH_QUEUE_ALIGN;
#endif

DISPATCH_CLASS_DECL(mach_msg);
struct dispatch_mach_msg_s {
	DISPATCH_OBJECT_HEADER(mach_msg);
	union {
		mach_msg_option_t dmsg_options;
		mach_error_t dmsg_error;
	};
	mach_port_t dmsg_reply;
	pthread_priority_t dmsg_priority;
	voucher_t dmsg_voucher;
	dispatch_mach_msg_destructor_t dmsg_destructor;
	size_t dmsg_size;
	union {
		mach_msg_header_t *dmsg_msg;
		char dmsg_buf[0];
	};
};
#endif // HAVE_MACH

extern const struct dispatch_source_type_s _dispatch_source_type_after;

#if TARGET_OS_EMBEDDED
#define DSL_HASH_SIZE  64u // must be a power of two
#else
#define DSL_HASH_SIZE 256u // must be a power of two
#endif

dispatch_source_t
_dispatch_source_create_mach_msg_direct_recv(mach_port_t recvp,
		const struct dispatch_continuation_s *dc);
void _dispatch_source_xref_dispose(dispatch_source_t ds);
void _dispatch_source_dispose(dispatch_source_t ds);
void _dispatch_source_finalize_activation(dispatch_source_t ds);
void _dispatch_source_invoke(dispatch_source_t ds, dispatch_invoke_flags_t flags);
void _dispatch_source_wakeup(dispatch_source_t ds, pthread_priority_t pp,
		dispatch_wakeup_flags_t flags);
size_t _dispatch_source_debug(dispatch_source_t ds, char* buf, size_t bufsiz);
void _dispatch_source_set_interval(dispatch_source_t ds, uint64_t interval);
void _dispatch_source_set_event_handler_continuation(dispatch_source_t ds,
		dispatch_continuation_t dc);
DISPATCH_EXPORT // for firehose server
void _dispatch_source_merge_data(dispatch_source_t ds, pthread_priority_t pp,
		unsigned long val);

#if HAVE_MACH
void _dispatch_mach_dispose(dispatch_mach_t dm);
void _dispatch_mach_finalize_activation(dispatch_mach_t dm);
void _dispatch_mach_invoke(dispatch_mach_t dm, dispatch_invoke_flags_t flags);
void _dispatch_mach_wakeup(dispatch_mach_t dm, pthread_priority_t pp,
		dispatch_wakeup_flags_t flags);
size_t _dispatch_mach_debug(dispatch_mach_t dm, char* buf, size_t bufsiz);

void _dispatch_mach_msg_dispose(dispatch_mach_msg_t dmsg);
void _dispatch_mach_msg_invoke(dispatch_mach_msg_t dmsg,
		dispatch_invoke_flags_t flags);
size_t _dispatch_mach_msg_debug(dispatch_mach_msg_t dmsg, char* buf,
		size_t bufsiz);

void _dispatch_mach_send_barrier_drain_invoke(dispatch_continuation_t dc,
		dispatch_invoke_flags_t flags);
void _dispatch_mach_barrier_invoke(dispatch_continuation_t dc,
		dispatch_invoke_flags_t flags);
#endif // HAVE_MACH

void _dispatch_mgr_queue_wakeup(dispatch_queue_t dq, pthread_priority_t pp,
		dispatch_wakeup_flags_t flags);
void _dispatch_mgr_thread(dispatch_queue_t dq, dispatch_invoke_flags_t flags);
#if DISPATCH_USE_KEVENT_WORKQUEUE
void _dispatch_kevent_worker_thread(_dispatch_kevent_qos_s **events,
		int *nevents);
#endif

#endif /* __DISPATCH_SOURCE_INTERNAL__ */
