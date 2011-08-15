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

#include "internal.h"
#if HAVE_MACH
#include "protocol.h"
#include "protocolServer.h"
#endif
#include <sys/mount.h>

static void _dispatch_source_dispose(dispatch_source_t ds);
static dispatch_queue_t _dispatch_source_invoke(dispatch_source_t ds);
static bool _dispatch_source_probe(dispatch_source_t ds);
static void _dispatch_source_merge_kevent(dispatch_source_t ds,
		const struct kevent *ke);
static void _dispatch_kevent_register(dispatch_source_t ds);
static void _dispatch_kevent_unregister(dispatch_source_t ds);
static bool _dispatch_kevent_resume(dispatch_kevent_t dk, uint32_t new_flags,
		uint32_t del_flags);
static inline void _dispatch_source_timer_init(void);
static void _dispatch_timer_list_update(dispatch_source_t ds);
static inline unsigned long _dispatch_source_timer_data(
		dispatch_source_refs_t dr, unsigned long prev);
#if HAVE_MACH
static kern_return_t _dispatch_kevent_machport_resume(dispatch_kevent_t dk,
		uint32_t new_flags, uint32_t del_flags);
static void _dispatch_drain_mach_messages(struct kevent *ke);
#endif
static size_t _dispatch_source_kevent_debug(dispatch_source_t ds,
		char* buf, size_t bufsiz);
#if DISPATCH_DEBUG
static void _dispatch_kevent_debugger(void *context);
#endif

#pragma mark -
#pragma mark dispatch_source_t

const struct dispatch_source_vtable_s _dispatch_source_kevent_vtable = {
	.do_type = DISPATCH_SOURCE_KEVENT_TYPE,
	.do_kind = "kevent-source",
	.do_invoke = _dispatch_source_invoke,
	.do_dispose = _dispatch_source_dispose,
	.do_probe = _dispatch_source_probe,
	.do_debug = _dispatch_source_kevent_debug,
};

dispatch_source_t
dispatch_source_create(dispatch_source_type_t type,
	uintptr_t handle,
	unsigned long mask,
	dispatch_queue_t q)
{
	const struct kevent *proto_kev = &type->ke;
	dispatch_source_t ds = NULL;
	dispatch_kevent_t dk = NULL;

	// input validation
	if (type == NULL || (mask & ~type->mask)) {
		goto out_bad;
	}

	switch (type->ke.filter) {
	case EVFILT_SIGNAL:
		if (handle >= NSIG) {
			goto out_bad;
		}
		break;
	case EVFILT_FS:
#if DISPATCH_USE_VM_PRESSURE
	case EVFILT_VM:
#endif
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
	case DISPATCH_EVFILT_TIMER:
		if (handle) {
			goto out_bad;
		}
		break;
	default:
		break;
	}

	ds = calloc(1ul, sizeof(struct dispatch_source_s));
	if (slowpath(!ds)) {
		goto out_bad;
	}
	dk = calloc(1ul, sizeof(struct dispatch_kevent_s));
	if (slowpath(!dk)) {
		goto out_bad;
	}

	dk->dk_kevent = *proto_kev;
	dk->dk_kevent.ident = handle;
	dk->dk_kevent.flags |= EV_ADD|EV_ENABLE;
	dk->dk_kevent.fflags |= (uint32_t)mask;
	dk->dk_kevent.udata = dk;
	TAILQ_INIT(&dk->dk_sources);

	// Initialize as a queue first, then override some settings below.
	_dispatch_queue_init((dispatch_queue_t)ds);
	strlcpy(ds->dq_label, "source", sizeof(ds->dq_label));

	// Dispatch Object
	ds->do_vtable = &_dispatch_source_kevent_vtable;
	ds->do_ref_cnt++; // the reference the manger queue holds
	ds->do_ref_cnt++; // since source is created suspended
	ds->do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_INTERVAL;
	// The initial target queue is the manager queue, in order to get
	// the source installed. <rdar://problem/8928171>
	ds->do_targetq = &_dispatch_mgr_q;

	// Dispatch Source
	ds->ds_ident_hack = dk->dk_kevent.ident;
	ds->ds_dkev = dk;
	ds->ds_pending_data_mask = dk->dk_kevent.fflags;
	if ((EV_DISPATCH|EV_ONESHOT) & proto_kev->flags) {
		ds->ds_is_level = true;
		ds->ds_needs_rearm = true;
	} else if (!(EV_CLEAR & proto_kev->flags)) {
		// we cheat and use EV_CLEAR to mean a "flag thingy"
		ds->ds_is_adder = true;
	}

	// Some sources require special processing
	if (type->init != NULL) {
		type->init(ds, type, handle, mask, q);
	}
	if (fastpath(!ds->ds_refs)) {
		ds->ds_refs = calloc(1ul, sizeof(struct dispatch_source_refs_s));
		if (slowpath(!ds->ds_refs)) {
			goto out_bad;
		}
	}
	ds->ds_refs->dr_source_wref = _dispatch_ptr2wref(ds);
	dispatch_assert(!(ds->ds_is_level && ds->ds_is_adder));

	// First item on the queue sets the user-specified target queue
	dispatch_set_target_queue(ds, q);
#if DISPATCH_DEBUG
	dispatch_debug(ds, "%s", __FUNCTION__);
#endif
	return ds;

out_bad:
	free(ds);
	free(dk);
	return NULL;
}

static void
_dispatch_source_dispose(dispatch_source_t ds)
{
	free(ds->ds_refs);
	_dispatch_queue_dispose((dispatch_queue_t)ds);
}

void
_dispatch_source_xref_release(dispatch_source_t ds)
{
	if (slowpath(DISPATCH_OBJECT_SUSPENDED(ds))) {
		// Arguments for and against this assert are within 6705399
		DISPATCH_CLIENT_CRASH("Release of a suspended object");
	}
	_dispatch_wakeup(ds);
	_dispatch_release(ds);
}

void
dispatch_source_cancel(dispatch_source_t ds)
{
#if DISPATCH_DEBUG
	dispatch_debug(ds, "%s", __FUNCTION__);
#endif
	// Right after we set the cancel flag, someone else
	// could potentially invoke the source, do the cancelation,
	// unregister the source, and deallocate it. We would
	// need to therefore retain/release before setting the bit

	_dispatch_retain(ds);
	(void)dispatch_atomic_or2o(ds, ds_atomic_flags, DSF_CANCELED);
	_dispatch_wakeup(ds);
	_dispatch_release(ds);
}

long
dispatch_source_testcancel(dispatch_source_t ds)
{
	return (bool)(ds->ds_atomic_flags & DSF_CANCELED);
}


unsigned long
dispatch_source_get_mask(dispatch_source_t ds)
{
	return ds->ds_pending_data_mask;
}

uintptr_t
dispatch_source_get_handle(dispatch_source_t ds)
{
	return (int)ds->ds_ident_hack;
}

unsigned long
dispatch_source_get_data(dispatch_source_t ds)
{
	return ds->ds_data;
}

void
dispatch_source_merge_data(dispatch_source_t ds, unsigned long val)
{
	struct kevent kev = {
		.fflags = (typeof(kev.fflags))val,
		.data = val,
	};

	dispatch_assert(
			ds->ds_dkev->dk_kevent.filter == DISPATCH_EVFILT_CUSTOM_ADD ||
			ds->ds_dkev->dk_kevent.filter == DISPATCH_EVFILT_CUSTOM_OR);

	_dispatch_source_merge_kevent(ds, &kev);
}

#pragma mark -
#pragma mark dispatch_source_handler

#ifdef __BLOCKS__
// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
static void
_dispatch_source_set_event_handler2(void *context)
{
	struct Block_layout *bl = context;

	dispatch_source_t ds = (dispatch_source_t)_dispatch_queue_get_current();
	dispatch_assert(ds->do_vtable == &_dispatch_source_kevent_vtable);
	dispatch_source_refs_t dr = ds->ds_refs;

	if (ds->ds_handler_is_block && dr->ds_handler_ctxt) {
		Block_release(dr->ds_handler_ctxt);
	}
	dr->ds_handler_func = bl ? (void *)bl->invoke : NULL;
	dr->ds_handler_ctxt = bl;
	ds->ds_handler_is_block = true;
}

void
dispatch_source_set_event_handler(dispatch_source_t ds,
		dispatch_block_t handler)
{
	handler = _dispatch_Block_copy(handler);
	dispatch_barrier_async_f((dispatch_queue_t)ds, handler,
			_dispatch_source_set_event_handler2);
}
#endif /* __BLOCKS__ */

static void
_dispatch_source_set_event_handler_f(void *context)
{
	dispatch_source_t ds = (dispatch_source_t)_dispatch_queue_get_current();
	dispatch_assert(ds->do_vtable == &_dispatch_source_kevent_vtable);
	dispatch_source_refs_t dr = ds->ds_refs;

#ifdef __BLOCKS__
	if (ds->ds_handler_is_block && dr->ds_handler_ctxt) {
		Block_release(dr->ds_handler_ctxt);
	}
#endif
	dr->ds_handler_func = context;
	dr->ds_handler_ctxt = ds->do_ctxt;
	ds->ds_handler_is_block = false;
}

void
dispatch_source_set_event_handler_f(dispatch_source_t ds,
	dispatch_function_t handler)
{
	dispatch_barrier_async_f((dispatch_queue_t)ds, handler,
			_dispatch_source_set_event_handler_f);
}

