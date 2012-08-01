/*
 * Copyright (c) 2008-2011 Apple Inc. All rights reserved.
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
 * @constant DISPATCH_MACH_RECV_NO_SENDERS
 * Receive right has no more senders. TODO <rdar://problem/8132399>
 */
enum {
	DISPATCH_MACH_RECV_MESSAGE = 0x2,
	DISPATCH_MACH_RECV_NO_SENDERS = 0x10,
};

enum {
	DISPATCH_TIMER_WALL_CLOCK = 0x4,
};

#define DISPATCH_EVFILT_TIMER		(-EVFILT_SYSCOUNT - 1)
#define DISPATCH_EVFILT_CUSTOM_ADD	(-EVFILT_SYSCOUNT - 2)
#define DISPATCH_EVFILT_CUSTOM_OR	(-EVFILT_SYSCOUNT - 3)
#define DISPATCH_EVFILT_SYSCOUNT	( EVFILT_SYSCOUNT + 3)

#define DISPATCH_TIMER_INDEX_WALL	0
#define DISPATCH_TIMER_INDEX_MACH	1
#define DISPATCH_TIMER_INDEX_DISARM	2

struct dispatch_source_vtable_s {
	DISPATCH_VTABLE_HEADER(dispatch_source_s);
};

extern const struct dispatch_source_vtable_s _dispatch_source_kevent_vtable;

struct dispatch_kevent_s {
	TAILQ_ENTRY(dispatch_kevent_s) dk_list;
	TAILQ_HEAD(, dispatch_source_refs_s) dk_sources;
	struct kevent dk_kevent;
};

typedef struct dispatch_kevent_s *dispatch_kevent_t;

struct dispatch_source_type_s {
	struct kevent ke;
	uint64_t mask;
	void (*init)(dispatch_source_t ds, dispatch_source_type_t type,
			uintptr_t handle, unsigned long mask, dispatch_queue_t q);
};

struct dispatch_timer_source_s {
	uint64_t target;
	uint64_t last_fire;
	uint64_t interval;
	uint64_t leeway;
	uint64_t flags; // dispatch_timer_flags_t
	unsigned long missed;
};

// Source state which may contain references to the source object
// Separately allocated so that 'leaks' can see sources <rdar://problem/9050566>
struct dispatch_source_refs_s {
	TAILQ_ENTRY(dispatch_source_refs_s) dr_list;
	uintptr_t dr_source_wref; // "weak" backref to dispatch_source_t
	dispatch_function_t ds_handler_func;
	void *ds_handler_ctxt;
	void *ds_cancel_handler;
	void *ds_registration_handler;
};

typedef struct dispatch_source_refs_s *dispatch_source_refs_t;

struct dispatch_timer_source_refs_s {
	struct dispatch_source_refs_s _ds_refs;
	struct dispatch_timer_source_s _ds_timer;
};

#define _dispatch_ptr2wref(ptr) (~(uintptr_t)(ptr))
#define _dispatch_wref2ptr(ref) ((void*)~(ref))
#define _dispatch_source_from_refs(dr) \
		((dispatch_source_t)_dispatch_wref2ptr((dr)->dr_source_wref))
#define ds_timer(dr) \
		(((struct dispatch_timer_source_refs_s *)(dr))->_ds_timer)

// ds_atomic_flags bits
#define DSF_CANCELED 1u // cancellation has been requested
#define DSF_ARMED 2u // source is armed

struct dispatch_source_s {
	DISPATCH_STRUCT_HEADER(dispatch_source_s, dispatch_source_vtable_s);
	DISPATCH_QUEUE_HEADER;
	// Instruments always copies DISPATCH_QUEUE_MIN_LABEL_SIZE, which is 64,
	// so the remainder of the structure must be big enough
	union {
		char _ds_pad[DISPATCH_QUEUE_MIN_LABEL_SIZE];
		struct {
			char dq_label[8];
			dispatch_kevent_t ds_dkev;
			dispatch_source_refs_t ds_refs;
			unsigned int ds_atomic_flags;
			unsigned int
				ds_is_level:1,
				ds_is_adder:1,
				ds_is_installed:1,
				ds_needs_rearm:1,
				ds_is_timer:1,
				ds_cancel_is_block:1,
				ds_handler_is_block:1,
				ds_registration_is_block:1;
			unsigned long ds_data;
			unsigned long ds_pending_data;
			unsigned long ds_pending_data_mask;
			unsigned long ds_ident_hack;
		};
	};
};

void _dispatch_source_xref_release(dispatch_source_t ds);
void _dispatch_mach_notify_source_init(void *context);

#endif /* __DISPATCH_SOURCE_INTERNAL__ */
