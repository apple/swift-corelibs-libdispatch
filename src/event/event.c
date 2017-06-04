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

#include "internal.h"

DISPATCH_NOINLINE
static dispatch_unote_t
_dispatch_unote_create(dispatch_source_type_t dst,
		uintptr_t handle, unsigned long mask)
{
	dispatch_unote_linkage_t dul;
	dispatch_unote_class_t du;

	if (mask & ~dst->dst_mask) {
		return DISPATCH_UNOTE_NULL;
	}

	if (dst->dst_filter != DISPATCH_EVFILT_TIMER) {
		if (dst->dst_mask && !mask) {
			return DISPATCH_UNOTE_NULL;
		}
	}

	if ((dst->dst_flags & EV_UDATA_SPECIFIC) ||
			(dst->dst_filter == DISPATCH_EVFILT_TIMER)) {
		du = _dispatch_calloc(1u, dst->dst_size);
	} else {
		dul = _dispatch_calloc(1u, sizeof(*dul) + dst->dst_size);
		du = _dispatch_unote_linkage_get_unote(dul)._du;
	}
	du->du_type = dst;
	du->du_can_be_wlh = dst->dst_per_trigger_qos;
	du->du_ident = (uint32_t)handle;
	du->du_filter = dst->dst_filter;
	du->du_fflags = (typeof(du->du_fflags))mask;
	if (dst->dst_flags & EV_UDATA_SPECIFIC) {
		du->du_is_direct = true;
	}
	du->du_data_action = DISPATCH_UNOTE_ACTION_DATA_OR;
	return (dispatch_unote_t){ ._du = du };
}

DISPATCH_NOINLINE
dispatch_unote_t
_dispatch_unote_create_with_handle(dispatch_source_type_t dst,
		uintptr_t handle, unsigned long mask)
{
	if (!handle) {
		return DISPATCH_UNOTE_NULL;
	}
	return _dispatch_unote_create(dst, handle, mask);
}

DISPATCH_NOINLINE
dispatch_unote_t
_dispatch_unote_create_with_fd(dispatch_source_type_t dst,
		uintptr_t handle, unsigned long mask)
{
#if !TARGET_OS_MAC // <rdar://problem/27756657>
	if (handle > INT_MAX) {
		return DISPATCH_UNOTE_NULL;
	}
#endif
	dispatch_unote_t du = _dispatch_unote_create(dst, handle, mask);
	if (du._du) {
		int16_t filter = dst->dst_filter;
		du._du->du_data_action = (filter == EVFILT_READ||filter == EVFILT_WRITE)
			? DISPATCH_UNOTE_ACTION_DATA_SET : DISPATCH_UNOTE_ACTION_DATA_OR;
	}
	return du;
}

DISPATCH_NOINLINE
dispatch_unote_t
_dispatch_unote_create_without_handle(dispatch_source_type_t dst,
		uintptr_t handle, unsigned long mask)
{
	if (handle) {
		return DISPATCH_UNOTE_NULL;
	}
	return _dispatch_unote_create(dst, handle, mask);
}

DISPATCH_NOINLINE
void
_dispatch_unote_dispose(dispatch_unote_t du)
{
	void *ptr = du._du;
#if HAVE_MACH
	if (du._du->dmrr_handler_is_block) {
		Block_release(du._dmrr->dmrr_handler_ctxt);
	}
#endif
	if (du._du->du_is_timer) {
		if (unlikely(du._dt->dt_heap_entry[DTH_TARGET_ID] != DTH_INVALID_ID ||
				du._dt->dt_heap_entry[DTH_DEADLINE_ID] != DTH_INVALID_ID)) {
			DISPATCH_INTERNAL_CRASH(0, "Disposing of timer still in its heap");
		}
		if (unlikely(du._dt->dt_pending_config)) {
			free(du._dt->dt_pending_config);
			du._dt->dt_pending_config = NULL;
		}
	} else if (!du._du->du_is_direct) {
		ptr = _dispatch_unote_get_linkage(du);
	}
	free(ptr);
}

#pragma mark data or / add