#ifdef __BLOCKS__
// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
static void
_dispatch_source_set_cancel_handler2(void *context)
{
	dispatch_source_t ds = (dispatch_source_t)_dispatch_queue_get_current();
	dispatch_assert(ds->do_vtable == &_dispatch_source_kevent_vtable);
	dispatch_source_refs_t dr = ds->ds_refs;

	if (ds->ds_cancel_is_block && dr->ds_cancel_handler) {
		Block_release(dr->ds_cancel_handler);
	}
	dr->ds_cancel_handler = context;
	ds->ds_cancel_is_block = true;
}

void
dispatch_source_set_cancel_handler(dispatch_source_t ds,
	dispatch_block_t handler)
{
	handler = _dispatch_Block_copy(handler);
	dispatch_barrier_async_f((dispatch_queue_t)ds, handler,
			_dispatch_source_set_cancel_handler2);
}
#endif /* __BLOCKS__ */

static void
_dispatch_source_set_cancel_handler_f(void *context)
{
	dispatch_source_t ds = (dispatch_source_t)_dispatch_queue_get_current();
	dispatch_assert(ds->do_vtable == &_dispatch_source_kevent_vtable);
	dispatch_source_refs_t dr = ds->ds_refs;

#ifdef __BLOCKS__
	if (ds->ds_cancel_is_block && dr->ds_cancel_handler) {
		Block_release(dr->ds_cancel_handler);
	}
#endif
	dr->ds_cancel_handler = context;
	ds->ds_cancel_is_block = false;
}

void
dispatch_source_set_cancel_handler_f(dispatch_source_t ds,
	dispatch_function_t handler)
{
	dispatch_barrier_async_f((dispatch_queue_t)ds, handler,
			_dispatch_source_set_cancel_handler_f);
}

#ifdef __BLOCKS__
static void
_dispatch_source_set_registration_handler2(void *context)
{
	dispatch_source_t ds = (dispatch_source_t)_dispatch_queue_get_current();
	dispatch_assert(ds->do_vtable == &_dispatch_source_kevent_vtable);
	dispatch_source_refs_t dr = ds->ds_refs;

	if (ds->ds_registration_is_block && dr->ds_registration_handler) {
		Block_release(dr->ds_registration_handler);
	}
	dr->ds_registration_handler = context;
	ds->ds_registration_is_block = true;
}

void
dispatch_source_set_registration_handler(dispatch_source_t ds,
	dispatch_block_t handler)
{
	handler = _dispatch_Block_copy(handler);
	dispatch_barrier_async_f((dispatch_queue_t)ds, handler,
			_dispatch_source_set_registration_handler2);
}
#endif /* __BLOCKS__ */

static void
_dispatch_source_set_registration_handler_f(void *context)
{
	dispatch_source_t ds = (dispatch_source_t)_dispatch_queue_get_current();
	dispatch_assert(ds->do_vtable == &_dispatch_source_kevent_vtable);
	dispatch_source_refs_t dr = ds->ds_refs;

#ifdef __BLOCKS__
	if (ds->ds_registration_is_block && dr->ds_registration_handler) {
		Block_release(dr->ds_registration_handler);
	}
#endif
	dr->ds_registration_handler = context;
	ds->ds_registration_is_block = false;
}

void
dispatch_source_set_registration_handler_f(dispatch_source_t ds,
	dispatch_function_t handler)
{
	dispatch_barrier_async_f((dispatch_queue_t)ds, handler,
			_dispatch_source_set_registration_handler_f);
}

#pragma mark -
#pragma mark dispatch_source_invoke

static void
_dispatch_source_registration_callout(dispatch_source_t ds)
{
	dispatch_source_refs_t dr = ds->ds_refs;

	if ((ds->ds_atomic_flags & DSF_CANCELED) || (ds->do_xref_cnt == 0)) {
		// no registration callout if source is canceled rdar://problem/8955246
#ifdef __BLOCKS__
		if (ds->ds_registration_is_block) {
			Block_release(dr->ds_registration_handler);
		}
	} else if (ds->ds_registration_is_block) {
		dispatch_block_t b = dr->ds_registration_handler;
		_dispatch_client_callout_block(b);
		Block_release(dr->ds_registration_handler);
#endif
	} else {
		dispatch_function_t f = dr->ds_registration_handler;
		_dispatch_client_callout(ds->do_ctxt, f);
	}
	ds->ds_registration_is_block = false;
	dr->ds_registration_handler = NULL;
}

static void
_dispatch_source_cancel_callout(dispatch_source_t ds)
{
	dispatch_source_refs_t dr = ds->ds_refs;

	ds->ds_pending_data_mask = 0;
	ds->ds_pending_data = 0;
	ds->ds_data = 0;

#ifdef __BLOCKS__
	if (ds->ds_handler_is_block) {
		Block_release(dr->ds_handler_ctxt);
		ds->ds_handler_is_block = false;
		dr->ds_handler_func = NULL;
		dr->ds_handler_ctxt = NULL;
	}
	if (ds->ds_registration_is_block) {
		Block_release(dr->ds_registration_handler);
		ds->ds_registration_is_block = false;
		dr->ds_registration_handler = NULL;
	}
#endif

	if (!dr->ds_cancel_handler) {
		return;
	}
	if (ds->ds_cancel_is_block) {
#ifdef __BLOCKS__
		dispatch_block_t b = dr->ds_cancel_handler;
		if (ds->ds_atomic_flags & DSF_CANCELED) {
			_dispatch_client_callout_block(b);
		}
		Block_release(dr->ds_cancel_handler);
		ds->ds_cancel_is_block = false;
#endif
	} else {
		dispatch_function_t f = dr->ds_cancel_handler;
		if (ds->ds_atomic_flags & DSF_CANCELED) {
			_dispatch_client_callout(ds->do_ctxt, f);
		}
	}
	dr->ds_cancel_handler = NULL;
}

static void
_dispatch_source_latch_and_call(dispatch_source_t ds)
{
	unsigned long prev;

	if ((ds->ds_atomic_flags & DSF_CANCELED) || (ds->do_xref_cnt == 0)) {
		return;
	}
	dispatch_source_refs_t dr = ds->ds_refs;
	prev = dispatch_atomic_xchg2o(ds, ds_pending_data, 0);
	if (ds->ds_is_level) {
		ds->ds_data = ~prev;
	} else if (ds->ds_is_timer && ds_timer(dr).target && prev) {
		ds->ds_data = _dispatch_source_timer_data(dr, prev);
	} else {
		ds->ds_data = prev;
	}
	if (dispatch_assume(prev) && dr->ds_handler_func) {
		_dispatch_client_callout(dr->ds_handler_ctxt, dr->ds_handler_func);
	}
}

static void
_dispatch_source_kevent_resume(dispatch_source_t ds, uint32_t new_flags)
{
	switch (ds->ds_dkev->dk_kevent.filter) {
	case DISPATCH_EVFILT_TIMER:
		// called on manager queue only
		return _dispatch_timer_list_update(ds);
	case EVFILT_MACHPORT:
		if (ds->ds_pending_data_mask & DISPATCH_MACH_RECV_MESSAGE) {
			new_flags |= DISPATCH_MACH_RECV_MESSAGE; // emulate EV_DISPATCH
		}
		break;
	}
	if (_dispatch_kevent_resume(ds->ds_dkev, new_flags, 0)) {
		_dispatch_kevent_unregister(ds);
	}
}

static dispatch_queue_t
_dispatch_source_invoke(dispatch_source_t ds)
{
	// This function performs all source actions. Each action is responsible
	// for verifying that it takes place on the appropriate queue. If the
	// current queue is not the correct queue for this action, the correct queue
	// will be returned and the invoke will be re-driven on that queue.

	// The order of tests here in invoke and in probe should be consistent.

	dispatch_queue_t dq = _dispatch_queue_get_current();
	dispatch_source_refs_t dr = ds->ds_refs;

	if (!ds->ds_is_installed) {
		// The source needs to be installed on the manager queue.
		if (dq != &_dispatch_mgr_q) {
			return &_dispatch_mgr_q;
		}
		_dispatch_kevent_register(ds);
		if (dr->ds_registration_handler) {
			return ds->do_targetq;
		}
		if (slowpath(ds->do_xref_cnt == 0)) {
			return &_dispatch_mgr_q; // rdar://problem/9558246
		}
	} else if (slowpath(DISPATCH_OBJECT_SUSPENDED(ds))) {
		// Source suspended by an item drained from the source queue.
		return NULL;
	} else if (dr->ds_registration_handler) {
		// The source has been registered and the registration handler needs
		// to be delivered on the target queue.
		if (dq != ds->do_targetq) {
			return ds->do_targetq;
		}
		// clears ds_registration_handler
		_dispatch_source_registration_callout(ds);
		if (slowpath(ds->do_xref_cnt == 0)) {
			return &_dispatch_mgr_q; // rdar://problem/9558246
		}
	} else if ((ds->ds_atomic_flags & DSF_CANCELED) || (ds->do_xref_cnt == 0)) {
		// The source has been cancelled and needs to be uninstalled from the
		// manager queue. After uninstallation, the cancellation handler needs
		// to be delivered to the target queue.
		if (ds->ds_dkev) {
			if (dq != &_dispatch_mgr_q) {
				return &_dispatch_mgr_q;
			}
			_dispatch_kevent_unregister(ds);
			return ds->do_targetq;
		} else if (dr->ds_cancel_handler) {
			if (dq != ds->do_targetq) {
				return ds->do_targetq;
			}
		}
		_dispatch_source_cancel_callout(ds);
	} else if (ds->ds_pending_data) {
		// The source has pending data to deliver via the event handler callback
		// on the target queue. Some sources need to be rearmed on the manager
		// queue after event delivery.
		if (dq != ds->do_targetq) {
			return ds->do_targetq;
		}
		_dispatch_source_latch_and_call(ds);
		if (ds->ds_needs_rearm) {
			return &_dispatch_mgr_q;
		}
	} else if (ds->ds_needs_rearm && !(ds->ds_atomic_flags & DSF_ARMED)) {
		// The source needs to be rearmed on the manager queue.
		if (dq != &_dispatch_mgr_q) {
			return &_dispatch_mgr_q;
		}
		_dispatch_source_kevent_resume(ds, 0);
		(void)dispatch_atomic_or2o(ds, ds_atomic_flags, DSF_ARMED);
	}

	return NULL;
}

