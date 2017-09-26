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
#if HAVE_MACH

#define DISPATCH_MACH_RETURN_IMMEDIATE_SEND_RESULT 0x1
#define DISPATCH_MACH_REGISTER_FOR_REPLY 0x2
#define DISPATCH_MACH_WAIT_FOR_REPLY 0x4
#define DISPATCH_MACH_OWNED_REPLY_PORT 0x8
#define DISPATCH_MACH_ASYNC_REPLY 0x10
#define DISPATCH_MACH_OPTIONS_MASK 0xffff

#define DM_SEND_STATUS_SUCCESS 0x1
#define DM_SEND_STATUS_RETURNING_IMMEDIATE_SEND_RESULT 0x2

DISPATCH_ENUM(dispatch_mach_send_invoke_flags, uint32_t,
	DM_SEND_INVOKE_NONE            = 0x0,
	DM_SEND_INVOKE_MAKE_DIRTY      = 0x1,
	DM_SEND_INVOKE_NEEDS_BARRIER   = 0x2,
	DM_SEND_INVOKE_CANCEL          = 0x4,
	DM_SEND_INVOKE_CAN_RUN_BARRIER = 0x8,
	DM_SEND_INVOKE_IMMEDIATE_SEND  = 0x10,
);
#define DM_SEND_INVOKE_IMMEDIATE_SEND_MASK \
		((dispatch_mach_send_invoke_flags_t)DM_SEND_INVOKE_IMMEDIATE_SEND)

static inline mach_msg_option_t _dispatch_mach_checkin_options(void);
static mach_port_t _dispatch_mach_msg_get_remote_port(dispatch_object_t dou);
static mach_port_t _dispatch_mach_msg_get_reply_port(dispatch_object_t dou);
static void _dispatch_mach_msg_disconnected(dispatch_mach_t dm,
		mach_port_t local_port, mach_port_t remote_port);
static inline void _dispatch_mach_msg_reply_received(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, mach_port_t local_port);
static dispatch_mach_msg_t _dispatch_mach_msg_create_reply_disconnected(
		dispatch_object_t dou, dispatch_mach_reply_refs_t dmr,
		dispatch_mach_reason_t reason);
static bool _dispatch_mach_reconnect_invoke(dispatch_mach_t dm,
		dispatch_object_t dou);
static inline mach_msg_header_t* _dispatch_mach_msg_get_msg(
		dispatch_mach_msg_t dmsg);
static void _dispatch_mach_send_push(dispatch_mach_t dm, dispatch_object_t dou,
		dispatch_qos_t qos);
static void _dispatch_mach_cancel(dispatch_mach_t dm);
static void _dispatch_mach_push_send_barrier_drain(dispatch_mach_t dm,
		dispatch_qos_t qos);
static void _dispatch_mach_handle_or_push_received_msg(dispatch_mach_t dm,
		dispatch_mach_msg_t dmsg);
static void _dispatch_mach_push_async_reply_msg(dispatch_mach_t dm,
		dispatch_mach_msg_t dmsg, dispatch_queue_t drq);
static dispatch_queue_t _dispatch_mach_msg_context_async_reply_queue(
		void *ctxt);
static dispatch_continuation_t _dispatch_mach_msg_async_reply_wrap(
		dispatch_mach_msg_t dmsg, dispatch_mach_t dm);
static void _dispatch_mach_notification_kevent_unregister(dispatch_mach_t dm);
static void _dispatch_mach_notification_kevent_register(dispatch_mach_t dm,
		mach_port_t send);

// For tests only.
DISPATCH_EXPORT void _dispatch_mach_hooks_install_default(void);

dispatch_source_t
_dispatch_source_create_mach_msg_direct_recv(mach_port_t recvp,
		const struct dispatch_continuation_s *dc)
{
	dispatch_source_t ds;
	ds = dispatch_source_create(&_dispatch_source_type_mach_recv_direct,
			recvp, 0, &_dispatch_mgr_q);
	os_atomic_store(&ds->ds_refs->ds_handler[DS_EVENT_HANDLER],
			(dispatch_continuation_t)dc, relaxed);
	return ds;
}

#pragma mark -
#pragma mark dispatch to XPC callbacks

static dispatch_mach_xpc_hooks_t _dispatch_mach_xpc_hooks;

// Default dmxh_direct_message_handler callback that does not handle
// messages inline.
static bool
_dispatch_mach_xpc_no_handle_message(
		void *_Nullable context DISPATCH_UNUSED,
		dispatch_mach_reason_t reason DISPATCH_UNUSED,
		dispatch_mach_msg_t message DISPATCH_UNUSED,
		mach_error_t error DISPATCH_UNUSED)
{
	return false;
}

// Default dmxh_msg_context_reply_queue callback that returns a NULL queue.
static dispatch_queue_t
_dispatch_mach_msg_context_no_async_reply_queue(
		void *_Nonnull msg_context DISPATCH_UNUSED)
{
	return NULL;
}

// Default dmxh_async_reply_handler callback that crashes when called.
DISPATCH_NORETURN
static void
_dispatch_mach_default_async_reply_handler(void *context DISPATCH_UNUSED,
		dispatch_mach_reason_t reason DISPATCH_UNUSED,
		dispatch_mach_msg_t message DISPATCH_UNUSED)
{
	DISPATCH_CLIENT_CRASH(_dispatch_mach_xpc_hooks,
			"_dispatch_mach_default_async_reply_handler called");
}

// Default dmxh_enable_sigterm_notification callback that enables delivery of
// SIGTERM notifications (for backwards compatibility).
static bool
_dispatch_mach_enable_sigterm(void *_Nullable context DISPATCH_UNUSED)
{
	return true;
}

// Callbacks from dispatch to XPC. The default is to not support any callbacks.
static const struct dispatch_mach_xpc_hooks_s _dispatch_mach_xpc_hooks_default
		= {
	.version = DISPATCH_MACH_XPC_HOOKS_VERSION,
	.dmxh_direct_message_handler = &_dispatch_mach_xpc_no_handle_message,
	.dmxh_msg_context_reply_queue =
			&_dispatch_mach_msg_context_no_async_reply_queue,
	.dmxh_async_reply_handler = &_dispatch_mach_default_async_reply_handler,
	.dmxh_enable_sigterm_notification = &_dispatch_mach_enable_sigterm,
};

static dispatch_mach_xpc_hooks_t _dispatch_mach_xpc_hooks
		= &_dispatch_mach_xpc_hooks_default;

void
dispatch_mach_hooks_install_4libxpc(dispatch_mach_xpc_hooks_t hooks)
{
	if (!os_atomic_cmpxchg(&_dispatch_mach_xpc_hooks,
			&_dispatch_mach_xpc_hooks_default, hooks, relaxed)) {
		DISPATCH_CLIENT_CRASH(_dispatch_mach_xpc_hooks,
				"dispatch_mach_hooks_install_4libxpc called twice");
	}
}

void
_dispatch_mach_hooks_install_default(void)
{
	os_atomic_store(&_dispatch_mach_xpc_hooks,
			&_dispatch_mach_xpc_hooks_default, relaxed);
}

#pragma mark -
#pragma mark dispatch_mach_t

static dispatch_mach_t
_dispatch_mach_create(const char *label, dispatch_queue_t q, void *context,
		dispatch_mach_handler_function_t handler, bool handler_is_block,
		bool is_xpc)
{
	dispatch_mach_recv_refs_t dmrr;
	dispatch_mach_send_refs_t dmsr;
	dispatch_mach_t dm;
	dm = _dispatch_object_alloc(DISPATCH_VTABLE(mach),
			sizeof(struct dispatch_mach_s));
	_dispatch_queue_init(dm->_as_dq, DQF_LEGACY, 1,
			DISPATCH_QUEUE_INACTIVE | DISPATCH_QUEUE_ROLE_INNER);

	dm->dq_label = label;
	dm->do_ref_cnt++; // the reference _dispatch_mach_cancel_invoke holds
	dm->dm_is_xpc = is_xpc;

	dmrr = dux_create(&_dispatch_mach_type_recv, 0, 0)._dmrr;
	dispatch_assert(dmrr->du_is_direct);
	dmrr->du_owner_wref = _dispatch_ptr2wref(dm);
	dmrr->dmrr_handler_func = handler;
	dmrr->dmrr_handler_ctxt = context;
	dmrr->dmrr_handler_is_block = handler_is_block;
	dm->dm_recv_refs = dmrr;

	dmsr = dux_create(&_dispatch_mach_type_send, 0,
			DISPATCH_MACH_SEND_POSSIBLE|DISPATCH_MACH_SEND_DEAD)._dmsr;
	dmsr->du_owner_wref = _dispatch_ptr2wref(dm);
	dm->dm_send_refs = dmsr;

	if (slowpath(!q)) {
		q = _dispatch_get_root_queue(DISPATCH_QOS_DEFAULT, true);
	} else {
		_dispatch_retain(q);
	}
	dm->do_targetq = q;
	_dispatch_object_debug(dm, "%s", __func__);
	return dm;
}

dispatch_mach_t
dispatch_mach_create(const char *label, dispatch_queue_t q,
		dispatch_mach_handler_t handler)
{
	dispatch_block_t bb = _dispatch_Block_copy((void*)handler);
	return _dispatch_mach_create(label, q, bb,
			(dispatch_mach_handler_function_t)_dispatch_Block_invoke(bb), true,
			false);
}

dispatch_mach_t
dispatch_mach_create_f(const char *label, dispatch_queue_t q, void *context,
		dispatch_mach_handler_function_t handler)
{
	return _dispatch_mach_create(label, q, context, handler, false, false);
}

dispatch_mach_t
dispatch_mach_create_4libxpc(const char *label, dispatch_queue_t q,
		void *context, dispatch_mach_handler_function_t handler)
{
	return _dispatch_mach_create(label, q, context, handler, false, true);
}

void
_dispatch_mach_dispose(dispatch_mach_t dm, bool *allow_free)
{
	_dispatch_object_debug(dm, "%s", __func__);
	_dispatch_unote_dispose(dm->dm_recv_refs);
	dm->dm_recv_refs = NULL;
	_dispatch_unote_dispose(dm->dm_send_refs);
	dm->dm_send_refs = NULL;
	if (dm->dm_xpc_term_refs) {
		_dispatch_unote_dispose(dm->dm_xpc_term_refs);
		dm->dm_xpc_term_refs = NULL;
	}
	_dispatch_queue_destroy(dm->_as_dq, allow_free);
}

void
dispatch_mach_connect(dispatch_mach_t dm, mach_port_t receive,
		mach_port_t send, dispatch_mach_msg_t checkin)
{
	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	uint32_t disconnect_cnt;

	if (MACH_PORT_VALID(receive)) {
		dm->dm_recv_refs->du_ident = receive;
		_dispatch_retain(dm); // the reference the manager queue holds
	}
	dmsr->dmsr_send = send;
	if (MACH_PORT_VALID(send)) {
		if (checkin) {
			dispatch_mach_msg_t dmsg = checkin;
			dispatch_retain(dmsg);
			dmsg->dmsg_options = _dispatch_mach_checkin_options();
			dmsr->dmsr_checkin_port = _dispatch_mach_msg_get_remote_port(dmsg);
		}
		dmsr->dmsr_checkin = checkin;
	}
	dispatch_assert(DISPATCH_MACH_NEVER_CONNECTED - 1 ==
			DISPATCH_MACH_NEVER_INSTALLED);
	disconnect_cnt = os_atomic_dec2o(dmsr, dmsr_disconnect_cnt, release);
	if (unlikely(disconnect_cnt != DISPATCH_MACH_NEVER_INSTALLED)) {
		DISPATCH_CLIENT_CRASH(disconnect_cnt, "Channel already connected");
	}
	_dispatch_object_debug(dm, "%s", __func__);
	return dispatch_activate(dm);
}

static inline bool
_dispatch_mach_reply_tryremove(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr)
{
	bool removed;
	_dispatch_unfair_lock_lock(&dm->dm_send_refs->dmsr_replies_lock);
	if ((removed = _TAILQ_IS_ENQUEUED(dmr, dmr_list))) {
		TAILQ_REMOVE(&dm->dm_send_refs->dmsr_replies, dmr, dmr_list);
		_TAILQ_MARK_NOT_ENQUEUED(dmr, dmr_list);
	}
	_dispatch_unfair_lock_unlock(&dm->dm_send_refs->dmsr_replies_lock);
	return removed;
}

DISPATCH_NOINLINE
static void
_dispatch_mach_reply_waiter_unregister(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, uint32_t options)
{
	dispatch_mach_msg_t dmsgr = NULL;
	bool disconnected = (options & DU_UNREGISTER_DISCONNECTED);
	if (options & DU_UNREGISTER_REPLY_REMOVE) {
		_dispatch_unfair_lock_lock(&dm->dm_send_refs->dmsr_replies_lock);
		if (unlikely(!_TAILQ_IS_ENQUEUED(dmr, dmr_list))) {
			DISPATCH_INTERNAL_CRASH(0, "Could not find reply registration");
		}
		TAILQ_REMOVE(&dm->dm_send_refs->dmsr_replies, dmr, dmr_list);
		_TAILQ_MARK_NOT_ENQUEUED(dmr, dmr_list);
		_dispatch_unfair_lock_unlock(&dm->dm_send_refs->dmsr_replies_lock);
	}
	if (disconnected) {
		dmsgr = _dispatch_mach_msg_create_reply_disconnected(NULL, dmr,
				DISPATCH_MACH_DISCONNECTED);
	} else if (dmr->dmr_voucher) {
		_voucher_release(dmr->dmr_voucher);
		dmr->dmr_voucher = NULL;
	}
	_dispatch_debug("machport[0x%08x]: unregistering for sync reply%s, ctxt %p",
			_dispatch_mach_reply_get_reply_port((mach_port_t)dmr->du_ident),
			disconnected ? " (disconnected)" : "", dmr->dmr_ctxt);
	if (dmsgr) {
		return _dispatch_mach_handle_or_push_received_msg(dm, dmsgr);
	}
}

DISPATCH_NOINLINE
static bool
_dispatch_mach_reply_list_remove(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr) {
	// dmsr_replies_lock must be held by the caller.
	bool removed = false;
	if (likely(_TAILQ_IS_ENQUEUED(dmr, dmr_list))) {
		TAILQ_REMOVE(&dm->dm_send_refs->dmsr_replies, dmr, dmr_list);
		_TAILQ_MARK_NOT_ENQUEUED(dmr, dmr_list);
		removed = true;
	}
	return removed;
}

DISPATCH_NOINLINE
static bool
_dispatch_mach_reply_kevent_unregister(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, uint32_t options)
{
	dispatch_assert(!_TAILQ_IS_ENQUEUED(dmr, dmr_list));

	bool disconnected = (options & DU_UNREGISTER_DISCONNECTED);
	_dispatch_debug("machport[0x%08x]: unregistering for reply%s, ctxt %p",
			(mach_port_t)dmr->du_ident, disconnected ? " (disconnected)" : "",
			dmr->dmr_ctxt);
	if (!_dispatch_unote_unregister(dmr, options)) {
		_dispatch_debug("machport[0x%08x]: deferred delete kevent[%p]",
						(mach_port_t)dmr->du_ident, dmr);
		dispatch_assert(options == DU_UNREGISTER_DISCONNECTED);
		return false;
	}

	dispatch_mach_msg_t dmsgr = NULL;
	dispatch_queue_t drq = NULL;
	if (disconnected) {
		// The next call is guaranteed to always transfer or consume the voucher
		// in the dmr, if there is one.
		dmsgr = _dispatch_mach_msg_create_reply_disconnected(NULL, dmr,
			dmr->dmr_async_reply ? DISPATCH_MACH_ASYNC_WAITER_DISCONNECTED
			: DISPATCH_MACH_DISCONNECTED);
		if (dmr->dmr_ctxt) {
			drq = _dispatch_mach_msg_context_async_reply_queue(dmr->dmr_ctxt);
		}
		dispatch_assert(dmr->dmr_voucher == NULL);
	} else if (dmr->dmr_voucher) {
		_voucher_release(dmr->dmr_voucher);
		dmr->dmr_voucher = NULL;
	}
	_dispatch_unote_dispose(dmr);

	if (dmsgr) {
		if (drq) {
			_dispatch_mach_push_async_reply_msg(dm, dmsgr, drq);
		} else {
			_dispatch_mach_handle_or_push_received_msg(dm, dmsgr);
		}
	}
	return true;
}

