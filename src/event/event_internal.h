/*
 * Copyright (c) 2008-2016 Apple Inc. All rights reserved.
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

#ifndef __DISPATCH_EVENT_EVENT_INTERNAL__
#define __DISPATCH_EVENT_EVENT_INTERNAL__

#include "event_config.h"

struct dispatch_sync_context_s;
typedef struct dispatch_wlh_s *dispatch_wlh_t; // opaque handle
#define DISPATCH_WLH_ANON       ((dispatch_wlh_t)(void*)(~0ul))
#define DISPATCH_WLH_MANAGER    ((dispatch_wlh_t)(void*)(~2ul))

#define DISPATCH_UNOTE_DATA_ACTION_SIZE 2

#define DISPATCH_UNOTE_CLASS_HEADER() \
	dispatch_source_type_t du_type; \
	uintptr_t du_owner_wref; /* "weak" back reference to the owner object */ \
	dispatch_wlh_t du_wlh; \
	uint32_t  du_ident; \
	int8_t    du_filter; \
	os_atomic(bool) dmsr_notification_armed; \
	uint16_t  du_data_action : DISPATCH_UNOTE_DATA_ACTION_SIZE; \
	uint16_t  du_is_direct : 1; \
	uint16_t  du_is_timer : 1; \
	uint16_t  du_memorypressure_override : 1; \
	uint16_t  du_vmpressure_override : 1; \
	uint16_t  du_can_be_wlh : 1; \
	uint16_t  dmr_async_reply : 1; \
	uint16_t  dmrr_handler_is_block : 1; \
	uint16_t  du_unused : 7; \
	uint32_t  du_fflags; \
	dispatch_priority_t du_priority

#define _dispatch_ptr2wref(ptr) (~(uintptr_t)(ptr))
#define _dispatch_wref2ptr(ref) ((void*)~(ref))
#define _dispatch_source_from_refs(dr) \
		((dispatch_source_t)_dispatch_wref2ptr((dr)->du_owner_wref))

DISPATCH_ENUM(dispatch_unote_action, uint8_t,
    DISPATCH_UNOTE_ACTION_DATA_OR = 0,
    DISPATCH_UNOTE_ACTION_DATA_OR_STATUS_SET,
    DISPATCH_UNOTE_ACTION_DATA_SET,
    DISPATCH_UNOTE_ACTION_DATA_ADD,
	DISPATCH_UNOTE_ACTION_LAST = DISPATCH_UNOTE_ACTION_DATA_ADD
);
_Static_assert(DISPATCH_UNOTE_ACTION_LAST <
		(1 << DISPATCH_UNOTE_DATA_ACTION_SIZE),
		"DISPATCH_UNOTE_ACTION_LAST too large for du_data_action field");

typedef struct dispatch_unote_class_s {
	DISPATCH_UNOTE_CLASS_HEADER();
} *dispatch_unote_class_t;


enum {
	DS_EVENT_HANDLER = 0,
	DS_CANCEL_HANDLER,
	DS_REGISTN_HANDLER,
};

#define DISPATCH_SOURCE_REFS_HEADER() \
	DISPATCH_UNOTE_CLASS_HEADER(); \
	struct dispatch_continuation_s *volatile ds_handler[3]

// Source state which may contain references to the source object
// Separately allocated so that 'leaks' can see sources <rdar://problem/9050566>
typedef struct dispatch_source_refs_s {
	DISPATCH_SOURCE_REFS_HEADER();
} *dispatch_source_refs_t;

typedef struct dispatch_timer_delay_s {
	uint64_t delay, leeway;
} dispatch_timer_delay_s;

#define DTH_INVALID_ID  (~0u)
#define DTH_TARGET_ID   0u
#define DTH_DEADLINE_ID 1u
#define DTH_ID_COUNT    2u

typedef struct dispatch_timer_source_s {
	union {
		struct {
			uint64_t target;
			uint64_t deadline;
		};
		uint64_t heap_key[DTH_ID_COUNT];
	};
	uint64_t interval;
} *dispatch_timer_source_t;

typedef struct dispatch_timer_config_s {
	struct dispatch_timer_source_s dtc_timer;
	dispatch_clock_t dtc_clock;
} *dispatch_timer_config_t;

typedef struct dispatch_timer_source_refs_s {
	DISPATCH_SOURCE_REFS_HEADER();
	struct dispatch_timer_source_s dt_timer;
	struct dispatch_timer_config_s *dt_pending_config;
	uint32_t dt_heap_entry[DTH_ID_COUNT];
} *dispatch_timer_source_refs_t;

typedef struct dispatch_timer_heap_s {
	uint64_t dth_target, dth_deadline;
	uint32_t dth_count;
	uint16_t dth_segments;
#define DTH_ARMED  1u
	uint16_t dth_flags;
	dispatch_timer_source_refs_t dth_min[DTH_ID_COUNT];
	void **dth_heap;
} *dispatch_timer_heap_t;