static bool
_dispatch_source_probe(dispatch_source_t ds)
{
	// This function determines whether the source needs to be invoked.
	// The order of tests here in probe and in invoke should be consistent.

	dispatch_source_refs_t dr = ds->ds_refs;
	if (!ds->ds_is_installed) {
		// The source needs to be installed on the manager queue.
		return true;
	} else if (dr->ds_registration_handler) {
		// The registration handler needs to be delivered to the target queue.
		return true;
	} else if ((ds->ds_atomic_flags & DSF_CANCELED) || (ds->do_xref_cnt == 0)) {
		// The source needs to be uninstalled from the manager queue, or the
		// cancellation handler needs to be delivered to the target queue.
		// Note: cancellation assumes installation.
		if (ds->ds_dkev || dr->ds_cancel_handler) {
			return true;
		}
	} else if (ds->ds_pending_data) {
		// The source has pending data to deliver to the target queue.
		return true;
	} else if (ds->ds_needs_rearm && !(ds->ds_atomic_flags & DSF_ARMED)) {
		// The source needs to be rearmed on the manager queue.
		return true;
	}
	// Nothing to do.
	return false;
}

#pragma mark -
#pragma mark dispatch_source_kevent

static void
_dispatch_source_merge_kevent(dispatch_source_t ds, const struct kevent *ke)
{
	struct kevent fake;

	if ((ds->ds_atomic_flags & DSF_CANCELED) || (ds->do_xref_cnt == 0)) {
		return;
	}

	// EVFILT_PROC may fail with ESRCH when the process exists but is a zombie
	// <rdar://problem/5067725>. As a workaround, we simulate an exit event for
	// any EVFILT_PROC with an invalid pid <rdar://problem/6626350>.
	if (ke->flags & EV_ERROR) {
		if (ke->filter == EVFILT_PROC && ke->data == ESRCH) {
			fake = *ke;
			fake.flags &= ~EV_ERROR;
			fake.fflags = NOTE_EXIT;
			fake.data = 0;
			ke = &fake;
#if DISPATCH_USE_VM_PRESSURE
		} else if (ke->filter == EVFILT_VM && ke->data == ENOTSUP) {
			// Memory pressure kevent is not supported on all platforms
			// <rdar://problem/8636227>
			return;
#endif
		} else {
			// log the unexpected error
			(void)dispatch_assume_zero(ke->data);
			return;
		}
	}

	if (ds->ds_is_level) {
		// ke->data is signed and "negative available data" makes no sense
		// zero bytes happens when EV_EOF is set
		// 10A268 does not fail this assert with EVFILT_READ and a 10 GB file
		dispatch_assert(ke->data >= 0l);
		ds->ds_pending_data = ~ke->data;
	} else if (ds->ds_is_adder) {
		(void)dispatch_atomic_add2o(ds, ds_pending_data, ke->data);
	} else if (ke->fflags & ds->ds_pending_data_mask) {
		(void)dispatch_atomic_or2o(ds, ds_pending_data,
				ke->fflags & ds->ds_pending_data_mask);
	}

	// EV_DISPATCH and EV_ONESHOT sources are no longer armed after delivery
	if (ds->ds_needs_rearm) {
		(void)dispatch_atomic_and2o(ds, ds_atomic_flags, ~DSF_ARMED);
	}

	_dispatch_wakeup(ds);
}

void
_dispatch_source_drain_kevent(struct kevent *ke)
{
	dispatch_kevent_t dk = ke->udata;
	dispatch_source_refs_t dri;

#if DISPATCH_DEBUG
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_kevent_debugger);
#endif

	dispatch_debug_kevents(ke, 1, __func__);

#if HAVE_MACH
	if (ke->filter == EVFILT_MACHPORT) {
		return _dispatch_drain_mach_messages(ke);
	}
#endif
	dispatch_assert(dk);

	if (ke->flags & EV_ONESHOT) {
		dk->dk_kevent.flags |= EV_ONESHOT;
	}

	TAILQ_FOREACH(dri, &dk->dk_sources, dr_list) {
		_dispatch_source_merge_kevent(_dispatch_source_from_refs(dri), ke);
	}
}

#pragma mark -
#pragma mark dispatch_kevent_t

static struct dispatch_kevent_s _dispatch_kevent_data_or = {
	.dk_kevent = {
		.filter = DISPATCH_EVFILT_CUSTOM_OR,
		.flags = EV_CLEAR,
		.udata = &_dispatch_kevent_data_or,
	},
	.dk_sources = TAILQ_HEAD_INITIALIZER(_dispatch_kevent_data_or.dk_sources),
};
static struct dispatch_kevent_s _dispatch_kevent_data_add = {
	.dk_kevent = {
		.filter = DISPATCH_EVFILT_CUSTOM_ADD,
		.udata = &_dispatch_kevent_data_add,
	},
	.dk_sources = TAILQ_HEAD_INITIALIZER(_dispatch_kevent_data_add.dk_sources),
};

#if TARGET_OS_EMBEDDED
#define DSL_HASH_SIZE  64u // must be a power of two
#else
#define DSL_HASH_SIZE 256u // must be a power of two
#endif
#define DSL_HASH(x) ((x) & (DSL_HASH_SIZE - 1))

DISPATCH_CACHELINE_ALIGN
static TAILQ_HEAD(, dispatch_kevent_s) _dispatch_sources[DSL_HASH_SIZE];

static dispatch_once_t __dispatch_kevent_init_pred;

static void
_dispatch_kevent_init(void *context DISPATCH_UNUSED)
{
	unsigned int i;
	for (i = 0; i < DSL_HASH_SIZE; i++) {
		TAILQ_INIT(&_dispatch_sources[i]);
	}

	TAILQ_INSERT_TAIL(&_dispatch_sources[0],
			&_dispatch_kevent_data_or, dk_list);
	TAILQ_INSERT_TAIL(&_dispatch_sources[0],
			&_dispatch_kevent_data_add, dk_list);

	_dispatch_source_timer_init();
}

static inline uintptr_t
_dispatch_kevent_hash(uintptr_t ident, short filter)
{
	uintptr_t value;
#if HAVE_MACH
	value = (filter == EVFILT_MACHPORT ? MACH_PORT_INDEX(ident) : ident);
#else
	value = ident;
#endif
	return DSL_HASH(value);
}

static dispatch_kevent_t
_dispatch_kevent_find(uintptr_t ident, short filter)
{
	uintptr_t hash = _dispatch_kevent_hash(ident, filter);
	dispatch_kevent_t dki;

	TAILQ_FOREACH(dki, &_dispatch_sources[hash], dk_list) {
		if (dki->dk_kevent.ident == ident && dki->dk_kevent.filter == filter) {
			break;
		}
	}
	return dki;
}

static void
_dispatch_kevent_insert(dispatch_kevent_t dk)
{
	uintptr_t hash = _dispatch_kevent_hash(dk->dk_kevent.ident,
			dk->dk_kevent.filter);

	TAILQ_INSERT_TAIL(&_dispatch_sources[hash], dk, dk_list);
}

// Find existing kevents, and merge any new flags if necessary
static void
_dispatch_kevent_register(dispatch_source_t ds)
{
	dispatch_kevent_t dk;
	typeof(dk->dk_kevent.fflags) new_flags;
	bool do_resume = false;

	if (ds->ds_is_installed) {
		return;
	}
	ds->ds_is_installed = true;

	dispatch_once_f(&__dispatch_kevent_init_pred,
			NULL, _dispatch_kevent_init);

	dk = _dispatch_kevent_find(ds->ds_dkev->dk_kevent.ident,
			ds->ds_dkev->dk_kevent.filter);

	if (dk) {
		// If an existing dispatch kevent is found, check to see if new flags
		// need to be added to the existing kevent
		new_flags = ~dk->dk_kevent.fflags & ds->ds_dkev->dk_kevent.fflags;
		dk->dk_kevent.fflags |= ds->ds_dkev->dk_kevent.fflags;
		free(ds->ds_dkev);
		ds->ds_dkev = dk;
		do_resume = new_flags;
	} else {
		dk = ds->ds_dkev;
		_dispatch_kevent_insert(dk);
		new_flags = dk->dk_kevent.fflags;
		do_resume = true;
	}

	TAILQ_INSERT_TAIL(&dk->dk_sources, ds->ds_refs, dr_list);

	// Re-register the kevent with the kernel if new flags were added
	// by the dispatch kevent
	if (do_resume) {
		dk->dk_kevent.flags |= EV_ADD;
	}
	if (do_resume || ds->ds_needs_rearm) {
		_dispatch_source_kevent_resume(ds, new_flags);
	}
	(void)dispatch_atomic_or2o(ds, ds_atomic_flags, DSF_ARMED);
}