DISPATCH_NOINLINE
static void
_dispatch_mach_reply_waiter_register(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, mach_port_t reply_port,
		dispatch_mach_msg_t dmsg, mach_msg_option_t msg_opts)
{
	dmr->du_owner_wref = _dispatch_ptr2wref(dm);
	dmr->du_wlh = NULL;
	dmr->du_filter = EVFILT_MACHPORT;
	dmr->du_ident = reply_port;
	if (msg_opts & DISPATCH_MACH_OWNED_REPLY_PORT) {
		_dispatch_mach_reply_mark_reply_port_owned(dmr);
	} else {
		if (dmsg->dmsg_voucher) {
			dmr->dmr_voucher = _voucher_retain(dmsg->dmsg_voucher);
		}
		dmr->dmr_priority = _dispatch_priority_from_pp(dmsg->dmsg_priority);
		// make reply context visible to leaks rdar://11777199
		dmr->dmr_ctxt = dmsg->do_ctxt;
	}

	_dispatch_debug("machport[0x%08x]: registering for sync reply, ctxt %p",
			reply_port, dmsg->do_ctxt);
	_dispatch_unfair_lock_lock(&dm->dm_send_refs->dmsr_replies_lock);
	if (unlikely(_TAILQ_IS_ENQUEUED(dmr, dmr_list))) {
		DISPATCH_INTERNAL_CRASH(dmr->dmr_list.tqe_prev,
				"Reply already registered");
	}
	TAILQ_INSERT_TAIL(&dm->dm_send_refs->dmsr_replies, dmr, dmr_list);
	_dispatch_unfair_lock_unlock(&dm->dm_send_refs->dmsr_replies_lock);
}

DISPATCH_NOINLINE
static void
_dispatch_mach_reply_kevent_register(dispatch_mach_t dm, mach_port_t reply_port,
		dispatch_mach_msg_t dmsg)
{
	dispatch_mach_reply_refs_t dmr;
	dispatch_priority_t mpri, pri, overcommit;
	dispatch_wlh_t wlh;

	dmr = dux_create(&_dispatch_mach_type_reply, reply_port, 0)._dmr;
	dispatch_assert(dmr->du_is_direct);
	dmr->du_owner_wref = _dispatch_ptr2wref(dm);
	if (dmsg->dmsg_voucher) {
		dmr->dmr_voucher = _voucher_retain(dmsg->dmsg_voucher);
	}
	dmr->dmr_priority = _dispatch_priority_from_pp(dmsg->dmsg_priority);
	// make reply context visible to leaks rdar://11777199
	dmr->dmr_ctxt = dmsg->do_ctxt;

	dispatch_queue_t drq = NULL;
	if (dmsg->dmsg_options & DISPATCH_MACH_ASYNC_REPLY) {
		dmr->dmr_async_reply = true;
		drq = _dispatch_mach_msg_context_async_reply_queue(dmsg->do_ctxt);
	}

	if (!drq) {
		pri = dm->dq_priority;
		wlh = dm->dm_recv_refs->du_wlh;
	} else if (dx_type(drq) == DISPATCH_QUEUE_NETWORK_EVENT_TYPE) {
		pri = DISPATCH_PRIORITY_FLAG_MANAGER;
		wlh = (dispatch_wlh_t)drq;
	} else if (dx_hastypeflag(drq, QUEUE_ROOT)) {
		pri = drq->dq_priority;
		wlh = DISPATCH_WLH_ANON;
	} else if (drq == dm->do_targetq) {
		pri = dm->dq_priority;
		wlh = dm->dm_recv_refs->du_wlh;
	} else if (!(pri = _dispatch_queue_compute_priority_and_wlh(drq, &wlh))) {
		pri = drq->dq_priority;
		wlh = DISPATCH_WLH_ANON;
	}
	if (pri & DISPATCH_PRIORITY_REQUESTED_MASK) {
		overcommit = pri & DISPATCH_PRIORITY_FLAG_OVERCOMMIT;
		pri &= DISPATCH_PRIORITY_REQUESTED_MASK;
		mpri = _dispatch_priority_from_pp_strip_flags(dmsg->dmsg_priority);
		if (pri < mpri) pri = mpri;
		pri |= overcommit;
	} else {
		pri = DISPATCH_PRIORITY_FLAG_MANAGER;
	}

	_dispatch_debug("machport[0x%08x]: registering for reply, ctxt %p",
			reply_port, dmsg->do_ctxt);
	_dispatch_unfair_lock_lock(&dm->dm_send_refs->dmsr_replies_lock);
	if (unlikely(_TAILQ_IS_ENQUEUED(dmr, dmr_list))) {
		DISPATCH_INTERNAL_CRASH(dmr->dmr_list.tqe_prev,
				"Reply already registered");
	}
	TAILQ_INSERT_TAIL(&dm->dm_send_refs->dmsr_replies, dmr, dmr_list);
	_dispatch_unfair_lock_unlock(&dm->dm_send_refs->dmsr_replies_lock);

	if (!_dispatch_unote_register(dmr, wlh, pri)) {
		_dispatch_unfair_lock_lock(&dm->dm_send_refs->dmsr_replies_lock);
		_dispatch_mach_reply_list_remove(dm, dmr);
		_dispatch_unfair_lock_unlock(&dm->dm_send_refs->dmsr_replies_lock);
		_dispatch_mach_reply_kevent_unregister(dm, dmr,
				DU_UNREGISTER_DISCONNECTED);
	}
}

#pragma mark -
#pragma mark dispatch_mach_msg

DISPATCH_ALWAYS_INLINE DISPATCH_CONST
static inline bool
_dispatch_use_mach_special_reply_port(void)
{
#if DISPATCH_USE_MACH_SEND_SYNC_OVERRIDE
	return true;
#else
#define thread_get_special_reply_port() ({__builtin_trap(); MACH_PORT_NULL;})
	return false;
#endif
}

static mach_port_t
_dispatch_get_thread_reply_port(void)
{
	mach_port_t reply_port, mrp;
	if (_dispatch_use_mach_special_reply_port()) {
		mrp = _dispatch_get_thread_special_reply_port();
	} else {
		mrp = _dispatch_get_thread_mig_reply_port();
	}
	if (mrp) {
		reply_port = mrp;
		_dispatch_debug("machport[0x%08x]: borrowed thread sync reply port",
				reply_port);
	} else {
		if (_dispatch_use_mach_special_reply_port()) {
			reply_port = thread_get_special_reply_port();
			_dispatch_set_thread_special_reply_port(reply_port);
		} else {
			reply_port = mach_reply_port();
			_dispatch_set_thread_mig_reply_port(reply_port);
		}
		if (unlikely(!MACH_PORT_VALID(reply_port))) {
			DISPATCH_CLIENT_CRASH(_dispatch_use_mach_special_reply_port(),
				"Unable to allocate reply port, possible port leak");
		}
		_dispatch_debug("machport[0x%08x]: allocated thread sync reply port",
				reply_port);
	}
	_dispatch_debug_machport(reply_port);
	return reply_port;
}

static void
_dispatch_clear_thread_reply_port(mach_port_t reply_port)
{
	mach_port_t mrp;
	if (_dispatch_use_mach_special_reply_port()) {
		mrp = _dispatch_get_thread_special_reply_port();
	} else {
		mrp = _dispatch_get_thread_mig_reply_port();
	}
	if (reply_port != mrp) {
		if (mrp) {
			_dispatch_debug("machport[0x%08x]: did not clear thread sync reply "
					"port (found 0x%08x)", reply_port, mrp);
		}
		return;
	}
	if (_dispatch_use_mach_special_reply_port()) {
		_dispatch_set_thread_special_reply_port(MACH_PORT_NULL);
	} else {
		_dispatch_set_thread_mig_reply_port(MACH_PORT_NULL);
	}
	_dispatch_debug_machport(reply_port);
	_dispatch_debug("machport[0x%08x]: cleared thread sync reply port",
			reply_port);
}

static void
_dispatch_set_thread_reply_port(mach_port_t reply_port)
{
	_dispatch_debug_machport(reply_port);
	mach_port_t mrp;
	if (_dispatch_use_mach_special_reply_port()) {
		mrp = _dispatch_get_thread_special_reply_port();
	} else {
		mrp = _dispatch_get_thread_mig_reply_port();
	}
	if (mrp) {
		kern_return_t kr = mach_port_mod_refs(mach_task_self(), reply_port,
				MACH_PORT_RIGHT_RECEIVE, -1);
		DISPATCH_VERIFY_MIG(kr);
		dispatch_assume_zero(kr);
		_dispatch_debug("machport[0x%08x]: deallocated sync reply port "
				"(found 0x%08x)", reply_port, mrp);
	} else {
		if (_dispatch_use_mach_special_reply_port()) {
			_dispatch_set_thread_special_reply_port(reply_port);
		} else {
			_dispatch_set_thread_mig_reply_port(reply_port);
		}
		_dispatch_debug("machport[0x%08x]: restored thread sync reply port",
				reply_port);
	}
}

static inline mach_port_t
_dispatch_mach_msg_get_remote_port(dispatch_object_t dou)
{
	mach_msg_header_t *hdr = _dispatch_mach_msg_get_msg(dou._dmsg);
	mach_port_t remote = hdr->msgh_remote_port;
	return remote;
}

static inline mach_port_t
_dispatch_mach_msg_get_reply_port(dispatch_object_t dou)
{
	mach_msg_header_t *hdr = _dispatch_mach_msg_get_msg(dou._dmsg);
	mach_port_t local = hdr->msgh_local_port;
	if (!MACH_PORT_VALID(local) || MACH_MSGH_BITS_LOCAL(hdr->msgh_bits) !=
			MACH_MSG_TYPE_MAKE_SEND_ONCE) return MACH_PORT_NULL;
	return local;
}

static inline void
_dispatch_mach_msg_set_reason(dispatch_mach_msg_t dmsg, mach_error_t err,
		unsigned long reason)
{
	dispatch_assert_zero(reason & ~(unsigned long)code_emask);
	dmsg->dmsg_error = ((err || !reason) ? err :
			 err_local|err_sub(0x3e0)|(mach_error_t)reason);
}

static inline unsigned long
_dispatch_mach_msg_get_reason(dispatch_mach_msg_t dmsg, mach_error_t *err_ptr)
{
	mach_error_t err = dmsg->dmsg_error;

	if ((err & system_emask) == err_local && err_get_sub(err) == 0x3e0) {
		*err_ptr = 0;
		return err_get_code(err);
	}
	*err_ptr = err;
	return err ? DISPATCH_MACH_MESSAGE_SEND_FAILED : DISPATCH_MACH_MESSAGE_SENT;
}

static inline dispatch_mach_msg_t
_dispatch_mach_msg_create_recv(mach_msg_header_t *hdr, mach_msg_size_t siz,
		dispatch_mach_reply_refs_t dmr, uint32_t flags)
{
	dispatch_mach_msg_destructor_t destructor;
	dispatch_mach_msg_t dmsg;
	voucher_t voucher;
	pthread_priority_t pp;

	if (dmr) {
		_voucher_mach_msg_clear(hdr, false); // deallocate reply message voucher
		pp = _dispatch_priority_to_pp(dmr->dmr_priority);
		voucher = dmr->dmr_voucher;
		dmr->dmr_voucher = NULL; // transfer reference
	} else {
		voucher = voucher_create_with_mach_msg(hdr);
		pp = _dispatch_priority_compute_propagated(
				_voucher_get_priority(voucher), 0);
	}

	destructor = (flags & DISPATCH_EV_MSG_NEEDS_FREE) ?
			DISPATCH_MACH_MSG_DESTRUCTOR_FREE :
			DISPATCH_MACH_MSG_DESTRUCTOR_DEFAULT;
	dmsg = dispatch_mach_msg_create(hdr, siz, destructor, NULL);
	if (!(flags & DISPATCH_EV_MSG_NEEDS_FREE)) {
		_dispatch_ktrace2(DISPATCH_MACH_MSG_hdr_move,
				(uint64_t)hdr, (uint64_t)dmsg->dmsg_buf);
	}
	dmsg->dmsg_voucher = voucher;
	dmsg->dmsg_priority = pp;
	dmsg->do_ctxt = dmr ? dmr->dmr_ctxt : NULL;
	_dispatch_mach_msg_set_reason(dmsg, 0, DISPATCH_MACH_MESSAGE_RECEIVED);
	_dispatch_voucher_debug("mach-msg[%p] create", voucher, dmsg);
	_dispatch_voucher_ktrace_dmsg_push(dmsg);
	return dmsg;
}

void
_dispatch_mach_merge_msg(dispatch_unote_t du, uint32_t flags,
		mach_msg_header_t *hdr, mach_msg_size_t siz)
{
	// this function is very similar with what _dispatch_source_merge_evt does
	// but can't reuse it as handling the message must be protected by the
	// internal refcount between the first half and the trailer of what
	// _dispatch_source_merge_evt does.

	dispatch_mach_recv_refs_t dmrr = du._dmrr;
	dispatch_mach_t dm = _dispatch_wref2ptr(dmrr->du_owner_wref);
	dispatch_queue_flags_t dqf;
	dispatch_mach_msg_t dmsg;

	dispatch_assert(_dispatch_unote_needs_rearm(du));

	if (flags & EV_VANISHED) {
		DISPATCH_CLIENT_CRASH(du._du->du_ident,
				"Unexpected EV_VANISHED (do not destroy random mach ports)");
	}

	// once we modify the queue atomic flags below, it will allow concurrent
	// threads running _dispatch_mach_invoke2 to dispose of the source,
	// so we can't safely borrow the reference we get from the muxnote udata
	// anymore, and need our own
	dispatch_wakeup_flags_t wflags = DISPATCH_WAKEUP_CONSUME_2;
	_dispatch_retain_2(dm); // rdar://20382435

	if (unlikely((flags & EV_ONESHOT) && !(flags & EV_DELETE))) {
		dqf = _dispatch_queue_atomic_flags_set_and_clear(dm->_as_dq,
				DSF_DEFERRED_DELETE, DSF_ARMED);
		_dispatch_debug("kevent-source[%p]: deferred delete oneshot kevent[%p]",
				dm, dmrr);
	} else if (unlikely(flags & (EV_ONESHOT | EV_DELETE))) {
		_dispatch_source_refs_unregister(dm->_as_ds,
				DU_UNREGISTER_ALREADY_DELETED);
		dqf = _dispatch_queue_atomic_flags(dm->_as_dq);
		_dispatch_debug("kevent-source[%p]: deleted kevent[%p]", dm, dmrr);
	} else {
		dqf = _dispatch_queue_atomic_flags_clear(dm->_as_dq, DSF_ARMED);
		_dispatch_debug("kevent-source[%p]: disarmed kevent[%p]", dm, dmrr);
	}

	_dispatch_debug_machport(hdr->msgh_remote_port);
	_dispatch_debug("machport[0x%08x]: received msg id 0x%x, reply on 0x%08x",
			hdr->msgh_local_port, hdr->msgh_id, hdr->msgh_remote_port);

	if (dqf & DSF_CANCELED) {
		_dispatch_debug("machport[0x%08x]: drop msg id 0x%x, reply on 0x%08x",
				hdr->msgh_local_port, hdr->msgh_id, hdr->msgh_remote_port);
		mach_msg_destroy(hdr);
		if (flags & DISPATCH_EV_MSG_NEEDS_FREE) {
			free(hdr);
		}
		return dx_wakeup(dm, 0, wflags | DISPATCH_WAKEUP_MAKE_DIRTY);
	}

	// Once the mach channel disarming is visible, cancellation will switch to
	// immediate deletion.  If we're preempted here, then the whole cancellation
	// sequence may be complete by the time we really enqueue the message.
	//
	// _dispatch_mach_msg_invoke_with_mach() is responsible for filtering it out
	// to keep the promise that DISPATCH_MACH_DISCONNECTED is the last
	// event sent.

	dmsg = _dispatch_mach_msg_create_recv(hdr, siz, NULL, flags);
	_dispatch_mach_handle_or_push_received_msg(dm, dmsg);
	return _dispatch_release_2_tailcall(dm);
}

