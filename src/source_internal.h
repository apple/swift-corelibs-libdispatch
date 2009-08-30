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

#ifndef __DISPATCH_SOURCE_INTERNAL__
#define __DISPATCH_SOURCE_INTERNAL__

#ifndef __DISPATCH_INDIRECT__
#error "Please #include <dispatch/dispatch.h> instead of this file directly."
#include <dispatch/base.h> // for HeaderDoc
#endif

struct dispatch_source_vtable_s {
	DISPATCH_VTABLE_HEADER(dispatch_source_s);
};

extern const struct dispatch_source_vtable_s _dispatch_source_kevent_vtable;

struct dispatch_kevent_s {
	TAILQ_ENTRY(dispatch_kevent_s) dk_list;
	TAILQ_HEAD(, dispatch_source_s) dk_sources;
	struct kevent dk_kevent;
};

typedef struct dispatch_kevent_s *dispatch_kevent_t;

struct dispatch_timer_source_s {
	uint64_t target;
	uint64_t start;
	uint64_t interval;
	uint64_t leeway;
	uint64_t flags; // dispatch_timer_flags_t
};

#define DSF_CANCELED 1u // cancellation has been requested

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
			
			dispatch_source_handler_function_t ds_handler_func;
			void *ds_handler_ctxt;
			
			void *ds_cancel_handler;
			
			unsigned int ds_is_level:1,
			ds_is_adder:1,
			ds_is_installed:1,
			ds_needs_rearm:1,
			ds_is_armed:1,
			ds_is_legacy:1,
			ds_cancel_is_block:1,
			ds_handler_is_block:1;

			unsigned int ds_atomic_flags;

			unsigned long ds_data;
			unsigned long ds_pending_data;
			unsigned long ds_pending_data_mask;
			
			TAILQ_ENTRY(dispatch_source_s) ds_list;
			
			unsigned long ds_ident_hack;
			
			struct dispatch_timer_source_s ds_timer;
		};
	};
};


void _dispatch_source_legacy_xref_release(dispatch_source_t ds);

#endif /* __DISPATCH_SOURCE_INTERNAL__ */