static bool
_dispatch_kevent_resume(dispatch_kevent_t dk, uint32_t new_flags,
		uint32_t del_flags)
{
	long r;
	switch (dk->dk_kevent.filter) {
	case DISPATCH_EVFILT_TIMER:
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
		// these types not registered with kevent
		return 0;
#if HAVE_MACH
	case EVFILT_MACHPORT:
		return _dispatch_kevent_machport_resume(dk, new_flags, del_flags);
#endif
	case EVFILT_PROC:
		if (dk->dk_kevent.flags & EV_ONESHOT) {
			return 0;
		}
		// fall through
	default:
		r = _dispatch_update_kq(&dk->dk_kevent);
		if (dk->dk_kevent.flags & EV_DISPATCH) {
			dk->dk_kevent.flags &= ~EV_ADD;
		}
		return r;
	}
}

static void
_dispatch_kevent_dispose(dispatch_kevent_t dk)
{
	uintptr_t hash;

	switch (dk->dk_kevent.filter) {
	case DISPATCH_EVFILT_TIMER:
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
		// these sources live on statically allocated lists
		return;
#if HAVE_MACH
	case EVFILT_MACHPORT:
		_dispatch_kevent_machport_resume(dk, 0, dk->dk_kevent.fflags);
		break;
#endif
	case EVFILT_PROC:
		if (dk->dk_kevent.flags & EV_ONESHOT) {
			break; // implicitly deleted
		}
		// fall through
	default:
		if (~dk->dk_kevent.flags & EV_DELETE) {
			dk->dk_kevent.flags |= EV_DELETE;
			_dispatch_update_kq(&dk->dk_kevent);
		}
		break;
	}

	hash = _dispatch_kevent_hash(dk->dk_kevent.ident,
			dk->dk_kevent.filter);
	TAILQ_REMOVE(&_dispatch_sources[hash], dk, dk_list);
	free(dk);
}

static void
_dispatch_kevent_unregister(dispatch_source_t ds)
{
	dispatch_kevent_t dk = ds->ds_dkev;
	dispatch_source_refs_t dri;
	uint32_t del_flags, fflags = 0;

	ds->ds_dkev = NULL;

	TAILQ_REMOVE(&dk->dk_sources, ds->ds_refs, dr_list);

	if (TAILQ_EMPTY(&dk->dk_sources)) {
		_dispatch_kevent_dispose(dk);
	} else {
		TAILQ_FOREACH(dri, &dk->dk_sources, dr_list) {
			dispatch_source_t dsi = _dispatch_source_from_refs(dri);
			fflags |= (uint32_t)dsi->ds_pending_data_mask;
		}
		del_flags = (uint32_t)ds->ds_pending_data_mask & ~fflags;
		if (del_flags) {
			dk->dk_kevent.flags |= EV_ADD;
			dk->dk_kevent.fflags = fflags;
			_dispatch_kevent_resume(dk, 0, del_flags);
		}
	}

	(void)dispatch_atomic_and2o(ds, ds_atomic_flags, ~DSF_ARMED);
	ds->ds_needs_rearm = false; // re-arm is pointless and bad now
	_dispatch_release(ds); // the retain is done at creation time
}

#pragma mark -
#pragma mark dispatch_timer

DISPATCH_CACHELINE_ALIGN
static struct dispatch_kevent_s _dispatch_kevent_timer[] = {
	[DISPATCH_TIMER_INDEX_WALL] = {
		.dk_kevent = {
			.ident = DISPATCH_TIMER_INDEX_WALL,
			.filter = DISPATCH_EVFILT_TIMER,
			.udata = &_dispatch_kevent_timer[DISPATCH_TIMER_INDEX_WALL],
		},
		.dk_sources = TAILQ_HEAD_INITIALIZER(
				_dispatch_kevent_timer[DISPATCH_TIMER_INDEX_WALL].dk_sources),
	},
	[DISPATCH_TIMER_INDEX_MACH] = {
		.dk_kevent = {
			.ident = DISPATCH_TIMER_INDEX_MACH,
			.filter = DISPATCH_EVFILT_TIMER,
			.udata = &_dispatch_kevent_timer[DISPATCH_TIMER_INDEX_MACH],
		},
		.dk_sources = TAILQ_HEAD_INITIALIZER(
				_dispatch_kevent_timer[DISPATCH_TIMER_INDEX_MACH].dk_sources),
	},
	[DISPATCH_TIMER_INDEX_DISARM] = {
		.dk_kevent = {
			.ident = DISPATCH_TIMER_INDEX_DISARM,
			.filter = DISPATCH_EVFILT_TIMER,
			.udata = &_dispatch_kevent_timer[DISPATCH_TIMER_INDEX_DISARM],
		},
		.dk_sources = TAILQ_HEAD_INITIALIZER(
				_dispatch_kevent_timer[DISPATCH_TIMER_INDEX_DISARM].dk_sources),
	},
};
// Don't count disarmed timer list
#define DISPATCH_TIMER_COUNT ((sizeof(_dispatch_kevent_timer) \
		/ sizeof(_dispatch_kevent_timer[0])) - 1)

static inline void
_dispatch_source_timer_init(void)
{
	TAILQ_INSERT_TAIL(&_dispatch_sources[DSL_HASH(DISPATCH_TIMER_INDEX_WALL)],
			&_dispatch_kevent_timer[DISPATCH_TIMER_INDEX_WALL], dk_list);
	TAILQ_INSERT_TAIL(&_dispatch_sources[DSL_HASH(DISPATCH_TIMER_INDEX_MACH)],
			&_dispatch_kevent_timer[DISPATCH_TIMER_INDEX_MACH], dk_list);
	TAILQ_INSERT_TAIL(&_dispatch_sources[DSL_HASH(DISPATCH_TIMER_INDEX_DISARM)],
			&_dispatch_kevent_timer[DISPATCH_TIMER_INDEX_DISARM], dk_list);
}