void
_dispatch_mach_reply_merge_msg(dispatch_unote_t du, uint32_t flags,
		mach_msg_header_t *hdr, mach_msg_size_t siz)
{
	dispatch_mach_reply_refs_t dmr = du._dmr;
	dispatch_mach_t dm = _dispatch_wref2ptr(dmr->du_owner_wref);
	bool canceled = (_dispatch_queue_atomic_flags(dm->_as_dq) & DSF_CANCELED);
	dispatch_mach_msg_t dmsg = NULL;

	_dispatch_debug_machport(hdr->msgh_remote_port);
	_dispatch_debug("machport[0x%08x]: received msg id 0x%x, reply on 0x%08x",
			hdr->msgh_local_port, hdr->msgh_id, hdr->msgh_remote_port);

	if (!canceled) {
		dmsg = _dispatch_mach_msg_create_recv(hdr, siz, dmr, flags);
	}

	if (dmsg) {
		dispatch_queue_t drq = NULL;
		if (dmsg->do_ctxt) {
			drq = _dispatch_mach_msg_context_async_reply_queue(dmsg->do_ctxt);
		}
		if (drq) {
			_dispatch_mach_push_async_reply_msg(dm, dmsg, drq);
		} else {
			_dispatch_mach_handle_or_push_received_msg(dm, dmsg);
		}
	} else {
		_dispatch_debug("machport[0x%08x]: drop msg id 0x%x, reply on 0x%08x",
				hdr->msgh_local_port, hdr->msgh_id, hdr->msgh_remote_port);
		mach_msg_destroy(hdr);
		if (flags & DISPATCH_EV_MSG_NEEDS_FREE) {
			free(hdr);
		}
	}

	dispatch_wakeup_flags_t wflags = 0;
	uint32_t options = DU_UNREGISTER_IMMEDIATE_DELETE;
	if (canceled) {
		options |= DU_UNREGISTER_DISCONNECTED;
	}

	_dispatch_unfair_lock_lock(&dm->dm_send_refs->dmsr_replies_lock);
	bool removed = _dispatch_mach_reply_list_remove(dm, dmr);
	dispatch_assert(removed);
	if (TAILQ_EMPTY(&dm->dm_send_refs->dmsr_replies) &&
			(dm->dm_send_refs->dmsr_disconnect_cnt ||
			(dm->dq_atomic_flags & DSF_CANCELED))) {
		// When the list is empty, _dispatch_mach_disconnect() may release the
		// last reference count on the Mach channel. To avoid this, take our
		// own reference before releasing the lock.
		wflags = DISPATCH_WAKEUP_MAKE_DIRTY | DISPATCH_WAKEUP_CONSUME_2;
		_dispatch_retain_2(dm);
	}
	_dispatch_unfair_lock_unlock(&dm->dm_send_refs->dmsr_replies_lock);

	bool result = _dispatch_mach_reply_kevent_unregister(dm, dmr, options);
	dispatch_assert(result);
	if (wflags) dx_wakeup(dm, 0, wflags);
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_mach_msg_t
_dispatch_mach_msg_reply_recv(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, mach_port_t reply_port,
		mach_port_t send)
{
	if (slowpath(!MACH_PORT_VALID(reply_port))) {
		DISPATCH_CLIENT_CRASH(reply_port, "Invalid reply port");
	}
	void *ctxt = dmr->dmr_ctxt;
	mach_msg_header_t *hdr, *hdr2 = NULL;
	void *hdr_copyout_addr;
	mach_msg_size_t siz, msgsiz = 0;
	mach_msg_return_t kr;
	mach_msg_option_t options;
	mach_port_t notify = MACH_PORT_NULL;
	siz = mach_vm_round_page(DISPATCH_MACH_RECEIVE_MAX_INLINE_MESSAGE_SIZE +
			DISPATCH_MACH_TRAILER_SIZE);
	hdr = alloca(siz);
	for (mach_vm_address_t p = mach_vm_trunc_page(hdr + vm_page_size);
			p < (mach_vm_address_t)hdr + siz; p += vm_page_size) {
		*(char*)p = 0; // ensure alloca buffer doesn't overlap with stack guard
	}
	options = DISPATCH_MACH_RCV_OPTIONS & (~MACH_RCV_VOUCHER);
	if (MACH_PORT_VALID(send)) {
		notify = send;
		options |= MACH_RCV_SYNC_WAIT;
	}

retry:
	_dispatch_debug_machport(reply_port);
	_dispatch_debug("machport[0x%08x]: MACH_RCV_MSG %s", reply_port,
			(options & MACH_RCV_TIMEOUT) ? "poll" : "wait");
	kr = mach_msg(hdr, options, 0, siz, reply_port, MACH_MSG_TIMEOUT_NONE,
			notify);
	hdr_copyout_addr = hdr;
	_dispatch_debug_machport(reply_port);
	_dispatch_debug("machport[0x%08x]: MACH_RCV_MSG (size %u, opts 0x%x) "
			"returned: %s - 0x%x", reply_port, siz, options,
			mach_error_string(kr), kr);
	switch (kr) {
	case MACH_RCV_TOO_LARGE:
		if (!fastpath(hdr->msgh_size <= UINT_MAX -
				DISPATCH_MACH_TRAILER_SIZE)) {
			DISPATCH_CLIENT_CRASH(hdr->msgh_size, "Overlarge message");
		}
		if (options & MACH_RCV_LARGE) {
			msgsiz = hdr->msgh_size + DISPATCH_MACH_TRAILER_SIZE;
			hdr2 = malloc(msgsiz);
			if (dispatch_assume(hdr2)) {
				hdr = hdr2;
				siz = msgsiz;
			}
			options |= MACH_RCV_TIMEOUT;
			options &= ~MACH_RCV_LARGE;
			goto retry;
		}
		_dispatch_log("BUG in libdispatch client: "
				"dispatch_mach_send_and_wait_for_reply: dropped message too "
				"large to fit in memory: id = 0x%x, size = %u", hdr->msgh_id,
				hdr->msgh_size);
		break;
	case MACH_RCV_INVALID_NAME: // rdar://problem/21963848
	case MACH_RCV_PORT_CHANGED: // rdar://problem/21885327
	case MACH_RCV_PORT_DIED:
		// channel was disconnected/canceled and reply port destroyed
		_dispatch_debug("machport[0x%08x]: sync reply port destroyed, ctxt %p: "
				"%s - 0x%x", reply_port, ctxt, mach_error_string(kr), kr);
		goto out;
	case MACH_MSG_SUCCESS:
		if (hdr->msgh_remote_port) {
			_dispatch_debug_machport(hdr->msgh_remote_port);
		}
		_dispatch_debug("machport[0x%08x]: received msg id 0x%x, size = %u, "
				"reply on 0x%08x", hdr->msgh_local_port, hdr->msgh_id,
				hdr->msgh_size, hdr->msgh_remote_port);
		siz = hdr->msgh_size + DISPATCH_MACH_TRAILER_SIZE;
		if (hdr2 && siz < msgsiz) {
			void *shrink = realloc(hdr2, msgsiz);
			if (shrink) hdr = hdr2 = shrink;
		}
		break;
	case MACH_RCV_INVALID_NOTIFY:
	default:
		DISPATCH_INTERNAL_CRASH(kr, "Unexpected error from mach_msg_receive");
		break;
	}
	_dispatch_mach_msg_reply_received(dm, dmr, hdr->msgh_local_port);
	hdr->msgh_local_port = MACH_PORT_NULL;
	if (slowpath((dm->dq_atomic_flags & DSF_CANCELED) || kr)) {
		if (!kr) mach_msg_destroy(hdr);
		goto out;
	}
	dispatch_mach_msg_t dmsg;
	dispatch_mach_msg_destructor_t destructor = (!hdr2) ?
			DISPATCH_MACH_MSG_DESTRUCTOR_DEFAULT :
			DISPATCH_MACH_MSG_DESTRUCTOR_FREE;
	dmsg = dispatch_mach_msg_create(hdr, siz, destructor, NULL);
	if (!hdr2 || hdr != hdr_copyout_addr) {
		_dispatch_ktrace2(DISPATCH_MACH_MSG_hdr_move,
				(uint64_t)hdr_copyout_addr,
				(uint64_t)_dispatch_mach_msg_get_msg(dmsg));
	}
	dmsg->do_ctxt = ctxt;
	return dmsg;
out:
	free(hdr2);
	return NULL;
}

static inline void
_dispatch_mach_msg_reply_received(dispatch_mach_t dm,
		dispatch_mach_reply_refs_t dmr, mach_port_t local_port)
{
	bool removed = _dispatch_mach_reply_tryremove(dm, dmr);
	if (!MACH_PORT_VALID(local_port) || !removed) {
		// port moved/destroyed during receive, or reply waiter was never
		// registered or already removed (disconnected)
		return;
	}
	mach_port_t reply_port = _dispatch_mach_reply_get_reply_port(
			(mach_port_t)dmr->du_ident);
	_dispatch_debug("machport[0x%08x]: unregistered for sync reply, ctxt %p",
			reply_port, dmr->dmr_ctxt);
	if (_dispatch_mach_reply_is_reply_port_owned(dmr)) {
		_dispatch_set_thread_reply_port(reply_port);
		if (local_port != reply_port) {
			DISPATCH_CLIENT_CRASH(local_port,
					"Reply received on unexpected port");
		}
		return;
	}
	mach_msg_header_t *hdr;
	dispatch_mach_msg_t dmsg;
	dmsg = dispatch_mach_msg_create(NULL, sizeof(mach_msg_header_t),
			DISPATCH_MACH_MSG_DESTRUCTOR_DEFAULT, &hdr);
	hdr->msgh_local_port = local_port;
	dmsg->dmsg_voucher = dmr->dmr_voucher;
	dmr->dmr_voucher = NULL;  // transfer reference
	dmsg->dmsg_priority = _dispatch_priority_to_pp(dmr->dmr_priority);
	dmsg->do_ctxt = dmr->dmr_ctxt;
	_dispatch_mach_msg_set_reason(dmsg, 0, DISPATCH_MACH_REPLY_RECEIVED);
	return _dispatch_mach_handle_or_push_received_msg(dm, dmsg);
}

static inline void
_dispatch_mach_msg_disconnected(dispatch_mach_t dm, mach_port_t local_port,
		mach_port_t remote_port)
{
	mach_msg_header_t *hdr;
	dispatch_mach_msg_t dmsg;
	dmsg = dispatch_mach_msg_create(NULL, sizeof(mach_msg_header_t),
			DISPATCH_MACH_MSG_DESTRUCTOR_DEFAULT, &hdr);
	if (local_port) hdr->msgh_local_port = local_port;
	if (remote_port) hdr->msgh_remote_port = remote_port;
	_dispatch_mach_msg_set_reason(dmsg, 0, DISPATCH_MACH_DISCONNECTED);
	_dispatch_debug("machport[0x%08x]: %s right disconnected", local_port ?
			local_port : remote_port, local_port ? "receive" : "send");
	return _dispatch_mach_handle_or_push_received_msg(dm, dmsg);
}

static inline dispatch_mach_msg_t
_dispatch_mach_msg_create_reply_disconnected(dispatch_object_t dou,
		dispatch_mach_reply_refs_t dmr, dispatch_mach_reason_t reason)
{
	dispatch_mach_msg_t dmsg = dou._dmsg, dmsgr;
	mach_port_t reply_port = dmsg ? dmsg->dmsg_reply :
			_dispatch_mach_reply_get_reply_port((mach_port_t)dmr->du_ident);
	voucher_t v;

	if (!reply_port) {
		if (!dmsg) {
			v = dmr->dmr_voucher;
			dmr->dmr_voucher = NULL; // transfer reference
			if (v) _voucher_release(v);
		}
		return NULL;
	}

	if (dmsg) {
		v = dmsg->dmsg_voucher;
		if (v) _voucher_retain(v);
	} else {
		v = dmr->dmr_voucher;
		dmr->dmr_voucher = NULL; // transfer reference
	}

	if ((dmsg && (dmsg->dmsg_options & DISPATCH_MACH_WAIT_FOR_REPLY) &&
			(dmsg->dmsg_options & DISPATCH_MACH_OWNED_REPLY_PORT)) ||
			(dmr && !_dispatch_unote_registered(dmr) &&
			_dispatch_mach_reply_is_reply_port_owned(dmr))) {
		if (v) _voucher_release(v);
		// deallocate owned reply port to break _dispatch_mach_msg_reply_recv
		// out of waiting in mach_msg(MACH_RCV_MSG)
		kern_return_t kr = mach_port_mod_refs(mach_task_self(), reply_port,
				MACH_PORT_RIGHT_RECEIVE, -1);
		DISPATCH_VERIFY_MIG(kr);
		dispatch_assume_zero(kr);
		return NULL;
	}

	mach_msg_header_t *hdr;
	dmsgr = dispatch_mach_msg_create(NULL, sizeof(mach_msg_header_t),
			DISPATCH_MACH_MSG_DESTRUCTOR_DEFAULT, &hdr);
	dmsgr->dmsg_voucher = v;
	hdr->msgh_local_port = reply_port;
	if (dmsg) {
		dmsgr->dmsg_priority = dmsg->dmsg_priority;
		dmsgr->do_ctxt = dmsg->do_ctxt;
	} else {
		dmsgr->dmsg_priority = _dispatch_priority_to_pp(dmr->dmr_priority);
		dmsgr->do_ctxt = dmr->dmr_ctxt;
	}
	_dispatch_mach_msg_set_reason(dmsgr, 0, reason);
	_dispatch_debug("machport[0x%08x]: reply disconnected, ctxt %p",
			hdr->msgh_local_port, dmsgr->do_ctxt);
	return dmsgr;
}

DISPATCH_NOINLINE
static void
_dispatch_mach_msg_not_sent(dispatch_mach_t dm, dispatch_object_t dou)
{
	dispatch_mach_msg_t dmsg = dou._dmsg, dmsgr;
	dispatch_queue_t drq = NULL;
	mach_msg_header_t *msg = _dispatch_mach_msg_get_msg(dmsg);
	mach_msg_option_t msg_opts = dmsg->dmsg_options;
	_dispatch_debug("machport[0x%08x]: not sent msg id 0x%x, ctxt %p, "
			"msg_opts 0x%x, kvoucher 0x%08x, reply on 0x%08x",
			msg->msgh_remote_port, msg->msgh_id, dmsg->do_ctxt,
			msg_opts, msg->msgh_voucher_port, dmsg->dmsg_reply);
	unsigned long reason = (msg_opts & DISPATCH_MACH_REGISTER_FOR_REPLY) ?
			0 : DISPATCH_MACH_MESSAGE_NOT_SENT;
	dmsgr = _dispatch_mach_msg_create_reply_disconnected(dmsg, NULL,
			msg_opts & DISPATCH_MACH_ASYNC_REPLY
			? DISPATCH_MACH_ASYNC_WAITER_DISCONNECTED
			: DISPATCH_MACH_DISCONNECTED);
	if (dmsg->do_ctxt) {
		drq = _dispatch_mach_msg_context_async_reply_queue(dmsg->do_ctxt);
	}
	_dispatch_mach_msg_set_reason(dmsg, 0, reason);
	_dispatch_mach_handle_or_push_received_msg(dm, dmsg);
	if (dmsgr) {
		if (drq) {
			_dispatch_mach_push_async_reply_msg(dm, dmsgr, drq);
		} else {
			_dispatch_mach_handle_or_push_received_msg(dm, dmsgr);
		}
	}
}

DISPATCH_NOINLINE
static uint32_t
_dispatch_mach_msg_send(dispatch_mach_t dm, dispatch_object_t dou,
		dispatch_mach_reply_refs_t dmr, dispatch_qos_t qos,
		dispatch_mach_send_invoke_flags_t send_flags)
{
	dispatch_mach_send_refs_t dsrr = dm->dm_send_refs;
	dispatch_mach_msg_t dmsg = dou._dmsg, dmsgr = NULL;
	voucher_t voucher = dmsg->dmsg_voucher;
	dispatch_queue_t drq = NULL;
	mach_voucher_t ipc_kvoucher = MACH_VOUCHER_NULL;
	uint32_t send_status = 0;
	bool clear_voucher = false, kvoucher_move_send = false;
	mach_msg_header_t *msg = _dispatch_mach_msg_get_msg(dmsg);
	bool is_reply = (MACH_MSGH_BITS_REMOTE(msg->msgh_bits) ==
			MACH_MSG_TYPE_MOVE_SEND_ONCE);
	mach_port_t reply_port = dmsg->dmsg_reply;
	if (!is_reply) {
		dm->dm_needs_mgr = 0;
		if (unlikely(dsrr->dmsr_checkin && dmsg != dsrr->dmsr_checkin)) {
			// send initial checkin message
			if (unlikely(_dispatch_unote_registered(dsrr) &&
					_dispatch_queue_get_current() != &_dispatch_mgr_q)) {
				// send kevent must be uninstalled on the manager queue
				dm->dm_needs_mgr = 1;
				goto out;
			}
			if (unlikely(!_dispatch_mach_msg_send(dm,
					dsrr->dmsr_checkin, NULL, qos, DM_SEND_INVOKE_NONE))) {
				goto out;
			}
			dsrr->dmsr_checkin = NULL;
		}
	}
	mach_msg_return_t kr = 0;
	mach_msg_option_t opts = 0, msg_opts = dmsg->dmsg_options;
	if (!(msg_opts & DISPATCH_MACH_REGISTER_FOR_REPLY)) {
		mach_msg_priority_t msg_priority = MACH_MSG_PRIORITY_UNSPECIFIED;
		opts = MACH_SEND_MSG | (msg_opts & ~DISPATCH_MACH_OPTIONS_MASK);
		if (!is_reply) {
			if (dmsg != dsrr->dmsr_checkin) {
				msg->msgh_remote_port = dsrr->dmsr_send;
			}
			if (_dispatch_queue_get_current() == &_dispatch_mgr_q) {
				if (unlikely(!_dispatch_unote_registered(dsrr))) {
					_dispatch_mach_notification_kevent_register(dm,
							msg->msgh_remote_port);
				}
				if (likely(_dispatch_unote_registered(dsrr))) {
					if (os_atomic_load2o(dsrr, dmsr_notification_armed,
							relaxed)) {
						goto out;
					}
					opts |= MACH_SEND_NOTIFY;
				}
			}
			opts |= MACH_SEND_TIMEOUT;
			if (dmsg->dmsg_priority != _voucher_get_priority(voucher)) {
				ipc_kvoucher = _voucher_create_mach_voucher_with_priority(
						voucher, dmsg->dmsg_priority);
			}
			_dispatch_voucher_debug("mach-msg[%p] msg_set", voucher, dmsg);
			if (ipc_kvoucher) {
				kvoucher_move_send = true;
				clear_voucher = _voucher_mach_msg_set_mach_voucher(msg,
						ipc_kvoucher, kvoucher_move_send);
			} else {
				clear_voucher = _voucher_mach_msg_set(msg, voucher);
			}
			if (qos) {
				opts |= MACH_SEND_OVERRIDE;
				msg_priority = (mach_msg_priority_t)
						_dispatch_priority_compute_propagated(
						_dispatch_qos_to_pp(qos), 0);
			}
		}
		_dispatch_debug_machport(msg->msgh_remote_port);
		if (reply_port) _dispatch_debug_machport(reply_port);
		if (msg_opts & DISPATCH_MACH_WAIT_FOR_REPLY) {
			if (msg_opts & DISPATCH_MACH_OWNED_REPLY_PORT) {
				if (_dispatch_use_mach_special_reply_port()) {
					opts |= MACH_SEND_SYNC_OVERRIDE;
				}
				_dispatch_clear_thread_reply_port(reply_port);
			}
			_dispatch_mach_reply_waiter_register(dm, dmr, reply_port, dmsg,
					msg_opts);
		}
		kr = mach_msg(msg, opts, msg->msgh_size, 0, MACH_PORT_NULL, 0,
				msg_priority);
		_dispatch_debug("machport[0x%08x]: sent msg id 0x%x, ctxt %p, "
				"opts 0x%x, msg_opts 0x%x, kvoucher 0x%08x, reply on 0x%08x: "
				"%s - 0x%x", msg->msgh_remote_port, msg->msgh_id, dmsg->do_ctxt,
				opts, msg_opts, msg->msgh_voucher_port, reply_port,
				mach_error_string(kr), kr);
		if (unlikely(kr && (msg_opts & DISPATCH_MACH_WAIT_FOR_REPLY))) {
			_dispatch_mach_reply_waiter_unregister(dm, dmr,
					DU_UNREGISTER_REPLY_REMOVE);
		}
		if (clear_voucher) {
			if (kr == MACH_SEND_INVALID_VOUCHER && msg->msgh_voucher_port) {
				DISPATCH_CLIENT_CRASH(kr, "Voucher port corruption");
			}
			mach_voucher_t kv;
			kv = _voucher_mach_msg_clear(msg, kvoucher_move_send);
			if (kvoucher_move_send) ipc_kvoucher = kv;
		}
	}
	if (kr == MACH_SEND_TIMED_OUT && (opts & MACH_SEND_TIMEOUT)) {
		if (opts & MACH_SEND_NOTIFY) {
			_dispatch_debug("machport[0x%08x]: send-possible notification "
					"armed", (mach_port_t)dsrr->du_ident);
			_dispatch_mach_notification_set_armed(dsrr);
		} else {
			// send kevent must be installed on the manager queue
			dm->dm_needs_mgr = 1;
		}
		if (ipc_kvoucher) {
			_dispatch_kvoucher_debug("reuse on re-send", ipc_kvoucher);
			voucher_t ipc_voucher;
			ipc_voucher = _voucher_create_with_priority_and_mach_voucher(
					voucher, dmsg->dmsg_priority, ipc_kvoucher);
			_dispatch_voucher_debug("mach-msg[%p] replace voucher[%p]",
					ipc_voucher, dmsg, voucher);
			if (dmsg->dmsg_voucher) _voucher_release(dmsg->dmsg_voucher);
			dmsg->dmsg_voucher = ipc_voucher;
		}
		goto out;
	} else if (ipc_kvoucher && (kr || !kvoucher_move_send)) {
		_voucher_dealloc_mach_voucher(ipc_kvoucher);
	}
	dispatch_mach_recv_refs_t dmrr = dm->dm_recv_refs;
	if (!(msg_opts & DISPATCH_MACH_WAIT_FOR_REPLY) && !kr && reply_port &&
			!(_dispatch_unote_registered(dmrr) &&
			dmrr->du_ident == reply_port)) {
		_dispatch_mach_reply_kevent_register(dm, reply_port, dmsg);
	}
	if (unlikely(!is_reply && dmsg == dsrr->dmsr_checkin &&
			_dispatch_unote_registered(dsrr))) {
		_dispatch_mach_notification_kevent_unregister(dm);
	}
	if (slowpath(kr)) {
		// Send failed, so reply was never registered <rdar://problem/14309159>
		dmsgr = _dispatch_mach_msg_create_reply_disconnected(dmsg, NULL,
				msg_opts & DISPATCH_MACH_ASYNC_REPLY
				? DISPATCH_MACH_ASYNC_WAITER_DISCONNECTED
				: DISPATCH_MACH_DISCONNECTED);
		if (dmsg->do_ctxt) {
			drq = _dispatch_mach_msg_context_async_reply_queue(dmsg->do_ctxt);
		}
	}
	_dispatch_mach_msg_set_reason(dmsg, kr, 0);
	if ((send_flags & DM_SEND_INVOKE_IMMEDIATE_SEND) &&
			(msg_opts & DISPATCH_MACH_RETURN_IMMEDIATE_SEND_RESULT)) {
		// Return sent message synchronously <rdar://problem/25947334>
		send_status |= DM_SEND_STATUS_RETURNING_IMMEDIATE_SEND_RESULT;
	} else {
		_dispatch_mach_handle_or_push_received_msg(dm, dmsg);
	}
	if (dmsgr) {
		if (drq) {
			_dispatch_mach_push_async_reply_msg(dm, dmsgr, drq);
		} else {
			_dispatch_mach_handle_or_push_received_msg(dm, dmsgr);
		}
	}
	send_status |= DM_SEND_STATUS_SUCCESS;
out:
	return send_status;
}

#pragma mark -
#pragma mark dispatch_mach_send_refs_t

#define _dmsr_state_needs_lock_override(dq_state, qos) \
		unlikely(qos < _dq_state_max_qos(dq_state))

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dmsr_state_max_qos(uint64_t dmsr_state)
{
	return _dq_state_max_qos(dmsr_state);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dmsr_state_needs_override(uint64_t dmsr_state, dispatch_qos_t qos)
{
	dmsr_state &= DISPATCH_MACH_STATE_MAX_QOS_MASK;
	return dmsr_state < _dq_state_from_qos(qos);
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_dmsr_state_merge_override(uint64_t dmsr_state, dispatch_qos_t qos)
{
	if (_dmsr_state_needs_override(dmsr_state, qos)) {
		dmsr_state &= ~DISPATCH_MACH_STATE_MAX_QOS_MASK;
		dmsr_state |= _dq_state_from_qos(qos);
		dmsr_state |= DISPATCH_MACH_STATE_DIRTY;
		dmsr_state |= DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
	}
	return dmsr_state;
}

#define _dispatch_mach_send_push_update_tail(dmsr, tail) \
		os_mpsc_push_update_tail(dmsr, dmsr, tail, do_next)
#define _dispatch_mach_send_push_update_head(dmsr, head) \
		os_mpsc_push_update_head(dmsr, dmsr, head)
#define _dispatch_mach_send_get_head(dmsr) \
		os_mpsc_get_head(dmsr, dmsr)
#define _dispatch_mach_send_unpop_head(dmsr, dc, dc_next) \
		os_mpsc_undo_pop_head(dmsr, dmsr, dc, dc_next, do_next)
#define _dispatch_mach_send_pop_head(dmsr, head) \
		os_mpsc_pop_head(dmsr, dmsr, head, do_next)

#define dm_push(dm, dc, qos) \
		_dispatch_queue_push((dm)->_as_dq, dc, qos)

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_mach_send_push_inline(dispatch_mach_send_refs_t dmsr,
		dispatch_object_t dou)
{
	if (_dispatch_mach_send_push_update_tail(dmsr, dou._do)) {
		_dispatch_mach_send_push_update_head(dmsr, dou._do);
		return true;
	}
	return false;
}

DISPATCH_NOINLINE
static bool
_dispatch_mach_send_drain(dispatch_mach_t dm, dispatch_invoke_flags_t flags,
		dispatch_mach_send_invoke_flags_t send_flags)
{
	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	dispatch_mach_reply_refs_t dmr;
	dispatch_mach_msg_t dmsg;
	struct dispatch_object_s *dc = NULL, *next_dc = NULL;
	dispatch_qos_t qos = _dmsr_state_max_qos(dmsr->dmsr_state);
	uint64_t old_state, new_state;
	uint32_t send_status;
	bool needs_mgr, disconnecting, returning_send_result = false;

again:
	needs_mgr = false; disconnecting = false;
	while (dmsr->dmsr_tail) {
		dc = _dispatch_mach_send_get_head(dmsr);
		do {
			dispatch_mach_send_invoke_flags_t sf = send_flags;
			// Only request immediate send result for the first message
			send_flags &= ~DM_SEND_INVOKE_IMMEDIATE_SEND_MASK;
			next_dc = _dispatch_mach_send_pop_head(dmsr, dc);
			if (_dispatch_object_has_type(dc,
					DISPATCH_CONTINUATION_TYPE(MACH_SEND_BARRIER))) {
				if (!(send_flags & DM_SEND_INVOKE_CAN_RUN_BARRIER)) {
					goto partial_drain;
				}
				_dispatch_continuation_pop(dc, NULL, flags, dm->_as_dq);
				continue;
			}
			if (_dispatch_object_is_sync_waiter(dc)) {
				dmsg = ((dispatch_continuation_t)dc)->dc_data;
				dmr = ((dispatch_continuation_t)dc)->dc_other;
			} else if (_dispatch_object_has_vtable(dc)) {
				dmsg = (dispatch_mach_msg_t)dc;
				dmr = NULL;
			} else {
				if (_dispatch_unote_registered(dmsr) &&
						(_dispatch_queue_get_current() != &_dispatch_mgr_q)) {
					// send kevent must be uninstalled on the manager queue
					needs_mgr = true;
					goto partial_drain;
				}
				if (unlikely(!_dispatch_mach_reconnect_invoke(dm, dc))) {
					disconnecting = true;
					goto partial_drain;
				}
				_dispatch_perfmon_workitem_inc();
				continue;
			}
			_dispatch_voucher_ktrace_dmsg_pop(dmsg);
			if (unlikely(dmsr->dmsr_disconnect_cnt ||
					(dm->dq_atomic_flags & DSF_CANCELED))) {
				_dispatch_mach_msg_not_sent(dm, dmsg);
				_dispatch_perfmon_workitem_inc();
				continue;
			}
			send_status = _dispatch_mach_msg_send(dm, dmsg, dmr, qos, sf);
			if (unlikely(!send_status)) {
				goto partial_drain;
			}
			if (send_status & DM_SEND_STATUS_RETURNING_IMMEDIATE_SEND_RESULT) {
				returning_send_result = true;
			}
			_dispatch_perfmon_workitem_inc();
		} while ((dc = next_dc));
	}

	os_atomic_rmw_loop2o(dmsr, dmsr_state, old_state, new_state, release, {
		if (old_state & DISPATCH_MACH_STATE_DIRTY) {
			new_state = old_state;
			new_state &= ~DISPATCH_MACH_STATE_DIRTY;
			new_state &= ~DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
			new_state &= ~DISPATCH_MACH_STATE_PENDING_BARRIER;
		} else {
			// unlock
			new_state = 0;
		}
	});
	goto out;

partial_drain:
	// if this is not a complete drain, we must undo some things
	_dispatch_mach_send_unpop_head(dmsr, dc, next_dc);

	if (_dispatch_object_has_type(dc,
			DISPATCH_CONTINUATION_TYPE(MACH_SEND_BARRIER))) {
		os_atomic_rmw_loop2o(dmsr, dmsr_state, old_state, new_state, release, {
			new_state = old_state;
			new_state |= DISPATCH_MACH_STATE_DIRTY;
			new_state |= DISPATCH_MACH_STATE_PENDING_BARRIER;
			new_state &= ~DISPATCH_MACH_STATE_UNLOCK_MASK;
			new_state &= ~DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
		});
	} else {
		os_atomic_rmw_loop2o(dmsr, dmsr_state, old_state, new_state, release, {
			new_state = old_state;
			if (old_state & (DISPATCH_MACH_STATE_DIRTY |
					DISPATCH_MACH_STATE_RECEIVED_OVERRIDE)) {
				new_state &= ~DISPATCH_MACH_STATE_DIRTY;
				new_state &= ~DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
				new_state &= ~DISPATCH_MACH_STATE_PENDING_BARRIER;
			} else {
				new_state |= DISPATCH_MACH_STATE_DIRTY;
				new_state &= ~DISPATCH_MACH_STATE_UNLOCK_MASK;
			}
		});
	}

out:
	if (old_state & DISPATCH_MACH_STATE_RECEIVED_OVERRIDE) {
		// Ensure that the root queue sees that this thread was overridden.
		_dispatch_set_basepri_override_qos(_dmsr_state_max_qos(old_state));
	}

	if (unlikely(new_state & DISPATCH_MACH_STATE_UNLOCK_MASK)) {
		qos = _dmsr_state_max_qos(new_state);
		os_atomic_thread_fence(dependency);
		dmsr = os_atomic_force_dependency_on(dmsr, new_state);
		goto again;
	}

	if (new_state & DISPATCH_MACH_STATE_PENDING_BARRIER) {
		qos = _dmsr_state_max_qos(new_state);
		_dispatch_mach_push_send_barrier_drain(dm, qos);
	} else {
		if (needs_mgr || dm->dm_needs_mgr) {
			qos = _dmsr_state_max_qos(new_state);
		} else {
			qos = 0;
		}
		if (!disconnecting) dx_wakeup(dm, qos, DISPATCH_WAKEUP_MAKE_DIRTY);
	}
	return returning_send_result;
}

DISPATCH_NOINLINE
static void
_dispatch_mach_send_invoke(dispatch_mach_t dm, dispatch_invoke_flags_t flags,
		dispatch_mach_send_invoke_flags_t send_flags)
{
	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	dispatch_lock owner_self = _dispatch_lock_value_for_self();
	uint64_t old_state, new_state;

	uint64_t canlock_mask = DISPATCH_MACH_STATE_UNLOCK_MASK;
	uint64_t canlock_state = 0;

	if (send_flags & DM_SEND_INVOKE_NEEDS_BARRIER) {
		canlock_mask |= DISPATCH_MACH_STATE_PENDING_BARRIER;
		canlock_state = DISPATCH_MACH_STATE_PENDING_BARRIER;
	} else if (!(send_flags & DM_SEND_INVOKE_CAN_RUN_BARRIER)) {
		canlock_mask |= DISPATCH_MACH_STATE_PENDING_BARRIER;
	}

	dispatch_qos_t oq_floor = _dispatch_get_basepri_override_qos_floor();
retry:
	os_atomic_rmw_loop2o(dmsr, dmsr_state, old_state, new_state, acquire, {
		new_state = old_state;
		if (unlikely((old_state & canlock_mask) != canlock_state)) {
			if (!(send_flags & DM_SEND_INVOKE_MAKE_DIRTY)) {
				os_atomic_rmw_loop_give_up(break);
			}
			new_state |= DISPATCH_MACH_STATE_DIRTY;
		} else {
			if (_dmsr_state_needs_lock_override(old_state, oq_floor)) {
				os_atomic_rmw_loop_give_up({
					oq_floor = _dispatch_queue_override_self(old_state);
					goto retry;
				});
			}
			new_state |= owner_self;
			new_state &= ~DISPATCH_MACH_STATE_DIRTY;
			new_state &= ~DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
			new_state &= ~DISPATCH_MACH_STATE_PENDING_BARRIER;
		}
	});

	if (unlikely((old_state & canlock_mask) != canlock_state)) {
		return;
	}
	if (send_flags & DM_SEND_INVOKE_CANCEL) {
		_dispatch_mach_cancel(dm);
	}
	_dispatch_mach_send_drain(dm, flags, send_flags);
}

DISPATCH_NOINLINE
void
_dispatch_mach_send_barrier_drain_invoke(dispatch_continuation_t dc,
		DISPATCH_UNUSED dispatch_invoke_context_t dic,
		dispatch_invoke_flags_t flags)
{
	dispatch_mach_t dm = (dispatch_mach_t)_dispatch_queue_get_current();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT;
	dispatch_thread_frame_s dtf;

	DISPATCH_COMPILER_CAN_ASSUME(dc->dc_priority == DISPATCH_NO_PRIORITY);
	DISPATCH_COMPILER_CAN_ASSUME(dc->dc_voucher == DISPATCH_NO_VOUCHER);
	// hide the mach channel (see _dispatch_mach_barrier_invoke comment)
	_dispatch_thread_frame_stash(&dtf);
	_dispatch_continuation_pop_forwarded(dc, DISPATCH_NO_VOUCHER, dc_flags,{
		_dispatch_mach_send_invoke(dm, flags,
				DM_SEND_INVOKE_NEEDS_BARRIER | DM_SEND_INVOKE_CAN_RUN_BARRIER);
	});
	_dispatch_thread_frame_unstash(&dtf);
}

DISPATCH_NOINLINE
static void
_dispatch_mach_push_send_barrier_drain(dispatch_mach_t dm, dispatch_qos_t qos)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();

	dc->do_vtable = DC_VTABLE(MACH_SEND_BARRRIER_DRAIN);
	dc->dc_func = NULL;
	dc->dc_ctxt = NULL;
	dc->dc_voucher = DISPATCH_NO_VOUCHER;
	dc->dc_priority = DISPATCH_NO_PRIORITY;
	dm_push(dm, dc, qos);
}

DISPATCH_NOINLINE
static void
_dispatch_mach_send_push(dispatch_mach_t dm, dispatch_continuation_t dc,
		dispatch_qos_t qos)
{
	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	uint64_t old_state, new_state, state_flags = 0;
	dispatch_tid owner;
	bool wakeup;

	// <rdar://problem/25896179> when pushing a send barrier that destroys
	// the last reference to this channel, and the send queue is already
	// draining on another thread, the send barrier may run as soon as
	// _dispatch_mach_send_push_inline() returns.
	_dispatch_retain_2(dm);

	wakeup = _dispatch_mach_send_push_inline(dmsr, dc);
	if (wakeup) {
		state_flags = DISPATCH_MACH_STATE_DIRTY;
		if (dc->do_vtable == DC_VTABLE(MACH_SEND_BARRIER)) {
			state_flags |= DISPATCH_MACH_STATE_PENDING_BARRIER;
		}
	}

	if (state_flags) {
		os_atomic_rmw_loop2o(dmsr, dmsr_state, old_state, new_state, release, {
			new_state = _dmsr_state_merge_override(old_state, qos);
			new_state |= state_flags;
		});
	} else {
		os_atomic_rmw_loop2o(dmsr, dmsr_state, old_state, new_state, relaxed, {
			new_state = _dmsr_state_merge_override(old_state, qos);
			if (old_state == new_state) {
				os_atomic_rmw_loop_give_up(break);
			}
		});
	}

	qos = _dmsr_state_max_qos(new_state);
	owner = _dispatch_lock_owner((dispatch_lock)old_state);
	if (owner) {
		if (_dmsr_state_needs_override(old_state, qos)) {
			_dispatch_wqthread_override_start_check_owner(owner, qos,
					&dmsr->dmsr_state_lock.dul_lock);
		}
		return _dispatch_release_2_tailcall(dm);
	}

	dispatch_wakeup_flags_t wflags = 0;
	if (state_flags & DISPATCH_MACH_STATE_PENDING_BARRIER) {
		_dispatch_mach_push_send_barrier_drain(dm, qos);
	} else if (wakeup || dmsr->dmsr_disconnect_cnt ||
			(dm->dq_atomic_flags & DSF_CANCELED)) {
		wflags = DISPATCH_WAKEUP_MAKE_DIRTY | DISPATCH_WAKEUP_CONSUME_2;
	} else if (old_state & DISPATCH_MACH_STATE_PENDING_BARRIER) {
		wflags = DISPATCH_WAKEUP_CONSUME_2;
	}
	if (wflags) {
		return dx_wakeup(dm, qos, wflags);
	}
	return _dispatch_release_2_tailcall(dm);
}

DISPATCH_NOINLINE
static bool
_dispatch_mach_send_push_and_trydrain(dispatch_mach_t dm,
		dispatch_object_t dou, dispatch_qos_t qos,
		dispatch_mach_send_invoke_flags_t send_flags)
{
	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	dispatch_lock owner_self = _dispatch_lock_value_for_self();
	uint64_t old_state, new_state, canlock_mask, state_flags = 0;
	dispatch_tid owner;

	bool wakeup = _dispatch_mach_send_push_inline(dmsr, dou);
	if (wakeup) {
		state_flags = DISPATCH_MACH_STATE_DIRTY;
	}

	if (unlikely(dmsr->dmsr_disconnect_cnt ||
			(dm->dq_atomic_flags & DSF_CANCELED))) {
		os_atomic_rmw_loop2o(dmsr, dmsr_state, old_state, new_state, release, {
			new_state = _dmsr_state_merge_override(old_state, qos);
			new_state |= state_flags;
		});
		dx_wakeup(dm, qos, DISPATCH_WAKEUP_MAKE_DIRTY);
		return false;
	}

	canlock_mask = DISPATCH_MACH_STATE_UNLOCK_MASK |
			DISPATCH_MACH_STATE_PENDING_BARRIER;
	if (state_flags) {
		os_atomic_rmw_loop2o(dmsr, dmsr_state, old_state, new_state, seq_cst, {
			new_state = _dmsr_state_merge_override(old_state, qos);
			new_state |= state_flags;
			if (likely((old_state & canlock_mask) == 0)) {
				new_state |= owner_self;
				new_state &= ~DISPATCH_MACH_STATE_DIRTY;
				new_state &= ~DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
				new_state &= ~DISPATCH_MACH_STATE_PENDING_BARRIER;
			}
		});
	} else {
		os_atomic_rmw_loop2o(dmsr, dmsr_state, old_state, new_state, acquire, {
			new_state = _dmsr_state_merge_override(old_state, qos);
			if (new_state == old_state) {
				os_atomic_rmw_loop_give_up(return false);
			}
			if (likely((old_state & canlock_mask) == 0)) {
				new_state |= owner_self;
				new_state &= ~DISPATCH_MACH_STATE_DIRTY;
				new_state &= ~DISPATCH_MACH_STATE_RECEIVED_OVERRIDE;
				new_state &= ~DISPATCH_MACH_STATE_PENDING_BARRIER;
			}
		});
	}

	owner = _dispatch_lock_owner((dispatch_lock)old_state);
	if (owner) {
		if (_dmsr_state_needs_override(old_state, qos)) {
			_dispatch_wqthread_override_start_check_owner(owner, qos,
					&dmsr->dmsr_state_lock.dul_lock);
		}
		return false;
	}

	if (old_state & DISPATCH_MACH_STATE_PENDING_BARRIER) {
		dx_wakeup(dm, qos, 0);
		return false;
	}

	// Ensure our message is still at the head of the queue and has not already
	// been dequeued by another thread that raced us to the send queue lock.
	// A plain load of the head and comparison against our object pointer is
	// sufficient.
	if (unlikely(!(wakeup && dou._do == dmsr->dmsr_head))) {
		// Don't request immediate send result for messages we don't own
		send_flags &= ~DM_SEND_INVOKE_IMMEDIATE_SEND_MASK;
	}
	return _dispatch_mach_send_drain(dm, DISPATCH_INVOKE_NONE, send_flags);
}

#pragma mark -
#pragma mark dispatch_mach

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_mach_notification_kevent_unregister(dispatch_mach_t dm)
{
	DISPATCH_ASSERT_ON_MANAGER_QUEUE();
	if (_dispatch_unote_registered(dm->dm_send_refs)) {
		dispatch_assume(_dispatch_unote_unregister(dm->dm_send_refs, 0));
	}
	dm->dm_send_refs->du_ident = 0;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_mach_notification_kevent_register(dispatch_mach_t dm,mach_port_t send)
{
	DISPATCH_ASSERT_ON_MANAGER_QUEUE();
	dm->dm_send_refs->du_ident = send;
	dispatch_assume(_dispatch_unote_register(dm->dm_send_refs,
			DISPATCH_WLH_ANON, 0));
}

void
_dispatch_mach_merge_notification(dispatch_unote_t du,
		uint32_t flags DISPATCH_UNUSED, uintptr_t data,
		uintptr_t status DISPATCH_UNUSED,
		pthread_priority_t pp DISPATCH_UNUSED)
{
	dispatch_mach_send_refs_t dmsr = du._dmsr;
	dispatch_mach_t dm = _dispatch_wref2ptr(dmsr->du_owner_wref);

	if (data & dmsr->du_fflags) {
		_dispatch_mach_send_invoke(dm, DISPATCH_INVOKE_MANAGER_DRAIN,
				DM_SEND_INVOKE_MAKE_DIRTY);
	}
}

DISPATCH_NOINLINE
static void
_dispatch_mach_handle_or_push_received_msg(dispatch_mach_t dm,
		dispatch_mach_msg_t dmsg)
{
	mach_error_t error;
	dispatch_mach_reason_t reason = _dispatch_mach_msg_get_reason(dmsg, &error);
	if (reason == DISPATCH_MACH_MESSAGE_RECEIVED || !dm->dm_is_xpc ||
			!_dispatch_mach_xpc_hooks->dmxh_direct_message_handler(
			dm->dm_recv_refs->dmrr_handler_ctxt, reason, dmsg, error)) {
		// Not XPC client or not a message that XPC can handle inline - push
		// it onto the channel queue.
		dm_push(dm, dmsg, _dispatch_qos_from_pp(dmsg->dmsg_priority));
	} else {
		// XPC handled the message inline. Do the cleanup that would otherwise
		// have happened in _dispatch_mach_msg_invoke(), leaving out steps that
		// are not required in this context.
		dmsg->do_next = DISPATCH_OBJECT_LISTLESS;
		dispatch_release(dmsg);
	}
}

DISPATCH_ALWAYS_INLINE
static void
_dispatch_mach_push_async_reply_msg(dispatch_mach_t dm,
		dispatch_mach_msg_t dmsg, dispatch_queue_t drq) {
	// Push the message onto the given queue. This function is only used for
	// replies to messages sent by
	// dispatch_mach_send_with_result_and_async_reply_4libxpc().
	dispatch_continuation_t dc = _dispatch_mach_msg_async_reply_wrap(dmsg, dm);
	_dispatch_trace_continuation_push(drq, dc);
	dx_push(drq, dc, _dispatch_qos_from_pp(dmsg->dmsg_priority));
}

#pragma mark -
#pragma mark dispatch_mach_t

static inline mach_msg_option_t
_dispatch_mach_checkin_options(void)
{
	mach_msg_option_t options = 0;
#if DISPATCH_USE_CHECKIN_NOIMPORTANCE
	options = MACH_SEND_NOIMPORTANCE; // <rdar://problem/16996737>
#endif
	return options;
}


static inline mach_msg_option_t
_dispatch_mach_send_options(void)
{
	mach_msg_option_t options = 0;
	return options;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dispatch_mach_priority_propagate(mach_msg_option_t options,
		pthread_priority_t *msg_pp)
{
#if DISPATCH_USE_NOIMPORTANCE_QOS
	if (options & MACH_SEND_NOIMPORTANCE) {
		*msg_pp = 0;
		return 0;
	}
#endif
	unsigned int flags = DISPATCH_PRIORITY_PROPAGATE_CURRENT;
	if ((options & DISPATCH_MACH_WAIT_FOR_REPLY) &&
			(options & DISPATCH_MACH_OWNED_REPLY_PORT) &&
			_dispatch_use_mach_special_reply_port()) {
		flags |= DISPATCH_PRIORITY_PROPAGATE_FOR_SYNC_IPC;
	}
	*msg_pp = _dispatch_priority_compute_propagated(0, flags);
	// TODO: remove QoS contribution of sync IPC messages to send queue
	// rdar://31848737
	return _dispatch_qos_from_pp(*msg_pp);
}

DISPATCH_NOINLINE
static bool
_dispatch_mach_send_msg(dispatch_mach_t dm, dispatch_mach_msg_t dmsg,
		dispatch_continuation_t dc_wait, mach_msg_option_t options)
{
	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	if (slowpath(dmsg->do_next != DISPATCH_OBJECT_LISTLESS)) {
		DISPATCH_CLIENT_CRASH(dmsg->do_next, "Message already enqueued");
	}
	dispatch_retain(dmsg);
	pthread_priority_t msg_pp;
	dispatch_qos_t qos = _dispatch_mach_priority_propagate(options, &msg_pp);
	options |= _dispatch_mach_send_options();
	dmsg->dmsg_options = options;
	mach_msg_header_t *msg = _dispatch_mach_msg_get_msg(dmsg);
	dmsg->dmsg_reply = _dispatch_mach_msg_get_reply_port(dmsg);
	bool is_reply = (MACH_MSGH_BITS_REMOTE(msg->msgh_bits) ==
			MACH_MSG_TYPE_MOVE_SEND_ONCE);
	dmsg->dmsg_priority = msg_pp;
	dmsg->dmsg_voucher = _voucher_copy();
	_dispatch_voucher_debug("mach-msg[%p] set", dmsg->dmsg_voucher, dmsg);

	uint32_t send_status;
	bool returning_send_result = false;
	dispatch_mach_send_invoke_flags_t send_flags = DM_SEND_INVOKE_NONE;
	if (options & DISPATCH_MACH_RETURN_IMMEDIATE_SEND_RESULT) {
		send_flags = DM_SEND_INVOKE_IMMEDIATE_SEND;
	}
	if (is_reply && !dmsg->dmsg_reply && !dmsr->dmsr_disconnect_cnt &&
			!(dm->dq_atomic_flags & DSF_CANCELED)) {
		// replies are sent to a send-once right and don't need the send queue
		dispatch_assert(!dc_wait);
		send_status = _dispatch_mach_msg_send(dm, dmsg, NULL, 0, send_flags);
		dispatch_assert(send_status);
		returning_send_result = !!(send_status &
				DM_SEND_STATUS_RETURNING_IMMEDIATE_SEND_RESULT);
	} else {
		_dispatch_voucher_ktrace_dmsg_push(dmsg);
		dispatch_object_t dou = { ._dmsg = dmsg };
		if (dc_wait) dou._dc = dc_wait;
		returning_send_result = _dispatch_mach_send_push_and_trydrain(dm, dou,
				qos, send_flags);
	}
	if (returning_send_result) {
		_dispatch_voucher_debug("mach-msg[%p] clear", dmsg->dmsg_voucher, dmsg);
		if (dmsg->dmsg_voucher) _voucher_release(dmsg->dmsg_voucher);
		dmsg->dmsg_voucher = NULL;
		dmsg->do_next = DISPATCH_OBJECT_LISTLESS;
		dispatch_release(dmsg);
	}
	return returning_send_result;
}

DISPATCH_NOINLINE
void
dispatch_mach_send(dispatch_mach_t dm, dispatch_mach_msg_t dmsg,
		mach_msg_option_t options)
{
	dispatch_assert_zero(options & DISPATCH_MACH_OPTIONS_MASK);
	options &= ~DISPATCH_MACH_OPTIONS_MASK;
	bool returned_send_result = _dispatch_mach_send_msg(dm, dmsg, NULL,options);
	dispatch_assert(!returned_send_result);
}

DISPATCH_NOINLINE
void
dispatch_mach_send_with_result(dispatch_mach_t dm, dispatch_mach_msg_t dmsg,
		mach_msg_option_t options, dispatch_mach_send_flags_t send_flags,
		dispatch_mach_reason_t *send_result, mach_error_t *send_error)
{
	if (unlikely(send_flags != DISPATCH_MACH_SEND_DEFAULT)) {
		DISPATCH_CLIENT_CRASH(send_flags, "Invalid send flags");
	}
	dispatch_assert_zero(options & DISPATCH_MACH_OPTIONS_MASK);
	options &= ~DISPATCH_MACH_OPTIONS_MASK;
	options |= DISPATCH_MACH_RETURN_IMMEDIATE_SEND_RESULT;
	bool returned_send_result = _dispatch_mach_send_msg(dm, dmsg, NULL,options);
	unsigned long reason = DISPATCH_MACH_NEEDS_DEFERRED_SEND;
	mach_error_t err = 0;
	if (returned_send_result) {
		reason = _dispatch_mach_msg_get_reason(dmsg, &err);
	}
	*send_result = reason;
	*send_error = err;
}

static inline
dispatch_mach_msg_t
_dispatch_mach_send_and_wait_for_reply(dispatch_mach_t dm,
		dispatch_mach_msg_t dmsg, mach_msg_option_t options,
		bool *returned_send_result)
{
	mach_port_t send = MACH_PORT_NULL;
	mach_port_t reply_port = _dispatch_mach_msg_get_reply_port(dmsg);
	if (!reply_port) {
		// use per-thread mach reply port <rdar://24597802>
		reply_port = _dispatch_get_thread_reply_port();
		mach_msg_header_t *hdr = _dispatch_mach_msg_get_msg(dmsg);
		dispatch_assert(MACH_MSGH_BITS_LOCAL(hdr->msgh_bits) ==
				MACH_MSG_TYPE_MAKE_SEND_ONCE);
		hdr->msgh_local_port = reply_port;
		options |= DISPATCH_MACH_OWNED_REPLY_PORT;
	}
	options |= DISPATCH_MACH_WAIT_FOR_REPLY;

	dispatch_mach_reply_refs_t dmr;
#if DISPATCH_DEBUG
	dmr = _dispatch_calloc(1, sizeof(*dmr));
#else
	struct dispatch_mach_reply_refs_s dmr_buf = { };
	dmr = &dmr_buf;
#endif
	struct dispatch_continuation_s dc_wait = {
		.dc_flags = DISPATCH_OBJ_SYNC_WAITER_BIT,
		.dc_data = dmsg,
		.dc_other = dmr,
		.dc_priority = DISPATCH_NO_PRIORITY,
		.dc_voucher = DISPATCH_NO_VOUCHER,
	};
	dmr->dmr_ctxt = dmsg->do_ctxt;
	dmr->dmr_waiter_tid = _dispatch_tid_self();
	*returned_send_result = _dispatch_mach_send_msg(dm, dmsg, &dc_wait,options);
	if (options & DISPATCH_MACH_OWNED_REPLY_PORT) {
		_dispatch_clear_thread_reply_port(reply_port);
		if (_dispatch_use_mach_special_reply_port()) {
			// link special reply port to send right for remote receive right
			// TODO: extend to pre-connect phase <rdar://problem/31823384>
			send = dm->dm_send_refs->dmsr_send;
		}
	}
	dmsg = _dispatch_mach_msg_reply_recv(dm, dmr, reply_port, send);
#if DISPATCH_DEBUG
	free(dmr);
#endif
	return dmsg;
}

DISPATCH_NOINLINE
dispatch_mach_msg_t
dispatch_mach_send_and_wait_for_reply(dispatch_mach_t dm,
		dispatch_mach_msg_t dmsg, mach_msg_option_t options)
{
	bool returned_send_result;
	dispatch_mach_msg_t reply;
	dispatch_assert_zero(options & DISPATCH_MACH_OPTIONS_MASK);
	options &= ~DISPATCH_MACH_OPTIONS_MASK;
	reply = _dispatch_mach_send_and_wait_for_reply(dm, dmsg, options,
			&returned_send_result);
	dispatch_assert(!returned_send_result);
	return reply;
}

DISPATCH_NOINLINE
dispatch_mach_msg_t
dispatch_mach_send_with_result_and_wait_for_reply(dispatch_mach_t dm,
		dispatch_mach_msg_t dmsg, mach_msg_option_t options,
		dispatch_mach_send_flags_t send_flags,
		dispatch_mach_reason_t *send_result, mach_error_t *send_error)
{
	if (unlikely(send_flags != DISPATCH_MACH_SEND_DEFAULT)) {
		DISPATCH_CLIENT_CRASH(send_flags, "Invalid send flags");
	}
	bool returned_send_result;
	dispatch_mach_msg_t reply;
	dispatch_assert_zero(options & DISPATCH_MACH_OPTIONS_MASK);
	options &= ~DISPATCH_MACH_OPTIONS_MASK;
	options |= DISPATCH_MACH_RETURN_IMMEDIATE_SEND_RESULT;
	reply = _dispatch_mach_send_and_wait_for_reply(dm, dmsg, options,
			&returned_send_result);
	unsigned long reason = DISPATCH_MACH_NEEDS_DEFERRED_SEND;
	mach_error_t err = 0;
	if (returned_send_result) {
		reason = _dispatch_mach_msg_get_reason(dmsg, &err);
	}
	*send_result = reason;
	*send_error = err;
	return reply;
}

DISPATCH_NOINLINE
void
dispatch_mach_send_with_result_and_async_reply_4libxpc(dispatch_mach_t dm,
		dispatch_mach_msg_t dmsg, mach_msg_option_t options,
		dispatch_mach_send_flags_t send_flags,
		dispatch_mach_reason_t *send_result, mach_error_t *send_error)
{
	if (unlikely(send_flags != DISPATCH_MACH_SEND_DEFAULT)) {
		DISPATCH_CLIENT_CRASH(send_flags, "Invalid send flags");
	}
	if (unlikely(!dm->dm_is_xpc)) {
		DISPATCH_CLIENT_CRASH(0,
			"dispatch_mach_send_with_result_and_wait_for_reply is XPC only");
	}

	dispatch_assert_zero(options & DISPATCH_MACH_OPTIONS_MASK);
	options &= ~DISPATCH_MACH_OPTIONS_MASK;
	options |= DISPATCH_MACH_RETURN_IMMEDIATE_SEND_RESULT;
	mach_port_t reply_port = _dispatch_mach_msg_get_reply_port(dmsg);
	if (!reply_port) {
		DISPATCH_CLIENT_CRASH(0, "Reply port needed for async send with reply");
	}
	options |= DISPATCH_MACH_ASYNC_REPLY;
	bool returned_send_result = _dispatch_mach_send_msg(dm, dmsg, NULL,options);
	unsigned long reason = DISPATCH_MACH_NEEDS_DEFERRED_SEND;
	mach_error_t err = 0;
	if (returned_send_result) {
		reason = _dispatch_mach_msg_get_reason(dmsg, &err);
	}
	*send_result = reason;
	*send_error = err;
}

DISPATCH_NOINLINE
static bool
_dispatch_mach_disconnect(dispatch_mach_t dm)
{
	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	bool disconnected;
	if (_dispatch_unote_registered(dmsr)) {
		_dispatch_mach_notification_kevent_unregister(dm);
	}
	if (MACH_PORT_VALID(dmsr->dmsr_send)) {
		_dispatch_mach_msg_disconnected(dm, MACH_PORT_NULL, dmsr->dmsr_send);
		dmsr->dmsr_send = MACH_PORT_NULL;
	}
	if (dmsr->dmsr_checkin) {
		_dispatch_mach_msg_not_sent(dm, dmsr->dmsr_checkin);
		dmsr->dmsr_checkin = NULL;
	}
	_dispatch_unfair_lock_lock(&dm->dm_send_refs->dmsr_replies_lock);
	dispatch_mach_reply_refs_t dmr, tmp;
	TAILQ_FOREACH_SAFE(dmr, &dm->dm_send_refs->dmsr_replies, dmr_list, tmp) {
		TAILQ_REMOVE(&dm->dm_send_refs->dmsr_replies, dmr, dmr_list);
		_TAILQ_MARK_NOT_ENQUEUED(dmr, dmr_list);
		if (_dispatch_unote_registered(dmr)) {
			if (!_dispatch_mach_reply_kevent_unregister(dm, dmr,
					DU_UNREGISTER_DISCONNECTED)) {
				TAILQ_INSERT_HEAD(&dm->dm_send_refs->dmsr_replies, dmr,
					dmr_list);
			}
		} else {
			_dispatch_mach_reply_waiter_unregister(dm, dmr,
				DU_UNREGISTER_DISCONNECTED);
		}
	}
	disconnected = TAILQ_EMPTY(&dm->dm_send_refs->dmsr_replies);
	_dispatch_unfair_lock_unlock(&dm->dm_send_refs->dmsr_replies_lock);
	return disconnected;
}

static void
_dispatch_mach_cancel(dispatch_mach_t dm)
{
	_dispatch_object_debug(dm, "%s", __func__);
	if (!_dispatch_mach_disconnect(dm)) return;

	bool uninstalled = true;
	dispatch_assert(!dm->dm_uninstalled);

	if (dm->dm_xpc_term_refs) {
		uninstalled = _dispatch_unote_unregister(dm->dm_xpc_term_refs, 0);
	}

	dispatch_mach_recv_refs_t dmrr = dm->dm_recv_refs;
	mach_port_t local_port = (mach_port_t)dmrr->du_ident;
	if (local_port) {
		// handle the deferred delete case properly, similar to what
		// _dispatch_source_invoke2() does
		dispatch_queue_flags_t dqf = _dispatch_queue_atomic_flags(dm->_as_dq);
		if ((dqf & DSF_DEFERRED_DELETE) && !(dqf & DSF_ARMED)) {
			_dispatch_source_refs_unregister(dm->_as_ds,
					DU_UNREGISTER_IMMEDIATE_DELETE);
			dqf = _dispatch_queue_atomic_flags(dm->_as_dq);
		} else if (!(dqf & DSF_DEFERRED_DELETE) && !(dqf & DSF_DELETED)) {
			_dispatch_source_refs_unregister(dm->_as_ds, 0);
			dqf = _dispatch_queue_atomic_flags(dm->_as_dq);
		}
		if ((dqf & DSF_STATE_MASK) == DSF_DELETED) {
			_dispatch_mach_msg_disconnected(dm, local_port, MACH_PORT_NULL);
			dmrr->du_ident = 0;
		} else {
			uninstalled = false;
		}
	} else {
		_dispatch_queue_atomic_flags_set_and_clear(dm->_as_dq, DSF_DELETED,
				DSF_ARMED | DSF_DEFERRED_DELETE);
	}

	if (dm->dm_send_refs->dmsr_disconnect_cnt) {
		uninstalled = false; // <rdar://problem/31233110>
	}
	if (uninstalled) dm->dm_uninstalled = uninstalled;
}

DISPATCH_NOINLINE
static bool
_dispatch_mach_reconnect_invoke(dispatch_mach_t dm, dispatch_object_t dou)
{
	if (!_dispatch_mach_disconnect(dm)) return false;
	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	dmsr->dmsr_checkin = dou._dc->dc_data;
	dmsr->dmsr_send = (mach_port_t)dou._dc->dc_other;
	_dispatch_continuation_free(dou._dc);
	(void)os_atomic_dec2o(dmsr, dmsr_disconnect_cnt, relaxed);
	_dispatch_object_debug(dm, "%s", __func__);
	_dispatch_release(dm); // <rdar://problem/26266265>
	return true;
}

DISPATCH_NOINLINE
void
dispatch_mach_reconnect(dispatch_mach_t dm, mach_port_t send,
		dispatch_mach_msg_t checkin)
{
	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	(void)os_atomic_inc2o(dmsr, dmsr_disconnect_cnt, relaxed);
	if (MACH_PORT_VALID(send) && checkin) {
		dispatch_mach_msg_t dmsg = checkin;
		dispatch_retain(dmsg);
		dmsg->dmsg_options = _dispatch_mach_checkin_options();
		dmsr->dmsr_checkin_port = _dispatch_mach_msg_get_remote_port(dmsg);
	} else {
		checkin = NULL;
		dmsr->dmsr_checkin_port = MACH_PORT_NULL;
	}
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	dc->dc_flags = DISPATCH_OBJ_CONSUME_BIT;
	// actually called manually in _dispatch_mach_send_drain
	dc->dc_func = (void*)_dispatch_mach_reconnect_invoke;
	dc->dc_ctxt = dc;
	dc->dc_data = checkin;
	dc->dc_other = (void*)(uintptr_t)send;
	dc->dc_voucher = DISPATCH_NO_VOUCHER;
	dc->dc_priority = DISPATCH_NO_PRIORITY;
	_dispatch_retain(dm); // <rdar://problem/26266265>
	return _dispatch_mach_send_push(dm, dc, 0);
}

DISPATCH_NOINLINE
mach_port_t
dispatch_mach_get_checkin_port(dispatch_mach_t dm)
{
	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	if (slowpath(dm->dq_atomic_flags & DSF_CANCELED)) {
		return MACH_PORT_DEAD;
	}
	return dmsr->dmsr_checkin_port;
}

DISPATCH_NOINLINE
static void
_dispatch_mach_connect_invoke(dispatch_mach_t dm)
{
	dispatch_mach_recv_refs_t dmrr = dm->dm_recv_refs;
	_dispatch_client_callout4(dmrr->dmrr_handler_ctxt,
			DISPATCH_MACH_CONNECTED, NULL, 0, dmrr->dmrr_handler_func);
	dm->dm_connect_handler_called = 1;
	_dispatch_perfmon_workitem_inc();
}

DISPATCH_ALWAYS_INLINE
static void
_dispatch_mach_msg_invoke_with_mach(dispatch_mach_msg_t dmsg,
		dispatch_invoke_flags_t flags, dispatch_mach_t dm)
{
	dispatch_mach_recv_refs_t dmrr;
	mach_error_t err;
	unsigned long reason = _dispatch_mach_msg_get_reason(dmsg, &err);
	dispatch_thread_set_self_t adopt_flags = DISPATCH_PRIORITY_ENFORCE|
			DISPATCH_VOUCHER_CONSUME|DISPATCH_VOUCHER_REPLACE;

	dmrr = dm->dm_recv_refs;
	dmsg->do_next = DISPATCH_OBJECT_LISTLESS;
	_dispatch_voucher_ktrace_dmsg_pop(dmsg);
	_dispatch_voucher_debug("mach-msg[%p] adopt", dmsg->dmsg_voucher, dmsg);
	(void)_dispatch_adopt_priority_and_set_voucher(dmsg->dmsg_priority,
			dmsg->dmsg_voucher, adopt_flags);
	dmsg->dmsg_voucher = NULL;
	dispatch_invoke_with_autoreleasepool(flags, {
		if (flags & DISPATCH_INVOKE_ASYNC_REPLY) {
			_dispatch_client_callout3(dmrr->dmrr_handler_ctxt, reason, dmsg,
					_dispatch_mach_xpc_hooks->dmxh_async_reply_handler);
		} else {
			if (slowpath(!dm->dm_connect_handler_called)) {
				_dispatch_mach_connect_invoke(dm);
			}
			if (reason == DISPATCH_MACH_MESSAGE_RECEIVED &&
					(_dispatch_queue_atomic_flags(dm->_as_dq) & DSF_CANCELED)) {
				// <rdar://problem/32184699> Do not deliver message received
				// after cancellation: _dispatch_mach_merge_msg can be preempted
				// for a long time between clearing DSF_ARMED but before
				// enqueuing the message, allowing for cancellation to complete,
				// and then the message event to be delivered.
				//
				// This makes XPC unhappy because some of these messages are
				// port-destroyed notifications that can cause it to try to
				// reconnect on a channel that is almost fully canceled
			} else {
				_dispatch_client_callout4(dmrr->dmrr_handler_ctxt, reason, dmsg,
						err, dmrr->dmrr_handler_func);
			}
		}
		_dispatch_perfmon_workitem_inc();
	});
	_dispatch_introspection_queue_item_complete(dmsg);
	dispatch_release(dmsg);
}

DISPATCH_NOINLINE
void
_dispatch_mach_msg_invoke(dispatch_mach_msg_t dmsg,
		DISPATCH_UNUSED dispatch_invoke_context_t dic,
		dispatch_invoke_flags_t flags)
{
	dispatch_thread_frame_s dtf;

	// hide mach channel
	dispatch_mach_t dm = (dispatch_mach_t)_dispatch_thread_frame_stash(&dtf);
	_dispatch_mach_msg_invoke_with_mach(dmsg, flags, dm);
	_dispatch_thread_frame_unstash(&dtf);
}

DISPATCH_NOINLINE
void
_dispatch_mach_barrier_invoke(dispatch_continuation_t dc,
		DISPATCH_UNUSED dispatch_invoke_context_t dic,
		dispatch_invoke_flags_t flags)
{
	dispatch_thread_frame_s dtf;
	dispatch_mach_t dm = dc->dc_other;
	dispatch_mach_recv_refs_t dmrr;
	uintptr_t dc_flags = (uintptr_t)dc->dc_data;
	unsigned long type = dc_type(dc);

	// hide mach channel from clients
	if (type == DISPATCH_CONTINUATION_TYPE(MACH_RECV_BARRIER)) {
		// on the send queue, the mach channel isn't the current queue
		// its target queue is the current one already
		_dispatch_thread_frame_stash(&dtf);
	}
	dmrr = dm->dm_recv_refs;
	DISPATCH_COMPILER_CAN_ASSUME(dc_flags & DISPATCH_OBJ_CONSUME_BIT);
	_dispatch_continuation_pop_forwarded(dc, DISPATCH_NO_VOUCHER, dc_flags, {
		dispatch_invoke_with_autoreleasepool(flags, {
			if (slowpath(!dm->dm_connect_handler_called)) {
				_dispatch_mach_connect_invoke(dm);
			}
			_dispatch_client_callout(dc->dc_ctxt, dc->dc_func);
			_dispatch_client_callout4(dmrr->dmrr_handler_ctxt,
					DISPATCH_MACH_BARRIER_COMPLETED, NULL, 0,
					dmrr->dmrr_handler_func);
		});
	});
	if (type == DISPATCH_CONTINUATION_TYPE(MACH_RECV_BARRIER)) {
		_dispatch_thread_frame_unstash(&dtf);
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_mach_barrier_set_vtable(dispatch_continuation_t dc,
		dispatch_mach_t dm, dispatch_continuation_vtable_t vtable)
{
	dc->dc_data = (void *)dc->dc_flags;
	dc->dc_other = dm;
	dc->do_vtable = vtable; // Must be after dc_flags load, dc_vtable aliases
}

DISPATCH_NOINLINE
void
dispatch_mach_send_barrier_f(dispatch_mach_t dm, void *context,
		dispatch_function_t func)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT | DISPATCH_OBJ_MACH_BARRIER;
	dispatch_qos_t qos;

	_dispatch_continuation_init_f(dc, dm, context, func, 0, 0, dc_flags);
	_dispatch_mach_barrier_set_vtable(dc, dm, DC_VTABLE(MACH_SEND_BARRIER));
	_dispatch_trace_continuation_push(dm->_as_dq, dc);
	qos = _dispatch_continuation_override_qos(dm->_as_dq, dc);
	return _dispatch_mach_send_push(dm, dc, qos);
}

DISPATCH_NOINLINE
void
dispatch_mach_send_barrier(dispatch_mach_t dm, dispatch_block_t barrier)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT | DISPATCH_OBJ_MACH_BARRIER;
	dispatch_qos_t qos;

	_dispatch_continuation_init(dc, dm, barrier, 0, 0, dc_flags);
	_dispatch_mach_barrier_set_vtable(dc, dm, DC_VTABLE(MACH_SEND_BARRIER));
	_dispatch_trace_continuation_push(dm->_as_dq, dc);
	qos = _dispatch_continuation_override_qos(dm->_as_dq, dc);
	return _dispatch_mach_send_push(dm, dc, qos);
}

DISPATCH_NOINLINE
void
dispatch_mach_receive_barrier_f(dispatch_mach_t dm, void *context,
		dispatch_function_t func)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT | DISPATCH_OBJ_MACH_BARRIER;

	_dispatch_continuation_init_f(dc, dm, context, func, 0, 0, dc_flags);
	_dispatch_mach_barrier_set_vtable(dc, dm, DC_VTABLE(MACH_RECV_BARRIER));
	return _dispatch_continuation_async(dm->_as_dq, dc);
}

DISPATCH_NOINLINE
void
dispatch_mach_receive_barrier(dispatch_mach_t dm, dispatch_block_t barrier)
{
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	uintptr_t dc_flags = DISPATCH_OBJ_CONSUME_BIT | DISPATCH_OBJ_MACH_BARRIER;

	_dispatch_continuation_init(dc, dm, barrier, 0, 0, dc_flags);
	_dispatch_mach_barrier_set_vtable(dc, dm, DC_VTABLE(MACH_RECV_BARRIER));
	return _dispatch_continuation_async(dm->_as_dq, dc);
}

DISPATCH_NOINLINE
static void
_dispatch_mach_cancel_invoke(dispatch_mach_t dm, dispatch_invoke_flags_t flags)
{
	dispatch_mach_recv_refs_t dmrr = dm->dm_recv_refs;

	dispatch_invoke_with_autoreleasepool(flags, {
		if (slowpath(!dm->dm_connect_handler_called)) {
			_dispatch_mach_connect_invoke(dm);
		}
		_dispatch_client_callout4(dmrr->dmrr_handler_ctxt,
				DISPATCH_MACH_CANCELED, NULL, 0, dmrr->dmrr_handler_func);
		_dispatch_perfmon_workitem_inc();
	});
	dm->dm_cancel_handler_called = 1;
	_dispatch_release(dm); // the retain is done at creation time
}

DISPATCH_NOINLINE
void
dispatch_mach_cancel(dispatch_mach_t dm)
{
	dispatch_source_cancel(dm->_as_ds);
}

static void
_dispatch_mach_install(dispatch_mach_t dm, dispatch_wlh_t wlh,
		dispatch_priority_t pri)
{
	dispatch_mach_recv_refs_t dmrr = dm->dm_recv_refs;
	uint32_t disconnect_cnt;

	if (dmrr->du_ident) {
		_dispatch_source_refs_register(dm->_as_ds, wlh, pri);
		dispatch_assert(dmrr->du_is_direct);
	}

	if (dm->dm_is_xpc) {
		bool monitor_sigterm;
		if (_dispatch_mach_xpc_hooks->version < 3) {
			monitor_sigterm = true;
		} else if (!_dispatch_mach_xpc_hooks->dmxh_enable_sigterm_notification){
			monitor_sigterm = true;
		} else {
			monitor_sigterm =
					_dispatch_mach_xpc_hooks->dmxh_enable_sigterm_notification(
					dm->dm_recv_refs->dmrr_handler_ctxt);
		}
		if (monitor_sigterm) {
			dispatch_xpc_term_refs_t _dxtr =
					dux_create(&_dispatch_xpc_type_sigterm, SIGTERM, 0)._dxtr;
			_dxtr->du_owner_wref = _dispatch_ptr2wref(dm);
			dm->dm_xpc_term_refs = _dxtr;
			_dispatch_unote_register(dm->dm_xpc_term_refs, wlh, pri);
		}
	}
	if (!dm->dq_priority) {
		// _dispatch_mach_reply_kevent_register assumes this has been done
		// which is unlike regular sources or queues, the DEFAULTQUEUE flag
		// is used so that the priority of the channel doesn't act as
		// a QoS floor for incoming messages (26761457)
		dm->dq_priority = pri;
	}
	dm->ds_is_installed = true;
	if (unlikely(!os_atomic_cmpxchgv2o(dm->dm_send_refs, dmsr_disconnect_cnt,
			DISPATCH_MACH_NEVER_INSTALLED, 0, &disconnect_cnt, release))) {
		DISPATCH_INTERNAL_CRASH(disconnect_cnt, "Channel already installed");
	}
}

void
_dispatch_mach_finalize_activation(dispatch_mach_t dm, bool *allow_resume)
{
	dispatch_priority_t pri;
	dispatch_wlh_t wlh;

	// call "super"
	_dispatch_queue_finalize_activation(dm->_as_dq, allow_resume);

	if (!dm->ds_is_installed) {
		pri = _dispatch_queue_compute_priority_and_wlh(dm->_as_dq, &wlh);
		if (pri) _dispatch_mach_install(dm, wlh, pri);
	}
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_mach_tryarm(dispatch_mach_t dm, dispatch_queue_flags_t *out_dqf)
{
	dispatch_queue_flags_t oqf, nqf;
	bool rc = os_atomic_rmw_loop2o(dm, dq_atomic_flags, oqf, nqf, relaxed, {
		nqf = oqf;
		if (nqf & (DSF_ARMED | DSF_CANCELED | DSF_DEFERRED_DELETE |
				DSF_DELETED)) {
			// the test is inside the loop because it's convenient but the
			// result should not change for the duration of the rmw_loop
			os_atomic_rmw_loop_give_up(break);
		}
		nqf |= DSF_ARMED;
	});
	if (out_dqf) *out_dqf = nqf;
	return rc;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_queue_wakeup_target_t
_dispatch_mach_invoke2(dispatch_object_t dou,
		dispatch_invoke_context_t dic, dispatch_invoke_flags_t flags,
		uint64_t *owned)
{
	dispatch_mach_t dm = dou._dm;
	dispatch_queue_wakeup_target_t retq = NULL;
	dispatch_queue_t dq = _dispatch_queue_get_current();
	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	dispatch_mach_recv_refs_t dmrr = dm->dm_recv_refs;
	dispatch_queue_flags_t dqf = 0;

	if (!(flags & DISPATCH_INVOKE_MANAGER_DRAIN) && dmrr &&
			_dispatch_unote_wlh_changed(dmrr, _dispatch_get_wlh())) {
		dqf = _dispatch_queue_atomic_flags_set_orig(dm->_as_dq,
				DSF_WLH_CHANGED);
		if (!(dqf & DSF_WLH_CHANGED)) {
			if (dm->dm_is_xpc) {
				_dispatch_bug_deprecated("Changing target queue "
						"hierarchy after xpc connection was activated");
			} else {
				_dispatch_bug_deprecated("Changing target queue "
						"hierarchy after mach channel was activated");
			}
		}
	}

	// This function performs all mach channel actions. Each action is
	// responsible for verifying that it takes place on the appropriate queue.
	// If the current queue is not the correct queue for this action, the
	// correct queue will be returned and the invoke will be re-driven on that
	// queue.

	// The order of tests here in invoke and in wakeup should be consistent.

	if (unlikely(!dm->ds_is_installed)) {
		// The channel needs to be installed on the kevent queue.
		if (unlikely(flags & DISPATCH_INVOKE_MANAGER_DRAIN)) {
			return dm->do_targetq;
		}
		_dispatch_mach_install(dm, _dispatch_get_wlh(),_dispatch_get_basepri());
		_dispatch_perfmon_workitem_inc();
	}

	if (_dispatch_queue_class_probe(dm)) {
		if (dq == dm->do_targetq) {
drain:
			retq = _dispatch_queue_serial_drain(dm->_as_dq, dic, flags, owned);
		} else {
			retq = dm->do_targetq;
		}
	}

	if (!retq && _dispatch_unote_registered(dmrr)) {
		if (_dispatch_mach_tryarm(dm, &dqf)) {
			_dispatch_unote_resume(dmrr);
			if (dq == dm->do_targetq && !dq->do_targetq && !dmsr->dmsr_tail &&
					(dq->dq_priority & DISPATCH_PRIORITY_FLAG_OVERCOMMIT) &&
					_dispatch_wlh_should_poll_unote(dmrr)) {
				// try to redrive the drain from under the lock for channels
				// targeting an overcommit root queue to avoid parking
				// when the next message has already fired
				_dispatch_event_loop_drain(KEVENT_FLAG_IMMEDIATE);
				if (dm->dq_items_tail) goto drain;
			}
		}
	} else {
		dqf = _dispatch_queue_atomic_flags(dm->_as_dq);
	}

	if (dmsr->dmsr_tail) {
		bool requires_mgr = dm->dm_needs_mgr || (dmsr->dmsr_disconnect_cnt &&
				_dispatch_unote_registered(dmsr));
		if (!os_atomic_load2o(dmsr, dmsr_notification_armed, relaxed) ||
				(dqf & DSF_CANCELED) || dmsr->dmsr_disconnect_cnt) {
			// The channel has pending messages to send.
			if (unlikely(requires_mgr && dq != &_dispatch_mgr_q)) {
				return retq ? retq : &_dispatch_mgr_q;
			}
			dispatch_mach_send_invoke_flags_t send_flags = DM_SEND_INVOKE_NONE;
			if (dq != &_dispatch_mgr_q) {
				send_flags |= DM_SEND_INVOKE_CAN_RUN_BARRIER;
			}
			_dispatch_mach_send_invoke(dm, flags, send_flags);
		}
		if (!retq) retq = DISPATCH_QUEUE_WAKEUP_WAIT_FOR_EVENT;
	} else if (!retq && (dqf & DSF_CANCELED)) {
		// The channel has been cancelled and needs to be uninstalled from the
		// manager queue. After uninstallation, the cancellation handler needs
		// to be delivered to the target queue.
		if (!dm->dm_uninstalled) {
			if ((dqf & DSF_STATE_MASK) == (DSF_ARMED | DSF_DEFERRED_DELETE)) {
				// waiting for the delivery of a deferred delete event
				return retq ? retq : DISPATCH_QUEUE_WAKEUP_WAIT_FOR_EVENT;
			}
			if (dq != &_dispatch_mgr_q) {
				return retq ? retq : &_dispatch_mgr_q;
			}
			_dispatch_mach_send_invoke(dm, flags, DM_SEND_INVOKE_CANCEL);
			if (unlikely(!dm->dm_uninstalled)) {
				// waiting for the delivery of a deferred delete event
				// or deletion didn't happen because send_invoke couldn't
				// acquire the send lock
				return retq ? retq : DISPATCH_QUEUE_WAKEUP_WAIT_FOR_EVENT;
			}
		}
		if (!dm->dm_cancel_handler_called) {
			if (dq != dm->do_targetq) {
				return retq ? retq : dm->do_targetq;
			}
			_dispatch_mach_cancel_invoke(dm, flags);
		}
	}

	return retq;
}

DISPATCH_NOINLINE
void
_dispatch_mach_invoke(dispatch_mach_t dm,
		dispatch_invoke_context_t dic, dispatch_invoke_flags_t flags)
{
	_dispatch_queue_class_invoke(dm, dic, flags,
			DISPATCH_INVOKE_DISALLOW_SYNC_WAITERS, _dispatch_mach_invoke2);
}

void
_dispatch_mach_wakeup(dispatch_mach_t dm, dispatch_qos_t qos,
		dispatch_wakeup_flags_t flags)
{
	// This function determines whether the mach channel needs to be invoked.
	// The order of tests here in probe and in invoke should be consistent.

	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	dispatch_queue_wakeup_target_t tq = DISPATCH_QUEUE_WAKEUP_NONE;
	dispatch_queue_flags_t dqf = _dispatch_queue_atomic_flags(dm->_as_dq);

	if (!dm->ds_is_installed) {
		// The channel needs to be installed on the kevent queue.
		tq = DISPATCH_QUEUE_WAKEUP_TARGET;
		goto done;
	}

	if (_dispatch_queue_class_probe(dm)) {
		tq = DISPATCH_QUEUE_WAKEUP_TARGET;
		goto done;
	}

	if (_dispatch_lock_is_locked(dmsr->dmsr_state_lock.dul_lock)) {
		// Sending and uninstallation below require the send lock, the channel
		// will be woken up when the lock is dropped <rdar://15132939&15203957>
		goto done;
	}

	if (dmsr->dmsr_tail) {
		bool requires_mgr = dm->dm_needs_mgr || (dmsr->dmsr_disconnect_cnt &&
				_dispatch_unote_registered(dmsr));
		if (!os_atomic_load2o(dmsr, dmsr_notification_armed, relaxed) ||
				(dqf & DSF_CANCELED) || dmsr->dmsr_disconnect_cnt) {
			if (unlikely(requires_mgr)) {
				tq = DISPATCH_QUEUE_WAKEUP_MGR;
			} else {
				tq = DISPATCH_QUEUE_WAKEUP_TARGET;
			}
		}
	} else if (dqf & DSF_CANCELED) {
		if (!dm->dm_uninstalled) {
			if ((dqf & DSF_STATE_MASK) == (DSF_ARMED | DSF_DEFERRED_DELETE)) {
				// waiting for the delivery of a deferred delete event
			} else {
				// The channel needs to be uninstalled from the manager queue
				tq = DISPATCH_QUEUE_WAKEUP_MGR;
			}
		} else if (!dm->dm_cancel_handler_called) {
			// the cancellation handler needs to be delivered to the target
			// queue.
			tq = DISPATCH_QUEUE_WAKEUP_TARGET;
		}
	}

done:
	if ((tq == DISPATCH_QUEUE_WAKEUP_TARGET) &&
			dm->do_targetq == &_dispatch_mgr_q) {
		tq = DISPATCH_QUEUE_WAKEUP_MGR;
	}

	return _dispatch_queue_class_wakeup(dm->_as_dq, qos, flags, tq);
}

static void
_dispatch_mach_sigterm_invoke(void *ctx)
{
	dispatch_mach_t dm = ctx;
	if (!(dm->dq_atomic_flags & DSF_CANCELED)) {
		dispatch_mach_recv_refs_t dmrr = dm->dm_recv_refs;
		_dispatch_client_callout4(dmrr->dmrr_handler_ctxt,
				DISPATCH_MACH_SIGTERM_RECEIVED, NULL, 0,
				dmrr->dmrr_handler_func);
	}
}

void
_dispatch_xpc_sigterm_merge(dispatch_unote_t du,
		uint32_t flags DISPATCH_UNUSED, uintptr_t data DISPATCH_UNUSED,
		uintptr_t status DISPATCH_UNUSED, pthread_priority_t pp)
{
	dispatch_mach_t dm = _dispatch_wref2ptr(du._du->du_owner_wref);
	uint32_t options = 0;
	if ((flags & EV_UDATA_SPECIFIC) && (flags & EV_ONESHOT) &&
			!(flags & EV_DELETE)) {
		options = DU_UNREGISTER_IMMEDIATE_DELETE;
	} else {
		dispatch_assert((flags & EV_ONESHOT) && (flags & EV_DELETE));
		options = DU_UNREGISTER_ALREADY_DELETED;
	}
	_dispatch_unote_unregister(du, options);

	if (!(dm->dq_atomic_flags & DSF_CANCELED)) {
		_dispatch_barrier_async_detached_f(dm->_as_dq, dm,
				_dispatch_mach_sigterm_invoke);
	} else {
		dx_wakeup(dm, _dispatch_qos_from_pp(pp), DISPATCH_WAKEUP_MAKE_DIRTY);
	}
}

#pragma mark -
#pragma mark dispatch_mach_msg_t

dispatch_mach_msg_t
dispatch_mach_msg_create(mach_msg_header_t *msg, size_t size,
		dispatch_mach_msg_destructor_t destructor, mach_msg_header_t **msg_ptr)
{
	if (slowpath(size < sizeof(mach_msg_header_t)) ||
			slowpath(destructor && !msg)) {
		DISPATCH_CLIENT_CRASH(size, "Empty message");
	}

	dispatch_mach_msg_t dmsg;
	size_t msg_size = sizeof(struct dispatch_mach_msg_s);
	if (!destructor && os_add_overflow(msg_size,
			  (size - sizeof(dmsg->dmsg_msg)), &msg_size)) {
		DISPATCH_CLIENT_CRASH(size, "Message size too large");
	}

	dmsg = _dispatch_object_alloc(DISPATCH_VTABLE(mach_msg), msg_size);
	if (destructor) {
		dmsg->dmsg_msg = msg;
	} else if (msg) {
		memcpy(dmsg->dmsg_buf, msg, size);
	}
	dmsg->do_next = DISPATCH_OBJECT_LISTLESS;
	dmsg->do_targetq = _dispatch_get_root_queue(DISPATCH_QOS_DEFAULT, false);
	dmsg->dmsg_destructor = destructor;
	dmsg->dmsg_size = size;
	if (msg_ptr) {
		*msg_ptr = _dispatch_mach_msg_get_msg(dmsg);
	}
	return dmsg;
}

void
_dispatch_mach_msg_dispose(dispatch_mach_msg_t dmsg,
		DISPATCH_UNUSED bool *allow_free)
{
	if (dmsg->dmsg_voucher) {
		_voucher_release(dmsg->dmsg_voucher);
		dmsg->dmsg_voucher = NULL;
	}
	switch (dmsg->dmsg_destructor) {
	case DISPATCH_MACH_MSG_DESTRUCTOR_DEFAULT:
		break;
	case DISPATCH_MACH_MSG_DESTRUCTOR_FREE:
		free(dmsg->dmsg_msg);
		break;
	case DISPATCH_MACH_MSG_DESTRUCTOR_VM_DEALLOCATE: {
		mach_vm_size_t vm_size = dmsg->dmsg_size;
		mach_vm_address_t vm_addr = (uintptr_t)dmsg->dmsg_msg;
		(void)dispatch_assume_zero(mach_vm_deallocate(mach_task_self(),
				vm_addr, vm_size));
		break;
	}}
}

static inline mach_msg_header_t*
_dispatch_mach_msg_get_msg(dispatch_mach_msg_t dmsg)
{
	return dmsg->dmsg_destructor ? dmsg->dmsg_msg :
			(mach_msg_header_t*)dmsg->dmsg_buf;
}

mach_msg_header_t*
dispatch_mach_msg_get_msg(dispatch_mach_msg_t dmsg, size_t *size_ptr)
{
	if (size_ptr) {
		*size_ptr = dmsg->dmsg_size;
	}
	return _dispatch_mach_msg_get_msg(dmsg);
}

size_t
_dispatch_mach_msg_debug(dispatch_mach_msg_t dmsg, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			dx_kind(dmsg), dmsg);
	offset += _dispatch_object_debug_attr(dmsg, buf + offset, bufsiz - offset);
	offset += dsnprintf(&buf[offset], bufsiz - offset, "opts/err = 0x%x, "
			"msgh[%p] = { ", dmsg->dmsg_options, dmsg->dmsg_buf);
	mach_msg_header_t *hdr = _dispatch_mach_msg_get_msg(dmsg);
	if (hdr->msgh_id) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "id 0x%x, ",
				hdr->msgh_id);
	}
	if (hdr->msgh_size) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "size %u, ",
				hdr->msgh_size);
	}
	if (hdr->msgh_bits) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "bits <l %u, r %u",
				MACH_MSGH_BITS_LOCAL(hdr->msgh_bits),
				MACH_MSGH_BITS_REMOTE(hdr->msgh_bits));
		if (MACH_MSGH_BITS_OTHER(hdr->msgh_bits)) {
			offset += dsnprintf(&buf[offset], bufsiz - offset, ", o 0x%x",
					MACH_MSGH_BITS_OTHER(hdr->msgh_bits));
		}
		offset += dsnprintf(&buf[offset], bufsiz - offset, ">, ");
	}
	if (hdr->msgh_local_port && hdr->msgh_remote_port) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "local 0x%x, "
				"remote 0x%x", hdr->msgh_local_port, hdr->msgh_remote_port);
	} else if (hdr->msgh_local_port) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "local 0x%x",
				hdr->msgh_local_port);
	} else if (hdr->msgh_remote_port) {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "remote 0x%x",
				hdr->msgh_remote_port);
	} else {
		offset += dsnprintf(&buf[offset], bufsiz - offset, "no ports");
	}
	offset += dsnprintf(&buf[offset], bufsiz - offset, " } }");
	return offset;
}

