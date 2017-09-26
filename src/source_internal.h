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

enum {
	/* DISPATCH_TIMER_STRICT 0x1 */
	/* DISPATCH_TIMER_BACKGROUND = 0x2, */
	DISPATCH_TIMER_CLOCK_MACH = 0x4,
	DISPATCH_TIMER_INTERVAL = 0x8,
	DISPATCH_TIMER_AFTER = 0x10,
	/* DISPATCH_INTERVAL_UI_ANIMATION = 0x20 */
};

DISPATCH_ALWAYS_INLINE
static inline unsigned int
_dispatch_source_timer_idx(dispatch_unote_t du)
{
	uint32_t clock, qos = 0, fflags = du._dt->du_fflags;

	dispatch_assert(DISPATCH_CLOCK_MACH == 1);
	dispatch_assert(DISPATCH_CLOCK_WALL == 0);
	clock = (fflags & DISPATCH_TIMER_CLOCK_MACH) / DISPATCH_TIMER_CLOCK_MACH;

#if DISPATCH_HAVE_TIMER_QOS
	dispatch_assert(DISPATCH_TIMER_STRICT == DISPATCH_TIMER_QOS_CRITICAL);
	dispatch_assert(DISPATCH_TIMER_BACKGROUND == DISPATCH_TIMER_QOS_BACKGROUND);
	qos = fflags & (DISPATCH_TIMER_STRICT | DISPATCH_TIMER_BACKGROUND);
	// flags are normalized so this should never happen
	dispatch_assert(qos < DISPATCH_TIMER_QOS_COUNT);
#endif

	return DISPATCH_TIMER_INDEX(clock, qos);
}

#define _DISPATCH_SOURCE_HEADER(refs) \
	DISPATCH_QUEUE_HEADER(refs); \
	unsigned int \
		ds_is_installed:1, \
		dm_needs_mgr:1, \
		dm_connect_handler_called:1, \
		dm_uninstalled:1, \
		dm_cancel_handler_called:1, \
		dm_is_xpc:1

#define DISPATCH_SOURCE_HEADER(refs) \
	struct dispatch_source_s _as_ds[0]; \
	_DISPATCH_SOURCE_HEADER(refs)

DISPATCH_CLASS_DECL_BARE(source);
_OS_OBJECT_CLASS_IMPLEMENTS_PROTOCOL(dispatch_source, dispatch_object);

#ifndef __cplusplus
struct dispatch_source_s {
	_DISPATCH_SOURCE_HEADER(source);
	uint64_t ds_data DISPATCH_ATOMIC64_ALIGN;
	uint64_t ds_pending_data DISPATCH_ATOMIC64_ALIGN;
} DISPATCH_ATOMIC64_ALIGN;

// Extracts source data from the ds_data field
#define DISPATCH_SOURCE_GET_DATA(d) ((d) & 0xFFFFFFFF)

// Extracts status from the ds_data field
#define DISPATCH_SOURCE_GET_STATUS(d) ((d) >> 32)

// Combine data and status for the ds_data field
#define DISPATCH_SOURCE_COMBINE_DATA_AND_STATUS(data, status) \
		((((uint64_t)(status)) << 32) | (data))

#endif // __cplusplus

void _dispatch_source_refs_register(dispatch_source_t ds,
		dispatch_wlh_t wlh, dispatch_priority_t bp);
void _dispatch_source_refs_unregister(dispatch_source_t ds, uint32_t options);
void _dispatch_source_xref_dispose(dispatch_source_t ds);
void _dispatch_source_dispose(dispatch_source_t ds, bool *allow_free);
void _dispatch_source_finalize_activation(dispatch_source_t ds,
		bool *allow_resume);
void _dispatch_source_invoke(dispatch_source_t ds,
		dispatch_invoke_context_t dic, dispatch_invoke_flags_t flags);
void _dispatch_source_wakeup(dispatch_source_t ds, dispatch_qos_t qos,
		dispatch_wakeup_flags_t flags);
void _dispatch_source_merge_evt(dispatch_unote_t du, uint32_t flags,
		uintptr_t data, uintptr_t status, pthread_priority_t pp);
size_t _dispatch_source_debug(dispatch_source_t ds, char* buf, size_t bufsiz);

DISPATCH_EXPORT // for firehose server
void _dispatch_source_merge_data(dispatch_source_t ds, pthread_priority_t pp,
		unsigned long val);

void _dispatch_mgr_queue_push(dispatch_queue_t dq, dispatch_object_t dou,
		dispatch_qos_t qos);
void _dispatch_mgr_queue_wakeup(dispatch_queue_t dq, dispatch_qos_t qos,
		dispatch_wakeup_flags_t flags);
void _dispatch_mgr_thread(dispatch_queue_t dq, dispatch_invoke_context_t dic,
		dispatch_invoke_flags_t flags);
#if DISPATCH_USE_KEVENT_WORKQUEUE
void _dispatch_kevent_worker_thread(dispatch_kevent_t *events,
		int *nevents);
#endif // DISPATCH_USE_KEVENT_WORKQUEUE

#endif /* __DISPATCH_SOURCE_INTERNAL__ */