DISPATCH_ALWAYS_INLINE
static inline unsigned int
_dispatch_source_timer_idx(dispatch_source_refs_t dr)
{
	return ds_timer(dr).flags & DISPATCH_TIMER_WALL_CLOCK ?
		DISPATCH_TIMER_INDEX_WALL : DISPATCH_TIMER_INDEX_MACH;
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dispatch_source_timer_now2(unsigned int timer)
{
	switch (timer) {
	case DISPATCH_TIMER_INDEX_MACH:
		return _dispatch_absolute_time();
	case DISPATCH_TIMER_INDEX_WALL:
		return _dispatch_get_nanoseconds();
	default:
		DISPATCH_CRASH("Invalid timer");
	}
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dispatch_source_timer_now(dispatch_source_refs_t dr)
{
	return _dispatch_source_timer_now2(_dispatch_source_timer_idx(dr));
}

// Updates the ordered list of timers based on next fire date for changes to ds.
// Should only be called from the context of _dispatch_mgr_q.
static void
_dispatch_timer_list_update(dispatch_source_t ds)
{
	dispatch_source_refs_t dr = ds->ds_refs, dri = NULL;

	dispatch_assert(_dispatch_queue_get_current() == &_dispatch_mgr_q);

	// do not reschedule timers unregistered with _dispatch_kevent_unregister()
	if (!ds->ds_dkev) {
		return;
	}

	// Ensure the source is on the global kevent lists before it is removed and
	// readded below.
	_dispatch_kevent_register(ds);

	TAILQ_REMOVE(&ds->ds_dkev->dk_sources, dr, dr_list);

	// Move timers that are disabled, suspended or have missed intervals to the
	// disarmed list, rearm after resume resp. source invoke will reenable them
	if (!ds_timer(dr).target || DISPATCH_OBJECT_SUSPENDED(ds) ||
			ds->ds_pending_data) {
		(void)dispatch_atomic_and2o(ds, ds_atomic_flags, ~DSF_ARMED);
		ds->ds_dkev = &_dispatch_kevent_timer[DISPATCH_TIMER_INDEX_DISARM];
		TAILQ_INSERT_TAIL(&ds->ds_dkev->dk_sources, (dispatch_source_refs_t)dr,
				dr_list);
		return;
	}

	// change the list if the clock type has changed
	ds->ds_dkev = &_dispatch_kevent_timer[_dispatch_source_timer_idx(dr)];

	TAILQ_FOREACH(dri, &ds->ds_dkev->dk_sources, dr_list) {
		if (ds_timer(dri).target == 0 ||
				ds_timer(dr).target < ds_timer(dri).target) {
			break;
		}
	}

	if (dri) {
		TAILQ_INSERT_BEFORE(dri, dr, dr_list);
	} else {
		TAILQ_INSERT_TAIL(&ds->ds_dkev->dk_sources, dr, dr_list);
	}
}

static inline void
_dispatch_run_timers2(unsigned int timer)
{
	dispatch_source_refs_t dr;
	dispatch_source_t ds;
	uint64_t now, missed;

	now = _dispatch_source_timer_now2(timer);
	while ((dr = TAILQ_FIRST(&_dispatch_kevent_timer[timer].dk_sources))) {
		ds = _dispatch_source_from_refs(dr);
		// We may find timers on the wrong list due to a pending update from
		// dispatch_source_set_timer. Force an update of the list in that case.
		if (timer != ds->ds_ident_hack) {
			_dispatch_timer_list_update(ds);
			continue;
		}
		if (!ds_timer(dr).target) {
			// no configured timers on the list
			break;
		}
		if (ds_timer(dr).target > now) {
			// Done running timers for now.
			break;
		}
		// Remove timers that are suspended or have missed intervals from the
		// list, rearm after resume resp. source invoke will reenable them
		if (DISPATCH_OBJECT_SUSPENDED(ds) || ds->ds_pending_data) {
			_dispatch_timer_list_update(ds);
			continue;
		}
		// Calculate number of missed intervals.
		missed = (now - ds_timer(dr).target) / ds_timer(dr).interval;
		if (++missed > INT_MAX) {
			missed = INT_MAX;
		}
		ds_timer(dr).target += missed * ds_timer(dr).interval;
		_dispatch_timer_list_update(ds);
		ds_timer(dr).last_fire = now;
		(void)dispatch_atomic_add2o(ds, ds_pending_data, (int)missed);
		_dispatch_wakeup(ds);
	}
}

void
_dispatch_run_timers(void)
{
	dispatch_once_f(&__dispatch_kevent_init_pred,
			NULL, _dispatch_kevent_init);

	unsigned int i;
	for (i = 0; i < DISPATCH_TIMER_COUNT; i++) {
		if (!TAILQ_EMPTY(&_dispatch_kevent_timer[i].dk_sources)) {
			_dispatch_run_timers2(i);
		}
	}
}

static inline unsigned long
_dispatch_source_timer_data(dispatch_source_refs_t dr, unsigned long prev)
{
	// calculate the number of intervals since last fire
	unsigned long data, missed;
	uint64_t now = _dispatch_source_timer_now(dr);
	missed = (unsigned long)((now - ds_timer(dr).last_fire) /
			ds_timer(dr).interval);
	// correct for missed intervals already delivered last time
	data = prev - ds_timer(dr).missed + missed;
	ds_timer(dr).missed = missed;
	return data;
}

// approx 1 year (60s * 60m * 24h * 365d)
#define FOREVER_NSEC 31536000000000000ull

struct timespec *
_dispatch_get_next_timer_fire(struct timespec *howsoon)
{
	// <rdar://problem/6459649>
	// kevent(2) does not allow large timeouts, so we use a long timeout
	// instead (approximately 1 year).
	dispatch_source_refs_t dr = NULL;
	unsigned int timer;
	uint64_t now, delta_tmp, delta = UINT64_MAX;

	for (timer = 0; timer < DISPATCH_TIMER_COUNT; timer++) {
		// Timers are kept in order, first one will fire next
		dr = TAILQ_FIRST(&_dispatch_kevent_timer[timer].dk_sources);
		if (!dr || !ds_timer(dr).target) {
			// Empty list or disabled timer
			continue;
		}
		now = _dispatch_source_timer_now(dr);
		if (ds_timer(dr).target <= now) {
			howsoon->tv_sec = 0;
			howsoon->tv_nsec = 0;
			return howsoon;
		}
		// the subtraction cannot go negative because the previous "if"
		// verified that the target is greater than now.
		delta_tmp = ds_timer(dr).target - now;
		if (!(ds_timer(dr).flags & DISPATCH_TIMER_WALL_CLOCK)) {
			delta_tmp = _dispatch_time_mach2nano(delta_tmp);
		}
		if (delta_tmp < delta) {
			delta = delta_tmp;
		}
	}
	if (slowpath(delta > FOREVER_NSEC)) {
		return NULL;
	} else {
		howsoon->tv_sec = (time_t)(delta / NSEC_PER_SEC);
		howsoon->tv_nsec = (long)(delta % NSEC_PER_SEC);
	}
	return howsoon;
}

struct dispatch_set_timer_params {
	dispatch_source_t ds;
	uintptr_t ident;
	struct dispatch_timer_source_s values;
};

static void
_dispatch_source_set_timer3(void *context)
{
	// Called on the _dispatch_mgr_q
	struct dispatch_set_timer_params *params = context;
	dispatch_source_t ds = params->ds;
	ds->ds_ident_hack = params->ident;
	ds_timer(ds->ds_refs) = params->values;
	// Clear any pending data that might have accumulated on
	// older timer params <rdar://problem/8574886>
	ds->ds_pending_data = 0;
	_dispatch_timer_list_update(ds);
	dispatch_resume(ds);
	dispatch_release(ds);
	free(params);
}

static void
_dispatch_source_set_timer2(void *context)
{
	// Called on the source queue
	struct dispatch_set_timer_params *params = context;
	dispatch_suspend(params->ds);
	dispatch_barrier_async_f(&_dispatch_mgr_q, params,
			_dispatch_source_set_timer3);
}

void
dispatch_source_set_timer(dispatch_source_t ds,
	dispatch_time_t start,
	uint64_t interval,
	uint64_t leeway)
{
	if (slowpath(!ds->ds_is_timer)) {
		DISPATCH_CLIENT_CRASH("Attempt to set timer on a non-timer source");
	}

	struct dispatch_set_timer_params *params;

	// we use zero internally to mean disabled
	if (interval == 0) {
		interval = 1;
	} else if ((int64_t)interval < 0) {
		// 6866347 - make sure nanoseconds won't overflow
		interval = INT64_MAX;
	}
	if ((int64_t)leeway < 0) {
		leeway = INT64_MAX;
	}

	if (start == DISPATCH_TIME_NOW) {
		start = _dispatch_absolute_time();
	} else if (start == DISPATCH_TIME_FOREVER) {
		start = INT64_MAX;
	}

	while (!(params = calloc(1ul, sizeof(struct dispatch_set_timer_params)))) {
		sleep(1);
	}

	params->ds = ds;
	params->values.flags = ds_timer(ds->ds_refs).flags;

	if ((int64_t)start < 0) {
		// wall clock
		params->ident = DISPATCH_TIMER_INDEX_WALL;
		params->values.target = -((int64_t)start);
		params->values.interval = interval;
		params->values.leeway = leeway;
		params->values.flags |= DISPATCH_TIMER_WALL_CLOCK;
	} else {
		// absolute clock
		params->ident = DISPATCH_TIMER_INDEX_MACH;
		params->values.target = start;
		params->values.interval = _dispatch_time_nano2mach(interval);

		// rdar://problem/7287561 interval must be at least one in
		// in order to avoid later division by zero when calculating
		// the missed interval count. (NOTE: the wall clock's
		// interval is already "fixed" to be 1 or more)
		if (params->values.interval < 1) {
			params->values.interval = 1;
		}

		params->values.leeway = _dispatch_time_nano2mach(leeway);
		params->values.flags &= ~DISPATCH_TIMER_WALL_CLOCK;
	}
	// Suspend the source so that it doesn't fire with pending changes
	// The use of suspend/resume requires the external retain/release
	dispatch_retain(ds);
	dispatch_barrier_async_f((dispatch_queue_t)ds, params,
			_dispatch_source_set_timer2);
}

#pragma mark -
#pragma mark dispatch_mach

#if HAVE_MACH

#if DISPATCH_DEBUG && DISPATCH_MACHPORT_DEBUG
#define _dispatch_debug_machport(name) \
		dispatch_debug_machport((name), __func__)
#else
#define _dispatch_debug_machport(name)
#endif

// Flags for all notifications that are registered/unregistered when a
// send-possible notification is requested/delivered
#define _DISPATCH_MACH_SP_FLAGS (DISPATCH_MACH_SEND_POSSIBLE| \
		DISPATCH_MACH_SEND_DEAD|DISPATCH_MACH_SEND_DELETED)

#define _DISPATCH_IS_POWER_OF_TWO(v) (!(v & (v - 1)) && v)
#define _DISPATCH_HASH(x, y) (_DISPATCH_IS_POWER_OF_TWO(y) ? \
		(MACH_PORT_INDEX(x) & ((y) - 1)) : (MACH_PORT_INDEX(x) % (y)))

#define _DISPATCH_MACHPORT_HASH_SIZE 32
#define _DISPATCH_MACHPORT_HASH(x) \
		_DISPATCH_HASH((x), _DISPATCH_MACHPORT_HASH_SIZE)

static dispatch_source_t _dispatch_mach_notify_source;
static mach_port_t _dispatch_port_set;
static mach_port_t _dispatch_event_port;

static kern_return_t _dispatch_mach_notify_update(dispatch_kevent_t dk,
	uint32_t new_flags, uint32_t del_flags, uint32_t mask,
	mach_msg_id_t notify_msgid, mach_port_mscount_t notify_sync);

static void
_dispatch_port_set_init(void *context DISPATCH_UNUSED)
{
	struct kevent kev = {
		.filter = EVFILT_MACHPORT,
		.flags = EV_ADD,
	};
	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET,
			&_dispatch_port_set);
	DISPATCH_VERIFY_MIG(kr);
	if (kr) {
		_dispatch_bug_mach_client(
				"_dispatch_port_set_init: mach_port_allocate() failed", kr);
		DISPATCH_CLIENT_CRASH(
				"mach_port_allocate() failed: cannot create port set");
	}
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			&_dispatch_event_port);
	DISPATCH_VERIFY_MIG(kr);
	if (kr) {
		_dispatch_bug_mach_client(
				"_dispatch_port_set_init: mach_port_allocate() failed", kr);
		DISPATCH_CLIENT_CRASH(
				"mach_port_allocate() failed: cannot create receive right");
	}
	kr = mach_port_move_member(mach_task_self(), _dispatch_event_port,
			_dispatch_port_set);
	DISPATCH_VERIFY_MIG(kr);
	if (kr) {
		_dispatch_bug_mach_client(
				"_dispatch_port_set_init: mach_port_move_member() failed", kr);
		DISPATCH_CLIENT_CRASH("mach_port_move_member() failed");
	}

	kev.ident = _dispatch_port_set;

	_dispatch_update_kq(&kev);
}

static mach_port_t
_dispatch_get_port_set(void)
{
	static dispatch_once_t pred;

	dispatch_once_f(&pred, NULL, _dispatch_port_set_init);

	return _dispatch_port_set;
}