DISPATCH_ALWAYS_INLINE
static dispatch_queue_t
_dispatch_mach_msg_context_async_reply_queue(void *msg_context)
{
	if (DISPATCH_MACH_XPC_SUPPORTS_ASYNC_REPLIES(_dispatch_mach_xpc_hooks)) {
		return _dispatch_mach_xpc_hooks->dmxh_msg_context_reply_queue(
				msg_context);
	}
	return NULL;
}

static dispatch_continuation_t
_dispatch_mach_msg_async_reply_wrap(dispatch_mach_msg_t dmsg,
		dispatch_mach_t dm)
{
	_dispatch_retain(dm); // Released in _dispatch_mach_msg_async_reply_invoke()
	dispatch_continuation_t dc = _dispatch_continuation_alloc();
	dc->do_vtable = DC_VTABLE(MACH_ASYNC_REPLY);
	dc->dc_data = dmsg;
	dc->dc_other = dm;
	dc->dc_priority = DISPATCH_NO_PRIORITY;
	dc->dc_voucher = DISPATCH_NO_VOUCHER;
	return dc;
}

DISPATCH_NOINLINE
void
_dispatch_mach_msg_async_reply_invoke(dispatch_continuation_t dc,
		DISPATCH_UNUSED dispatch_invoke_context_t dic,
		dispatch_invoke_flags_t flags)
{
	// _dispatch_mach_msg_invoke_with_mach() releases the reference on dmsg
	// taken by _dispatch_mach_msg_async_reply_wrap() after handling it.
	dispatch_mach_msg_t dmsg = dc->dc_data;
	dispatch_mach_t dm = dc->dc_other;
	_dispatch_mach_msg_invoke_with_mach(dmsg,
			flags | DISPATCH_INVOKE_ASYNC_REPLY, dm);

	// Balances _dispatch_mach_msg_async_reply_wrap
	_dispatch_release(dc->dc_other);

	_dispatch_continuation_free(dc);
}