static dispatch_unote_t
_dispatch_source_data_create(dispatch_source_type_t dst, uintptr_t handle,
		unsigned long mask)
{
	if (handle || mask) {
		return DISPATCH_UNOTE_NULL;
	}

	// bypass _dispatch_unote_create() because this is always "direct"
	// even when EV_UDATA_SPECIFIC is 0
	dispatch_unote_class_t du = _dispatch_calloc(1u, dst->dst_size);
	du->du_type = dst;
	du->du_filter = dst->dst_filter;
	du->du_is_direct = true;
	return (dispatch_unote_t){ ._du = du };
}

const dispatch_source_type_s _dispatch_source_type_data_add = {
	.dst_kind       = "data-add",
	.dst_filter     = DISPATCH_EVFILT_CUSTOM_ADD,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_CLEAR,
	.dst_size       = sizeof(struct dispatch_source_refs_s),

	.dst_create     = _dispatch_source_data_create,
	.dst_merge_evt  = NULL,
};

const dispatch_source_type_s _dispatch_source_type_data_or = {
	.dst_kind       = "data-or",
	.dst_filter     = DISPATCH_EVFILT_CUSTOM_OR,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_CLEAR,
	.dst_size       = sizeof(struct dispatch_source_refs_s),

	.dst_create     = _dispatch_source_data_create,
	.dst_merge_evt  = NULL,
};

const dispatch_source_type_s _dispatch_source_type_data_replace = {
	.dst_kind       = "data-replace",
	.dst_filter     = DISPATCH_EVFILT_CUSTOM_REPLACE,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_CLEAR,
	.dst_size       = sizeof(struct dispatch_source_refs_s),

	.dst_create     = _dispatch_source_data_create,
	.dst_merge_evt  = NULL,
};

#pragma mark file descriptors

const dispatch_source_type_s _dispatch_source_type_read = {
	.dst_kind       = "read",
	.dst_filter     = EVFILT_READ,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_DISPATCH|EV_VANISHED,
#if DISPATCH_EVENT_BACKEND_KEVENT
#if HAVE_DECL_NOTE_LOWAT
	.dst_fflags     = NOTE_LOWAT,
#endif
	.dst_data       = 1,
#endif // DISPATCH_EVENT_BACKEND_KEVENT
	.dst_size       = sizeof(struct dispatch_source_refs_s),

	.dst_create     = _dispatch_unote_create_with_fd,
	.dst_merge_evt  = _dispatch_source_merge_evt,
};

const dispatch_source_type_s _dispatch_source_type_write = {
	.dst_kind       = "write",
	.dst_filter     = EVFILT_WRITE,
	.dst_flags      = EV_UDATA_SPECIFIC|EV_DISPATCH|EV_VANISHED,
#if DISPATCH_EVENT_BACKEND_KEVENT
#if HAVE_DECL_NOTE_LOWAT
	.dst_fflags     = NOTE_LOWAT,
#endif
	.dst_data       = 1,
#endif // DISPATCH_EVENT_BACKEND_KEVENT
	.dst_size       = sizeof(struct dispatch_source_refs_s),

	.dst_create     = _dispatch_unote_create_with_fd,
	.dst_merge_evt  = _dispatch_source_merge_evt,
};

#pragma mark signals

static dispatch_unote_t
_dispatch_source_signal_create(dispatch_source_type_t dst, uintptr_t handle,
		unsigned long mask)
{
	if (handle >= NSIG) {
		return DISPATCH_UNOTE_NULL;
	}
	dispatch_unote_t du = _dispatch_unote_create_with_handle(dst, handle, mask);
	if (du._du) {
		du._du->du_data_action = DISPATCH_UNOTE_ACTION_DATA_ADD;
	}
	return du;
}

const dispatch_source_type_s _dispatch_source_type_signal = {
	.dst_kind       = "signal",
	.dst_filter     = EVFILT_SIGNAL,
	.dst_flags      = DISPATCH_EV_DIRECT|EV_CLEAR,
	.dst_size       = sizeof(struct dispatch_source_refs_s),

	.dst_create     = _dispatch_source_signal_create,
	.dst_merge_evt  = _dispatch_source_merge_evt,
};

#pragma mark timers

bool _dispatch_timers_reconfigure, _dispatch_timers_expired;
uint32_t _dispatch_timers_processing_mask;
#if DISPATCH_USE_DTRACE
uint32_t _dispatch_timers_will_wake;
#endif
#define DISPATCH_TIMER_HEAP_INITIALIZER(tidx) \
	[tidx] = { \
		.dth_target = UINT64_MAX, \
		.dth_deadline = UINT64_MAX, \
	}