static kern_return_t
_dispatch_kevent_machport_enable(dispatch_kevent_t dk)
{
	mach_port_t mp = (mach_port_t)dk->dk_kevent.ident;
	kern_return_t kr;

	_dispatch_debug_machport(mp);
	kr = mach_port_move_member(mach_task_self(), mp, _dispatch_get_port_set());
	if (slowpath(kr)) {
		DISPATCH_VERIFY_MIG(kr);
		switch (kr) {
		case KERN_INVALID_NAME:
#if DISPATCH_DEBUG
			_dispatch_log("Corruption: Mach receive right 0x%x destroyed "
					"prematurely", mp);
#endif
			break;
		case KERN_INVALID_RIGHT:
			_dispatch_bug_mach_client("_dispatch_kevent_machport_enable: "
					"mach_port_move_member() failed ", kr);
			break;
		default:
			(void)dispatch_assume_zero(kr);
			break;
		}
	}
	return kr;
}

static void
_dispatch_kevent_machport_disable(dispatch_kevent_t dk)
{
	mach_port_t mp = (mach_port_t)dk->dk_kevent.ident;
	kern_return_t kr;

	_dispatch_debug_machport(mp);
	kr = mach_port_move_member(mach_task_self(), mp, 0);
	if (slowpath(kr)) {
		DISPATCH_VERIFY_MIG(kr);
		switch (kr) {
		case KERN_INVALID_RIGHT:
		case KERN_INVALID_NAME:
#if DISPATCH_DEBUG
			_dispatch_log("Corruption: Mach receive right 0x%x destroyed "
					"prematurely", mp);
#endif
			break;
		default:
			(void)dispatch_assume_zero(kr);
			break;
		}
	}
}

kern_return_t
_dispatch_kevent_machport_resume(dispatch_kevent_t dk, uint32_t new_flags,
		uint32_t del_flags)
{
	kern_return_t kr_recv = 0, kr_sp = 0;

	dispatch_assert_zero(new_flags & del_flags);
	if (new_flags & DISPATCH_MACH_RECV_MESSAGE) {
		kr_recv = _dispatch_kevent_machport_enable(dk);
	} else if (del_flags & DISPATCH_MACH_RECV_MESSAGE) {
		_dispatch_kevent_machport_disable(dk);
	}
	if ((new_flags & _DISPATCH_MACH_SP_FLAGS) ||
			(del_flags & _DISPATCH_MACH_SP_FLAGS)) {
		// Requesting a (delayed) non-sync send-possible notification
		// registers for both immediate dead-name notification and delayed-arm
		// send-possible notification for the port.
		// The send-possible notification is armed when a mach_msg() with the
		// the MACH_SEND_NOTIFY to the port times out.
		// If send-possible is unavailable, fall back to immediate dead-name
		// registration rdar://problem/2527840&9008724
		kr_sp = _dispatch_mach_notify_update(dk, new_flags, del_flags,
				_DISPATCH_MACH_SP_FLAGS, MACH_NOTIFY_SEND_POSSIBLE,
				MACH_NOTIFY_SEND_POSSIBLE == MACH_NOTIFY_DEAD_NAME ? 1 : 0);
	}

	return (kr_recv ? kr_recv : kr_sp);
}

void
_dispatch_drain_mach_messages(struct kevent *ke)
{
	mach_port_t name = (mach_port_name_t)ke->data;
	dispatch_source_refs_t dri;
	dispatch_kevent_t dk;
	struct kevent kev;

	if (!dispatch_assume(name)) {
		return;
	}
	_dispatch_debug_machport(name);
	dk = _dispatch_kevent_find(name, EVFILT_MACHPORT);
	if (!dispatch_assume(dk)) {
		return;
	}
	_dispatch_kevent_machport_disable(dk); // emulate EV_DISPATCH

	EV_SET(&kev, name, EVFILT_MACHPORT, EV_ADD|EV_ENABLE|EV_DISPATCH,
			DISPATCH_MACH_RECV_MESSAGE, 0, dk);

	TAILQ_FOREACH(dri, &dk->dk_sources, dr_list) {
		_dispatch_source_merge_kevent(_dispatch_source_from_refs(dri), &kev);
	}
}

static inline void
_dispatch_mach_notify_merge(mach_port_t name, uint32_t flag, uint32_t unreg,
		bool final)
{
	dispatch_source_refs_t dri;
	dispatch_kevent_t dk;
	struct kevent kev;

	dk = _dispatch_kevent_find(name, EVFILT_MACHPORT);
	if (!dk) {
		return;
	}

	// Update notification registration state.
	dk->dk_kevent.data &= ~unreg;
	if (!final) {
		// Re-register for notification before delivery
		_dispatch_kevent_resume(dk, flag, 0);
	}

	EV_SET(&kev, name, EVFILT_MACHPORT, EV_ADD|EV_ENABLE, flag, 0, dk);

	TAILQ_FOREACH(dri, &dk->dk_sources, dr_list) {
		_dispatch_source_merge_kevent(_dispatch_source_from_refs(dri), &kev);
		if (final) {
			// this can never happen again
			// this must happen after the merge
			// this may be racy in the future, but we don't provide a 'setter'
			// API for the mask yet
			_dispatch_source_from_refs(dri)->ds_pending_data_mask &= ~unreg;
		}
	}

	if (final) {
		// no more sources have these flags
		dk->dk_kevent.fflags &= ~unreg;
	}
}

static kern_return_t
_dispatch_mach_notify_update(dispatch_kevent_t dk, uint32_t new_flags,
		uint32_t del_flags, uint32_t mask, mach_msg_id_t notify_msgid,
		mach_port_mscount_t notify_sync)
{
	mach_port_t previous, port = (mach_port_t)dk->dk_kevent.ident;
	typeof(dk->dk_kevent.data) prev = dk->dk_kevent.data;
	kern_return_t kr, krr = 0;

	// Update notification registration state.
	dk->dk_kevent.data |= (new_flags | dk->dk_kevent.fflags) & mask;
	dk->dk_kevent.data &= ~(del_flags & mask);

	_dispatch_debug_machport(port);
	if ((dk->dk_kevent.data & mask) && !(prev & mask)) {
		previous = MACH_PORT_NULL;
		krr = mach_port_request_notification(mach_task_self(), port,
				notify_msgid, notify_sync, _dispatch_event_port,
				MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
		DISPATCH_VERIFY_MIG(krr);

		switch(krr) {
		case KERN_INVALID_NAME:
		case KERN_INVALID_RIGHT:
			// Supress errors & clear registration state
			dk->dk_kevent.data &= ~mask;
			break;
		default:
			// Else, we dont expect any errors from mach. Log any errors
			if (dispatch_assume_zero(krr)) {
				// log the error & clear registration state
				dk->dk_kevent.data &= ~mask;
			} else if (dispatch_assume_zero(previous)) {
				// Another subsystem has beat libdispatch to requesting the
				// specified Mach notification on this port. We should
				// technically cache the previous port and message it when the
				// kernel messages our port. Or we can just say screw those
				// subsystems and deallocate the previous port.
				// They should adopt libdispatch :-P
				kr = mach_port_deallocate(mach_task_self(), previous);
				DISPATCH_VERIFY_MIG(kr);
				(void)dispatch_assume_zero(kr);
				previous = MACH_PORT_NULL;
			}
		}
	} else if (!(dk->dk_kevent.data & mask) && (prev & mask)) {
		previous = MACH_PORT_NULL;
		kr = mach_port_request_notification(mach_task_self(), port,
				notify_msgid, notify_sync, MACH_PORT_NULL,
				MACH_MSG_TYPE_MOVE_SEND_ONCE, &previous);
		DISPATCH_VERIFY_MIG(kr);

		switch (kr) {
		case KERN_INVALID_NAME:
		case KERN_INVALID_RIGHT:
		case KERN_INVALID_ARGUMENT:
			break;
		default:
			if (dispatch_assume_zero(kr)) {
				// log the error
			}
		}
	} else {
		return 0;
	}
	if (slowpath(previous)) {
		// the kernel has not consumed the send-once right yet
		(void)dispatch_assume_zero(
				_dispatch_send_consume_send_once_right(previous));
	}
	return krr;
}

static void
_dispatch_mach_notify_source2(void *context)
{
	dispatch_source_t ds = context;
	size_t maxsz = MAX(sizeof(union
		__RequestUnion___dispatch_send_libdispatch_internal_protocol_subsystem),
		sizeof(union
		__ReplyUnion___dispatch_libdispatch_internal_protocol_subsystem));

	dispatch_mig_server(ds, maxsz, libdispatch_internal_protocol_server);
}

void
_dispatch_mach_notify_source_init(void *context DISPATCH_UNUSED)
{
	_dispatch_get_port_set();

	_dispatch_mach_notify_source = dispatch_source_create(
			DISPATCH_SOURCE_TYPE_MACH_RECV, _dispatch_event_port, 0,
			&_dispatch_mgr_q);
	dispatch_assert(_dispatch_mach_notify_source);
	dispatch_set_context(_dispatch_mach_notify_source,
			_dispatch_mach_notify_source);
	dispatch_source_set_event_handler_f(_dispatch_mach_notify_source,
			_dispatch_mach_notify_source2);
	dispatch_resume(_dispatch_mach_notify_source);
}

kern_return_t
_dispatch_mach_notify_port_deleted(mach_port_t notify DISPATCH_UNUSED,
		mach_port_name_t name)
{
#if DISPATCH_DEBUG
	_dispatch_log("Corruption: Mach send/send-once/dead-name right 0x%x "
			"deleted prematurely", name);
#endif

	_dispatch_debug_machport(name);
	_dispatch_mach_notify_merge(name, DISPATCH_MACH_SEND_DELETED,
				_DISPATCH_MACH_SP_FLAGS, true);

	return KERN_SUCCESS;
}

kern_return_t
_dispatch_mach_notify_dead_name(mach_port_t notify DISPATCH_UNUSED,
		mach_port_name_t name)
{
	kern_return_t kr;

#if DISPATCH_DEBUG
	_dispatch_log("machport[0x%08x]: dead-name notification: %s",
			name, __func__);
#endif
	_dispatch_debug_machport(name);
	_dispatch_mach_notify_merge(name, DISPATCH_MACH_SEND_DEAD,
				_DISPATCH_MACH_SP_FLAGS, true);

	// the act of receiving a dead name notification allocates a dead-name
	// right that must be deallocated
	kr = mach_port_deallocate(mach_task_self(), name);
	DISPATCH_VERIFY_MIG(kr);
	//(void)dispatch_assume_zero(kr);

	return KERN_SUCCESS;
}

kern_return_t
_dispatch_mach_notify_send_possible(mach_port_t notify DISPATCH_UNUSED,
		mach_port_name_t name)
{
#if DISPATCH_DEBUG
	_dispatch_log("machport[0x%08x]: send-possible notification: %s",
			name, __func__);
#endif
	_dispatch_debug_machport(name);
	_dispatch_mach_notify_merge(name, DISPATCH_MACH_SEND_POSSIBLE,
				_DISPATCH_MACH_SP_FLAGS, false);

	return KERN_SUCCESS;
}

mach_msg_return_t
dispatch_mig_server(dispatch_source_t ds, size_t maxmsgsz,
		dispatch_mig_callback_t callback)
{
	mach_msg_options_t options = MACH_RCV_MSG | MACH_RCV_TIMEOUT
		| MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_CTX)
		| MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0);
	mach_msg_options_t tmp_options;
	mig_reply_error_t *bufTemp, *bufRequest, *bufReply;
	mach_msg_return_t kr = 0;
	unsigned int cnt = 1000; // do not stall out serial queues
	int demux_success;
	bool received = false;
	size_t rcv_size = maxmsgsz + MAX_TRAILER_SIZE;

	// XXX FIXME -- allocate these elsewhere
	bufRequest = alloca(rcv_size);
	bufReply = alloca(rcv_size);
	bufReply->Head.msgh_size = 0; // make CLANG happy
	bufRequest->RetCode = 0;