#pragma mark -
#pragma mark dispatch_mig_server

mach_msg_return_t
dispatch_mig_server(dispatch_source_t ds, size_t maxmsgsz,
		dispatch_mig_callback_t callback)
{
	mach_msg_options_t options = MACH_RCV_MSG | MACH_RCV_TIMEOUT
		| MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_CTX)
		| MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) | MACH_RCV_VOUCHER;
	mach_msg_options_t tmp_options;
	mig_reply_error_t *bufTemp, *bufRequest, *bufReply;
	mach_msg_return_t kr = 0;
	uint64_t assertion_token = 0;
	uint32_t cnt = 1000; // do not stall out serial queues
	boolean_t demux_success;
	bool received = false;
	size_t rcv_size = maxmsgsz + MAX_TRAILER_SIZE;
	dispatch_source_refs_t dr = ds->ds_refs;

	bufRequest = alloca(rcv_size);
	bufRequest->RetCode = 0;
	for (mach_vm_address_t p = mach_vm_trunc_page(bufRequest + vm_page_size);
			p < (mach_vm_address_t)bufRequest + rcv_size; p += vm_page_size) {
		*(char*)p = 0; // ensure alloca buffer doesn't overlap with stack guard
	}

	bufReply = alloca(rcv_size);
	bufReply->Head.msgh_size = 0;
	for (mach_vm_address_t p = mach_vm_trunc_page(bufReply + vm_page_size);
			p < (mach_vm_address_t)bufReply + rcv_size; p += vm_page_size) {
		*(char*)p = 0; // ensure alloca buffer doesn't overlap with stack guard
	}