#if HAVE_MACH
#if DISPATCH_MACHPORT_DEBUG
void dispatch_debug_machport(mach_port_t name, const char *str);
#define _dispatch_debug_machport(name) \
		dispatch_debug_machport((name), __func__)
#else
#define _dispatch_debug_machport(name) ((void)(name))
#endif // DISPATCH_MACHPORT_DEBUG

// Mach channel state which may contain references to the channel object
// layout must match dispatch_source_refs_s
struct dispatch_mach_recv_refs_s {
	DISPATCH_UNOTE_CLASS_HEADER();
	dispatch_mach_handler_function_t dmrr_handler_func;
	void *dmrr_handler_ctxt;
};
typedef struct dispatch_mach_recv_refs_s *dispatch_mach_recv_refs_t;

struct dispatch_mach_reply_refs_s {
	DISPATCH_UNOTE_CLASS_HEADER();
	dispatch_priority_t dmr_priority;
	void *dmr_ctxt;
	voucher_t dmr_voucher;
	TAILQ_ENTRY(dispatch_mach_reply_refs_s) dmr_list;
	mach_port_t dmr_waiter_tid;
};
typedef struct dispatch_mach_reply_refs_s *dispatch_mach_reply_refs_t;

#define _DISPATCH_MACH_STATE_UNUSED_MASK        0xffffffa000000000ull
#define DISPATCH_MACH_STATE_DIRTY               0x0000002000000000ull
#define DISPATCH_MACH_STATE_PENDING_BARRIER     0x0000001000000000ull
#define DISPATCH_MACH_STATE_RECEIVED_OVERRIDE   0x0000000800000000ull
#define DISPATCH_MACH_STATE_MAX_QOS_MASK        0x0000000700000000ull
#define DISPATCH_MACH_STATE_MAX_QOS_SHIFT       32
#define DISPATCH_MACH_STATE_UNLOCK_MASK         0x00000000ffffffffull

struct dispatch_mach_send_refs_s {
	DISPATCH_UNOTE_CLASS_HEADER();
	dispatch_mach_msg_t dmsr_checkin;
	TAILQ_HEAD(, dispatch_mach_reply_refs_s) dmsr_replies;
	dispatch_unfair_lock_s dmsr_replies_lock;
#define DISPATCH_MACH_DISCONNECT_MAGIC_BASE (0x80000000)
#define DISPATCH_MACH_NEVER_INSTALLED (DISPATCH_MACH_DISCONNECT_MAGIC_BASE + 0)
#define DISPATCH_MACH_NEVER_CONNECTED (DISPATCH_MACH_DISCONNECT_MAGIC_BASE + 1)
	uint32_t volatile dmsr_disconnect_cnt;
	DISPATCH_UNION_LE(uint64_t volatile dmsr_state,
		dispatch_unfair_lock_s dmsr_state_lock,
		uint32_t dmsr_state_bits
	) DISPATCH_ATOMIC64_ALIGN;
	struct dispatch_object_s *volatile dmsr_tail;
	struct dispatch_object_s *volatile dmsr_head;
	mach_port_t dmsr_send, dmsr_checkin_port;
};
typedef struct dispatch_mach_send_refs_s *dispatch_mach_send_refs_t;

void _dispatch_mach_notification_set_armed(dispatch_mach_send_refs_t dmsr);

struct dispatch_xpc_term_refs_s {
	DISPATCH_UNOTE_CLASS_HEADER();
};
typedef struct dispatch_xpc_term_refs_s *dispatch_xpc_term_refs_t;
#endif // HAVE_MACH

typedef union dispatch_unote_u {
	dispatch_unote_class_t _du;
	dispatch_source_refs_t _dr;
	dispatch_timer_source_refs_t _dt;
#if HAVE_MACH
	dispatch_mach_recv_refs_t _dmrr;
	dispatch_mach_send_refs_t _dmsr;
	dispatch_mach_reply_refs_t _dmr;
	dispatch_xpc_term_refs_t _dxtr;
#endif
} dispatch_unote_t DISPATCH_TRANSPARENT_UNION;

#define DISPATCH_UNOTE_NULL ((dispatch_unote_t){ ._du = NULL })

#if TARGET_OS_EMBEDDED
#define DSL_HASH_SIZE  64u // must be a power of two
#else
#define DSL_HASH_SIZE 256u // must be a power of two
#endif
#define DSL_HASH(x) ((x) & (DSL_HASH_SIZE - 1))

typedef struct dispatch_unote_linkage_s {
	TAILQ_ENTRY(dispatch_unote_linkage_s) du_link;
	struct dispatch_muxnote_s *du_muxnote;
} DISPATCH_ATOMIC64_ALIGN *dispatch_unote_linkage_t;