#if DISPATCH_DEBUG
	options |= MACH_RCV_LARGE; // rdar://problem/8422992
#endif
	tmp_options = options;
	// XXX FIXME -- change this to not starve out the target queue
	for (;;) {
		if (DISPATCH_OBJECT_SUSPENDED(ds) || (--cnt == 0)) {
			options &= ~MACH_RCV_MSG;
			tmp_options &= ~MACH_RCV_MSG;

			if (!(tmp_options & MACH_SEND_MSG)) {
				break;
			}
		}
		kr = mach_msg(&bufReply->Head, tmp_options, bufReply->Head.msgh_size,
				(mach_msg_size_t)rcv_size, (mach_port_t)ds->ds_ident_hack, 0,0);

		tmp_options = options;

		if (slowpath(kr)) {
			switch (kr) {
			case MACH_SEND_INVALID_DEST:
			case MACH_SEND_TIMED_OUT:
				if (bufReply->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) {
					mach_msg_destroy(&bufReply->Head);
				}
				break;
			case MACH_RCV_TIMED_OUT:
				// Don't return an error if a message was sent this time or
				// a message was successfully received previously
				// rdar://problems/7363620&7791738
				if(bufReply->Head.msgh_remote_port || received) {
					kr = MACH_MSG_SUCCESS;
				}
				break;
			case MACH_RCV_INVALID_NAME:
				break;
#if DISPATCH_DEBUG
			case MACH_RCV_TOO_LARGE:
				// receive messages that are too large and log their id and size
				// rdar://problem/8422992
				tmp_options &= ~MACH_RCV_LARGE;
				size_t large_size = bufReply->Head.msgh_size + MAX_TRAILER_SIZE;
				void *large_buf = malloc(large_size);
				if (large_buf) {
					rcv_size = large_size;
					bufReply = large_buf;
				}
				if (!mach_msg(&bufReply->Head, tmp_options, 0,
						(mach_msg_size_t)rcv_size,
						(mach_port_t)ds->ds_ident_hack, 0, 0)) {
					_dispatch_log("BUG in libdispatch client: "
							"dispatch_mig_server received message larger than "
							"requested size %zd: id = 0x%x, size = %d",
							maxmsgsz, bufReply->Head.msgh_id,
							bufReply->Head.msgh_size);
				}
				if (large_buf) {
					free(large_buf);
				}
				// fall through
#endif
			default:
				_dispatch_bug_mach_client(
						"dispatch_mig_server: mach_msg() failed", kr);
				break;
			}
			break;
		}

		if (!(tmp_options & MACH_RCV_MSG)) {
			break;
		}
		received = true;

		bufTemp = bufRequest;
		bufRequest = bufReply;
		bufReply = bufTemp;

		demux_success = callback(&bufRequest->Head, &bufReply->Head);

		if (!demux_success) {
			// destroy the request - but not the reply port
			bufRequest->Head.msgh_remote_port = 0;
			mach_msg_destroy(&bufRequest->Head);
		} else if (!(bufReply->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
			// if MACH_MSGH_BITS_COMPLEX is _not_ set, then bufReply->RetCode
			// is present
			if (slowpath(bufReply->RetCode)) {
				if (bufReply->RetCode == MIG_NO_REPLY) {
					continue;
				}

				// destroy the request - but not the reply port
				bufRequest->Head.msgh_remote_port = 0;
				mach_msg_destroy(&bufRequest->Head);
			}
		}

		if (bufReply->Head.msgh_remote_port) {
			tmp_options |= MACH_SEND_MSG;
			if (MACH_MSGH_BITS_REMOTE(bufReply->Head.msgh_bits) !=
					MACH_MSG_TYPE_MOVE_SEND_ONCE) {
				tmp_options |= MACH_SEND_TIMEOUT;
			}
		}
	}

	return kr;
}

#endif /* HAVE_MACH */

#pragma mark -
#pragma mark dispatch_source_debug

DISPATCH_NOINLINE
static const char *
_evfiltstr(short filt)
{
	switch (filt) {
#define _evfilt2(f) case (f): return #f
	_evfilt2(EVFILT_READ);
	_evfilt2(EVFILT_WRITE);
	_evfilt2(EVFILT_AIO);
	_evfilt2(EVFILT_VNODE);
	_evfilt2(EVFILT_PROC);
	_evfilt2(EVFILT_SIGNAL);
	_evfilt2(EVFILT_TIMER);
#ifdef EVFILT_VM
	_evfilt2(EVFILT_VM);
#endif
#if HAVE_MACH
	_evfilt2(EVFILT_MACHPORT);
#endif
	_evfilt2(EVFILT_FS);
	_evfilt2(EVFILT_USER);

	_evfilt2(DISPATCH_EVFILT_TIMER);
	_evfilt2(DISPATCH_EVFILT_CUSTOM_ADD);
	_evfilt2(DISPATCH_EVFILT_CUSTOM_OR);
	default:
		return "EVFILT_missing";
	}
}

static size_t
_dispatch_source_debug_attr(dispatch_source_t ds, char* buf, size_t bufsiz)
{
	dispatch_queue_t target = ds->do_targetq;
	return snprintf(buf, bufsiz, "target = %s[%p], pending_data = 0x%lx, "
			"pending_data_mask = 0x%lx, ",
			target ? target->dq_label : "", target,
			ds->ds_pending_data, ds->ds_pending_data_mask);
}

static size_t
_dispatch_timer_debug_attr(dispatch_source_t ds, char* buf, size_t bufsiz)
{
	dispatch_source_refs_t dr = ds->ds_refs;
	return snprintf(buf, bufsiz, "timer = { target = 0x%llx, "
			"last_fire = 0x%llx, interval = 0x%llx, flags = 0x%llx }, ",
			ds_timer(dr).target, ds_timer(dr).last_fire, ds_timer(dr).interval,
			ds_timer(dr).flags);
}

static size_t
_dispatch_source_debug(dispatch_source_t ds, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	offset += snprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			dx_kind(ds), ds);
	offset += _dispatch_object_debug_attr(ds, &buf[offset], bufsiz - offset);
	offset += _dispatch_source_debug_attr(ds, &buf[offset], bufsiz - offset);
	if (ds->ds_is_timer) {
		offset += _dispatch_timer_debug_attr(ds, &buf[offset], bufsiz - offset);
	}
	return offset;
}

static size_t
_dispatch_source_kevent_debug(dispatch_source_t ds, char* buf, size_t bufsiz)
{
	size_t offset = _dispatch_source_debug(ds, buf, bufsiz);
	offset += snprintf(&buf[offset], bufsiz - offset, "filter = %s }",
			ds->ds_dkev ? _evfiltstr(ds->ds_dkev->dk_kevent.filter) : "????");
	return offset;
}

#if DISPATCH_DEBUG
void
dispatch_debug_kevents(struct kevent* kev, size_t count, const char* str)
{
	size_t i;
	for (i = 0; i < count; ++i) {
		_dispatch_log("kevent[%lu] = { ident = %p, filter = %s, flags = 0x%x, "
				"fflags = 0x%x, data = %p, udata = %p }: %s",
				i, (void*)kev[i].ident, _evfiltstr(kev[i].filter), kev[i].flags,
				kev[i].fflags, (void*)kev[i].data, (void*)kev[i].udata, str);
	}
}