#if DISPATCH_DEBUG
	options |= MACH_RCV_LARGE; // rdar://problem/8422992
#endif
	tmp_options = options;
	// XXX FIXME -- change this to not starve out the target queue
	for (;;) {
		if (DISPATCH_QUEUE_IS_SUSPENDED(ds) || (--cnt == 0)) {
			options &= ~MACH_RCV_MSG;
			tmp_options &= ~MACH_RCV_MSG;

			if (!(tmp_options & MACH_SEND_MSG)) {
				goto out;
			}
		}
		kr = mach_msg(&bufReply->Head, tmp_options, bufReply->Head.msgh_size,
				(mach_msg_size_t)rcv_size, (mach_port_t)dr->du_ident, 0, 0);

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
						(mach_port_t)dr->du_ident, 0, 0)) {
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
			goto out;
		}

		if (!(tmp_options & MACH_RCV_MSG)) {
			goto out;
		}

		if (assertion_token) {
#if DISPATCH_USE_IMPORTANCE_ASSERTION
			int r = proc_importance_assertion_complete(assertion_token);
			(void)dispatch_assume_zero(r);
#endif
			assertion_token = 0;
		}
		received = true;

		bufTemp = bufRequest;
		bufRequest = bufReply;
		bufReply = bufTemp;