#define DU_UNREGISTER_IMMEDIATE_DELETE 0x01
#define DU_UNREGISTER_ALREADY_DELETED  0x02
#define DU_UNREGISTER_DISCONNECTED     0x04
#define DU_UNREGISTER_REPLY_REMOVE     0x08

typedef struct dispatch_source_type_s {
	const char *dst_kind;
	int8_t     dst_filter;
	uint8_t    dst_per_trigger_qos : 1;
	uint16_t   dst_flags;
	uint32_t   dst_fflags;
	uint32_t   dst_mask;
	uint32_t   dst_size;
#if DISPATCH_EVENT_BACKEND_KEVENT
	uint32_t   dst_data;
#endif

	dispatch_unote_t (*dst_create)(dispatch_source_type_t dst,
			uintptr_t handle, unsigned long mask);
#if DISPATCH_EVENT_BACKEND_KEVENT
	bool (*dst_update_mux)(struct dispatch_muxnote_s *dmn);
#endif
	void (*dst_merge_evt)(dispatch_unote_t du, uint32_t flags, uintptr_t data,
			uintptr_t status, pthread_priority_t pp);
#if HAVE_MACH
	void (*dst_merge_msg)(dispatch_unote_t du, uint32_t flags,
			mach_msg_header_t *msg, mach_msg_size_t sz);
#endif
} dispatch_source_type_s;

#define dux_create(dst, handle, mask)	(dst)->dst_create(dst, handle, mask)
#define dux_merge_evt(du, ...)	(du)->du_type->dst_merge_evt(du, __VA_ARGS__)
#define dux_merge_msg(du, ...)	(du)->du_type->dst_merge_msg(du, __VA_ARGS__)

extern const dispatch_source_type_s _dispatch_source_type_after;

#if HAVE_MACH
extern const dispatch_source_type_s _dispatch_source_type_mach_recv_direct;
extern const dispatch_source_type_s _dispatch_mach_type_send;
extern const dispatch_source_type_s _dispatch_mach_type_recv;
extern const dispatch_source_type_s _dispatch_mach_type_reply;
extern const dispatch_source_type_s _dispatch_xpc_type_sigterm;
#endif

#pragma mark -
#pragma mark deferred items

#if DISPATCH_EVENT_BACKEND_KEVENT
#if DISPATCH_USE_KEVENT_QOS
typedef struct kevent_qos_s dispatch_kevent_s;
#else
typedef struct kevent dispatch_kevent_s;
#endif
typedef dispatch_kevent_s *dispatch_kevent_t;
#endif // DISPATCH_EVENT_BACKEND_KEVENT

#define DISPATCH_DEFERRED_ITEMS_EVENT_COUNT 16

typedef struct dispatch_deferred_items_s {
	dispatch_queue_t ddi_stashed_rq;
	dispatch_object_t ddi_stashed_dou;
	dispatch_qos_t ddi_stashed_qos;
#if DISPATCH_EVENT_BACKEND_KEVENT
	dispatch_kevent_t ddi_eventlist;
	uint16_t ddi_nevents;
	uint16_t ddi_maxevents;
	bool     ddi_can_stash;
	uint16_t ddi_wlh_needs_delete : 1;
	uint16_t ddi_wlh_needs_update : 1;
	uint16_t ddi_wlh_servicing : 1;
#endif
} dispatch_deferred_items_s, *dispatch_deferred_items_t;

#pragma mark -
#pragma mark inlines