#define DISPATCH_TIMER_HEAP_INIT(kind, qos) \
		DISPATCH_TIMER_HEAP_INITIALIZER(DISPATCH_TIMER_INDEX( \
		DISPATCH_CLOCK_##kind, DISPATCH_TIMER_QOS_##qos))

struct dispatch_timer_heap_s _dispatch_timers_heap[] =  {
	DISPATCH_TIMER_HEAP_INIT(WALL, NORMAL),
	DISPATCH_TIMER_HEAP_INIT(MACH, NORMAL),
#if DISPATCH_HAVE_TIMER_QOS
	DISPATCH_TIMER_HEAP_INIT(WALL, CRITICAL),
	DISPATCH_TIMER_HEAP_INIT(MACH, CRITICAL),
	DISPATCH_TIMER_HEAP_INIT(WALL, BACKGROUND),
	DISPATCH_TIMER_HEAP_INIT(MACH, BACKGROUND),
#endif
};

static dispatch_unote_t
_dispatch_source_timer_create(dispatch_source_type_t dst,
		uintptr_t handle, unsigned long mask)
{
	uint32_t fflags = dst->dst_fflags;
	dispatch_unote_t du;

	// normalize flags
	if (mask & DISPATCH_TIMER_STRICT) {
		mask &= ~(unsigned long)DISPATCH_TIMER_BACKGROUND;
	}

	if (fflags & DISPATCH_TIMER_INTERVAL) {
		if (!handle) return DISPATCH_UNOTE_NULL;
		du = _dispatch_unote_create_without_handle(dst, 0, mask);
	} else {
		du = _dispatch_unote_create_without_handle(dst, handle, mask);
	}

	if (du._dt) {
		du._dt->du_is_timer = true;
		du._dt->du_data_action = DISPATCH_UNOTE_ACTION_DATA_ADD;
		du._dt->du_fflags |= fflags;
		du._dt->du_ident = _dispatch_source_timer_idx(du);
		du._dt->dt_timer.target = UINT64_MAX;
		du._dt->dt_timer.deadline = UINT64_MAX;
		du._dt->dt_timer.interval = UINT64_MAX;
		du._dt->dt_heap_entry[DTH_TARGET_ID] = DTH_INVALID_ID;
		du._dt->dt_heap_entry[DTH_DEADLINE_ID] = DTH_INVALID_ID;
	}
	return du;
}

const dispatch_source_type_s _dispatch_source_type_timer = {
	.dst_kind       = "timer",
	.dst_filter     = DISPATCH_EVFILT_TIMER,
	.dst_flags      = EV_DISPATCH,
	.dst_mask       = DISPATCH_TIMER_STRICT|DISPATCH_TIMER_BACKGROUND,
	.dst_fflags     = 0,
	.dst_size       = sizeof(struct dispatch_timer_source_refs_s),

	.dst_create     = _dispatch_source_timer_create,
};

const dispatch_source_type_s _dispatch_source_type_after = {
	.dst_kind       = "timer (after)",
	.dst_filter     = DISPATCH_EVFILT_TIMER,
	.dst_flags      = EV_DISPATCH,
	.dst_mask       = 0,
	.dst_fflags     = DISPATCH_TIMER_AFTER,
	.dst_size       = sizeof(struct dispatch_timer_source_refs_s),

	.dst_create     = _dispatch_source_timer_create,
};

const dispatch_source_type_s _dispatch_source_type_interval = {
	.dst_kind       = "timer (interval)",
	.dst_filter     = DISPATCH_EVFILT_TIMER,
	.dst_flags      = EV_DISPATCH,
	.dst_mask       = DISPATCH_TIMER_STRICT|DISPATCH_TIMER_BACKGROUND
			|DISPATCH_INTERVAL_UI_ANIMATION,
	.dst_fflags     = DISPATCH_TIMER_INTERVAL|DISPATCH_TIMER_CLOCK_MACH,
	.dst_size       = sizeof(struct dispatch_timer_source_refs_s),

	.dst_create     = _dispatch_source_timer_create,
};