#if DISPATCH_USE_IMPORTANCE_ASSERTION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		int r = proc_importance_assertion_begin_with_msg(&bufRequest->Head,
				NULL, &assertion_token);
		if (r && slowpath(r != EIO)) {
			(void)dispatch_assume_zero(r);
		}
#pragma clang diagnostic pop
#endif
		_voucher_replace(voucher_create_with_mach_msg(&bufRequest->Head));
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

out:
	if (assertion_token) {
#if DISPATCH_USE_IMPORTANCE_ASSERTION
		int r = proc_importance_assertion_complete(assertion_token);
		(void)dispatch_assume_zero(r);
#endif
	}

	return kr;
}

#pragma mark -
#pragma mark dispatch_mach_debug

static size_t
_dispatch_mach_debug_attr(dispatch_mach_t dm, char *buf, size_t bufsiz)
{
	dispatch_queue_t target = dm->do_targetq;
	dispatch_mach_send_refs_t dmsr = dm->dm_send_refs;
	dispatch_mach_recv_refs_t dmrr = dm->dm_recv_refs;

	return dsnprintf(buf, bufsiz, "target = %s[%p], receive = 0x%x, "
			"send = 0x%x, send-possible = 0x%x%s, checkin = 0x%x%s, "
			"send state = %016llx, disconnected = %d, canceled = %d ",
			target && target->dq_label ? target->dq_label : "", target,
			(mach_port_t)dmrr->du_ident, dmsr->dmsr_send,
			(mach_port_t)dmsr->du_ident,
			dmsr->dmsr_notification_armed ? " (armed)" : "",
			dmsr->dmsr_checkin_port, dmsr->dmsr_checkin ? " (pending)" : "",
			dmsr->dmsr_state, dmsr->dmsr_disconnect_cnt,
			(bool)(dm->dq_atomic_flags & DSF_CANCELED));
}

size_t
_dispatch_mach_debug(dispatch_mach_t dm, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			dm->dq_label && !dm->dm_cancel_handler_called ? dm->dq_label :
			dx_kind(dm), dm);
	offset += _dispatch_object_debug_attr(dm, &buf[offset], bufsiz - offset);
	offset += _dispatch_mach_debug_attr(dm, &buf[offset], bufsiz - offset);
	offset += dsnprintf(&buf[offset], bufsiz - offset, "}");
	return offset;
}

#endif /* HAVE_MACH */