static void
_dispatch_kevent_debugger2(void *context)
{
	struct sockaddr sa;
	socklen_t sa_len = sizeof(sa);
	int c, fd = (int)(long)context;
	unsigned int i;
	dispatch_kevent_t dk;
	dispatch_source_t ds;
	dispatch_source_refs_t dr;
	FILE *debug_stream;

	c = accept(fd, &sa, &sa_len);
	if (c == -1) {
		if (errno != EAGAIN) {
			(void)dispatch_assume_zero(errno);
		}
		return;
	}
#if 0
	int r = fcntl(c, F_SETFL, 0); // disable non-blocking IO
	if (r == -1) {
		(void)dispatch_assume_zero(errno);
	}
#endif
	debug_stream = fdopen(c, "a");
	if (!dispatch_assume(debug_stream)) {
		close(c);
		return;
	}

	fprintf(debug_stream, "HTTP/1.0 200 OK\r\n");
	fprintf(debug_stream, "Content-type: text/html\r\n");
	fprintf(debug_stream, "Pragma: nocache\r\n");
	fprintf(debug_stream, "\r\n");
	fprintf(debug_stream, "<html>\n");
	fprintf(debug_stream, "<head><title>PID %u</title></head>\n", getpid());
	fprintf(debug_stream, "<body>\n<ul>\n");

	//fprintf(debug_stream, "<tr><td>DK</td><td>DK</td><td>DK</td><td>DK</td>"
	//		"<td>DK</td><td>DK</td><td>DK</td></tr>\n");

	for (i = 0; i < DSL_HASH_SIZE; i++) {
		if (TAILQ_EMPTY(&_dispatch_sources[i])) {
			continue;
		}
		TAILQ_FOREACH(dk, &_dispatch_sources[i], dk_list) {
			fprintf(debug_stream, "\t<br><li>DK %p ident %lu filter %s flags "
					"0x%hx fflags 0x%x data 0x%lx udata %p\n",
					dk, (unsigned long)dk->dk_kevent.ident,
					_evfiltstr(dk->dk_kevent.filter), dk->dk_kevent.flags,
					dk->dk_kevent.fflags, (unsigned long)dk->dk_kevent.data,
					dk->dk_kevent.udata);
			fprintf(debug_stream, "\t\t<ul>\n");
			TAILQ_FOREACH(dr, &dk->dk_sources, dr_list) {
				ds = _dispatch_source_from_refs(dr);
				fprintf(debug_stream, "\t\t\t<li>DS %p refcnt 0x%x suspend "
						"0x%x data 0x%lx mask 0x%lx flags 0x%x</li>\n",
						ds, ds->do_ref_cnt, ds->do_suspend_cnt,
						ds->ds_pending_data, ds->ds_pending_data_mask,
						ds->ds_atomic_flags);
				if (ds->do_suspend_cnt == DISPATCH_OBJECT_SUSPEND_LOCK) {
					dispatch_queue_t dq = ds->do_targetq;
					fprintf(debug_stream, "\t\t<br>DQ: %p refcnt 0x%x suspend "
							"0x%x label: %s\n", dq, dq->do_ref_cnt,
							dq->do_suspend_cnt, dq->dq_label);
				}
			}
			fprintf(debug_stream, "\t\t</ul>\n");
			fprintf(debug_stream, "\t</li>\n");
		}
	}
	fprintf(debug_stream, "</ul>\n</body>\n</html>\n");
	fflush(debug_stream);
	fclose(debug_stream);
}

static void
_dispatch_kevent_debugger2_cancel(void *context)
{
	int ret, fd = (int)(long)context;

	ret = close(fd);
	if (ret != -1) {
		(void)dispatch_assume_zero(errno);
	}
}

static void
_dispatch_kevent_debugger(void *context DISPATCH_UNUSED)
{
	union {
		struct sockaddr_in sa_in;
		struct sockaddr sa;
	} sa_u = {
		.sa_in = {
			.sin_family = AF_INET,
			.sin_addr = { htonl(INADDR_LOOPBACK), },
		},
	};
	dispatch_source_t ds;
	const char *valstr;
	int val, r, fd, sock_opt = 1;
	socklen_t slen = sizeof(sa_u);

	if (issetugid()) {
		return;
	}
	valstr = getenv("LIBDISPATCH_DEBUGGER");
	if (!valstr) {
		return;
	}
	val = atoi(valstr);
	if (val == 2) {
		sa_u.sa_in.sin_addr.s_addr = 0;
	}
	fd = socket(PF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		(void)dispatch_assume_zero(errno);
		return;
	}
	r = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&sock_opt,
			(socklen_t) sizeof sock_opt);
	if (r == -1) {
		(void)dispatch_assume_zero(errno);
		goto out_bad;
	}
#if 0
	r = fcntl(fd, F_SETFL, O_NONBLOCK);
	if (r == -1) {
		(void)dispatch_assume_zero(errno);
		goto out_bad;
	}
#endif
	r = bind(fd, &sa_u.sa, sizeof(sa_u));
	if (r == -1) {
		(void)dispatch_assume_zero(errno);
		goto out_bad;
	}
	r = listen(fd, SOMAXCONN);
	if (r == -1) {
		(void)dispatch_assume_zero(errno);
		goto out_bad;
	}
	r = getsockname(fd, &sa_u.sa, &slen);
	if (r == -1) {
		(void)dispatch_assume_zero(errno);
		goto out_bad;
	}

	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, 0,
			&_dispatch_mgr_q);
	if (dispatch_assume(ds)) {
		_dispatch_log("LIBDISPATCH: debug port: %hu",
				(in_port_t)ntohs(sa_u.sa_in.sin_port));

		/* ownership of fd transfers to ds */
		dispatch_set_context(ds, (void *)(long)fd);
		dispatch_source_set_event_handler_f(ds, _dispatch_kevent_debugger2);
		dispatch_source_set_cancel_handler_f(ds,
				_dispatch_kevent_debugger2_cancel);
		dispatch_resume(ds);

		return;
	}
out_bad:
	close(fd);
}

#if HAVE_MACH

#ifndef MACH_PORT_TYPE_SPREQUEST
#define MACH_PORT_TYPE_SPREQUEST 0x40000000
#endif

void
dispatch_debug_machport(mach_port_t name, const char* str)
{
	mach_port_type_t type;
	mach_msg_bits_t ns = 0, nr = 0, nso = 0, nd = 0;
	unsigned int dnreqs = 0, dnrsiz;
	kern_return_t kr = mach_port_type(mach_task_self(), name, &type);
	if (kr) {
		_dispatch_log("machport[0x%08x] = { error(0x%x) \"%s\" }: %s", name,
				kr, mach_error_string(kr), str);
		return;
	}
	if (type & MACH_PORT_TYPE_SEND) {
		(void)dispatch_assume_zero(mach_port_get_refs(mach_task_self(), name,
				MACH_PORT_RIGHT_SEND, &ns));
	}
	if (type & MACH_PORT_TYPE_SEND_ONCE) {
		(void)dispatch_assume_zero(mach_port_get_refs(mach_task_self(), name,
				MACH_PORT_RIGHT_SEND_ONCE, &nso));
	}
	if (type & MACH_PORT_TYPE_DEAD_NAME) {
		(void)dispatch_assume_zero(mach_port_get_refs(mach_task_self(), name,
				MACH_PORT_RIGHT_DEAD_NAME, &nd));
	}
	if (type & (MACH_PORT_TYPE_RECEIVE|MACH_PORT_TYPE_SEND|
			MACH_PORT_TYPE_SEND_ONCE)) {
		(void)dispatch_assume_zero(mach_port_dnrequest_info(mach_task_self(),
				name, &dnrsiz, &dnreqs));
		}
	if (type & MACH_PORT_TYPE_RECEIVE) {
		mach_port_status_t status = { .mps_pset = 0, };
		mach_msg_type_number_t cnt = MACH_PORT_RECEIVE_STATUS_COUNT;
		(void)dispatch_assume_zero(mach_port_get_refs(mach_task_self(), name,
				MACH_PORT_RIGHT_RECEIVE, &nr));
		(void)dispatch_assume_zero(mach_port_get_attributes(mach_task_self(),
				name, MACH_PORT_RECEIVE_STATUS, (void*)&status, &cnt));
		_dispatch_log("machport[0x%08x] = { R(%03u) S(%03u) SO(%03u) D(%03u) "
				"dnreqs(%03u) spreq(%s) nsreq(%s) pdreq(%s) srights(%s) "
				"sorights(%03u) qlim(%03u) msgcount(%03u) mkscount(%03u) "
				"seqno(%03u) }: %s", name, nr, ns, nso, nd, dnreqs,
				type & MACH_PORT_TYPE_SPREQUEST ? "Y":"N",
				status.mps_nsrequest ? "Y":"N", status.mps_pdrequest ? "Y":"N",
				status.mps_srights ? "Y":"N", status.mps_sorights,
				status.mps_qlimit, status.mps_msgcount, status.mps_mscount,
				status.mps_seqno, str);
	} else if (type & (MACH_PORT_TYPE_SEND|MACH_PORT_TYPE_SEND_ONCE|
			MACH_PORT_TYPE_DEAD_NAME)) {
		_dispatch_log("machport[0x%08x] = { R(%03u) S(%03u) SO(%03u) D(%03u) "
				"dnreqs(%03u) spreq(%s) }: %s", name, nr, ns, nso, nd, dnreqs,
				type & MACH_PORT_TYPE_SPREQUEST ? "Y":"N", str);
	} else {
		_dispatch_log("machport[0x%08x] = { type(0x%08x) }: %s", name, type,
				str);
	}
}

#endif // HAVE_MACH

#endif // DISPATCH_DEBUG