#if DISPATCH_PURE_C

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_deferred_items_set(dispatch_deferred_items_t ddi)
{
	_dispatch_thread_setspecific(dispatch_deferred_items_key, (void *)ddi);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_deferred_items_t
_dispatch_deferred_items_get(void)
{
	return (dispatch_deferred_items_t)
			_dispatch_thread_getspecific(dispatch_deferred_items_key);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_needs_to_return_to_kernel(void)
{
	return (uintptr_t)_dispatch_thread_getspecific(dispatch_r2k_key) != 0;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_set_return_to_kernel(void)
{
	_dispatch_thread_setspecific(dispatch_r2k_key, (void *)1);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_clear_return_to_kernel(void)
{
	_dispatch_thread_setspecific(dispatch_r2k_key, (void *)0);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_unote_registered(dispatch_unote_t du)
{
	return du._du->du_wlh != NULL;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_unote_wlh_changed(dispatch_unote_t du, dispatch_wlh_t expected_wlh)
{
	dispatch_wlh_t wlh = du._du->du_wlh;
	return wlh && wlh != DISPATCH_WLH_ANON && wlh != expected_wlh;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_unote_linkage_t
_dispatch_unote_get_linkage(dispatch_unote_t du)
{
	dispatch_assert(!du._du->du_is_direct);
	return (dispatch_unote_linkage_t)((char *)du._du
			- sizeof(struct dispatch_unote_linkage_s));
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_unote_needs_rearm(dispatch_unote_t du)
{
	return du._du->du_type->dst_flags & (EV_ONESHOT | EV_DISPATCH);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_unote_t
_dispatch_unote_linkage_get_unote(dispatch_unote_linkage_t dul)
{
	return (dispatch_unote_t){ ._du = (dispatch_unote_class_t)(dul + 1) };
}

#endif // DISPATCH_PURE_C

#pragma mark -
#pragma mark prototypes

#if DISPATCH_HAVE_TIMER_QOS
#define DISPATCH_TIMER_QOS_NORMAL       0u
#define DISPATCH_TIMER_QOS_CRITICAL     1u
#define DISPATCH_TIMER_QOS_BACKGROUND   2u
#define DISPATCH_TIMER_QOS_COUNT        3u
#else
#define DISPATCH_TIMER_QOS_NORMAL       0u
#define DISPATCH_TIMER_QOS_COUNT        1u
#endif

#define DISPATCH_TIMER_QOS(tidx)   (((uintptr_t)(tidx) >> 1) & 3u)
#define DISPATCH_TIMER_CLOCK(tidx) (dispatch_clock_t)((tidx) & 1u)

#define DISPATCH_TIMER_INDEX(clock, qos) ((qos) << 1 | (clock))
#define DISPATCH_TIMER_COUNT \
		DISPATCH_TIMER_INDEX(0, DISPATCH_TIMER_QOS_COUNT)
#define DISPATCH_TIMER_IDENT_CANCELED    (~0u)

extern struct dispatch_timer_heap_s _dispatch_timers_heap[DISPATCH_TIMER_COUNT];
extern bool _dispatch_timers_reconfigure, _dispatch_timers_expired;
extern uint32_t _dispatch_timers_processing_mask;
#if DISPATCH_USE_DTRACE
extern uint32_t _dispatch_timers_will_wake;
#endif

dispatch_unote_t _dispatch_unote_create_with_handle(dispatch_source_type_t dst,
		uintptr_t handle, unsigned long mask);
dispatch_unote_t _dispatch_unote_create_with_fd(dispatch_source_type_t dst,
		uintptr_t handle, unsigned long mask);
dispatch_unote_t _dispatch_unote_create_without_handle(
		dispatch_source_type_t dst, uintptr_t handle, unsigned long mask);

bool _dispatch_unote_register(dispatch_unote_t du, dispatch_wlh_t wlh,
		dispatch_priority_t pri);
void _dispatch_unote_resume(dispatch_unote_t du);
bool _dispatch_unote_unregister(dispatch_unote_t du, uint32_t flags);
void _dispatch_unote_dispose(dispatch_unote_t du);

void _dispatch_event_loop_atfork_child(void);
#define DISPATCH_EVENT_LOOP_CONSUME_2 DISPATCH_WAKEUP_CONSUME_2
#define DISPATCH_EVENT_LOOP_OVERRIDE  0x80000000
void _dispatch_event_loop_poke(dispatch_wlh_t wlh, uint64_t dq_state,
		uint32_t flags);
void _dispatch_event_loop_wake_owner(struct dispatch_sync_context_s *dsc,
		dispatch_wlh_t wlh, uint64_t old_state, uint64_t new_state);
void _dispatch_event_loop_wait_for_ownership(
		struct dispatch_sync_context_s *dsc);
void _dispatch_event_loop_end_ownership(dispatch_wlh_t wlh,
		uint64_t old_state, uint64_t new_state, uint32_t flags);
#if DISPATCH_WLH_DEBUG
void _dispatch_event_loop_assert_not_owned(dispatch_wlh_t wlh);
#else
#undef _dispatch_event_loop_assert_not_owned
#define _dispatch_event_loop_assert_not_owned(wlh) ((void)wlh)
#endif
void _dispatch_event_loop_leave_immediate(dispatch_wlh_t wlh, uint64_t dq_state);
#if DISPATCH_EVENT_BACKEND_KEVENT
void _dispatch_event_loop_leave_deferred(dispatch_wlh_t wlh,
		uint64_t dq_state);
void _dispatch_event_loop_merge(dispatch_kevent_t events, int nevents);
#endif
void _dispatch_event_loop_drain(uint32_t flags);
void _dispatch_event_loop_timer_arm(unsigned int tidx,
		dispatch_timer_delay_s range, dispatch_clock_now_cache_t nows);
void _dispatch_event_loop_timer_delete(unsigned int tidx);

#endif /* __DISPATCH_EVENT_EVENT_INTERNAL__ */
