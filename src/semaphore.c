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

#include "internal.h"

DISPATCH_WEAK // rdar://problem/8503746
long _dispatch_semaphore_signal_slow(dispatch_semaphore_t dsema);

#pragma mark -
#pragma mark dispatch_semaphore_class_t

static void
_dispatch_semaphore_class_init(long value, dispatch_semaphore_class_t dsemau)
{
	struct dispatch_semaphore_header_s *dsema = dsemau._dsema_hdr;

	dsema->do_next = DISPATCH_OBJECT_LISTLESS;
	dsema->do_targetq = _dispatch_get_root_queue(DISPATCH_QOS_DEFAULT, false);
	dsema->dsema_value = value;
	_dispatch_sema4_init(&dsema->dsema_sema, _DSEMA4_POLICY_FIFO);
}

#pragma mark -
#pragma mark dispatch_semaphore_t

dispatch_semaphore_t
dispatch_semaphore_create(long value)
{
	dispatch_semaphore_t dsema;

	// If the internal value is negative, then the absolute of the value is
	// equal to the number of waiting threads. Therefore it is bogus to
	// initialize the semaphore with a negative value.
	if (value < 0) {
		return DISPATCH_BAD_INPUT;
	}

	dsema = (dispatch_semaphore_t)_dispatch_object_alloc(
			DISPATCH_VTABLE(semaphore), sizeof(struct dispatch_semaphore_s));
	_dispatch_semaphore_class_init(value, dsema);
	dsema->dsema_orig = value;
	return dsema;
}

void
_dispatch_semaphore_dispose(dispatch_object_t dou,
		DISPATCH_UNUSED bool *allow_free)
{
	dispatch_semaphore_t dsema = dou._dsema;

	if (dsema->dsema_value < dsema->dsema_orig) {
		DISPATCH_CLIENT_CRASH(dsema->dsema_orig - dsema->dsema_value,
				"Semaphore object deallocated while in use");
	}

	_dispatch_sema4_dispose(&dsema->dsema_sema, _DSEMA4_POLICY_FIFO);
}

size_t
_dispatch_semaphore_debug(dispatch_object_t dou, char *buf, size_t bufsiz)
{
	dispatch_semaphore_t dsema = dou._dsema;

	size_t offset = 0;
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			dx_kind(dsema), dsema);
	offset += _dispatch_object_debug_attr(dsema, &buf[offset], bufsiz - offset);
#if USE_MACH_SEM
	offset += dsnprintf(&buf[offset], bufsiz - offset, "port = 0x%u, ",
			dsema->dsema_sema);
#endif
	offset += dsnprintf(&buf[offset], bufsiz - offset,
			"value = %ld, orig = %ld }", dsema->dsema_value, dsema->dsema_orig);
	return offset;
}

DISPATCH_NOINLINE
long
_dispatch_semaphore_signal_slow(dispatch_semaphore_t dsema)
{
	_dispatch_sema4_create(&dsema->dsema_sema, _DSEMA4_POLICY_FIFO);
	_dispatch_sema4_signal(&dsema->dsema_sema, 1);
	return 1;
}

long
dispatch_semaphore_signal(dispatch_semaphore_t dsema)
{
	long value = os_atomic_inc2o(dsema, dsema_value, release);
	if (fastpath(value > 0)) {
		return 0;
	}
	if (slowpath(value == LONG_MIN)) {
		DISPATCH_CLIENT_CRASH(value,
				"Unbalanced call to dispatch_semaphore_signal()");
	}
	return _dispatch_semaphore_signal_slow(dsema);
}

DISPATCH_NOINLINE
static long
_dispatch_semaphore_wait_slow(dispatch_semaphore_t dsema,
		dispatch_time_t timeout)
{
	long orig;

	_dispatch_sema4_create(&dsema->dsema_sema, _DSEMA4_POLICY_FIFO);
	switch (timeout) {
	default:
		if (!_dispatch_sema4_timedwait(&dsema->dsema_sema, timeout)) {
			break;
		}
		// Fall through and try to undo what the fast path did to
		// dsema->dsema_value
	case DISPATCH_TIME_NOW:
		orig = dsema->dsema_value;
		while (orig < 0) {
			if (os_atomic_cmpxchgvw2o(dsema, dsema_value, orig, orig + 1,
					&orig, relaxed)) {
				return _DSEMA4_TIMEOUT();
			}
		}
		// Another thread called semaphore_signal().
		// Fall through and drain the wakeup.
	case DISPATCH_TIME_FOREVER:
		_dispatch_sema4_wait(&dsema->dsema_sema);
		break;
	}
	return 0;
}

long
dispatch_semaphore_wait(dispatch_semaphore_t dsema, dispatch_time_t timeout)
{
	long value = os_atomic_dec2o(dsema, dsema_value, acquire);
	if (fastpath(value >= 0)) {
		return 0;
	}
	return _dispatch_semaphore_wait_slow(dsema, timeout);
}

#pragma mark -
#pragma mark dispatch_group_t

DISPATCH_ALWAYS_INLINE
static inline dispatch_group_t
_dispatch_group_create_with_count(long count)
{
	dispatch_group_t dg = (dispatch_group_t)_dispatch_object_alloc(
			DISPATCH_VTABLE(group), sizeof(struct dispatch_group_s));
	_dispatch_semaphore_class_init(count, dg);
	if (count) {
		os_atomic_store2o(dg, do_ref_cnt, 1, relaxed); // <rdar://problem/22318411>
	}
	return dg;
}

dispatch_group_t
dispatch_group_create(void)
{
	return _dispatch_group_create_with_count(0);
}

dispatch_group_t
_dispatch_group_create_and_enter(void)
{
	return _dispatch_group_create_with_count(1);
}

void
dispatch_group_enter(dispatch_group_t dg)
{
	long value = os_atomic_inc_orig2o(dg, dg_value, acquire);
	if (slowpath((unsigned long)value >= (unsigned long)LONG_MAX)) {
		DISPATCH_CLIENT_CRASH(value,
				"Too many nested calls to dispatch_group_enter()");
	}
	if (value == 0) {
		_dispatch_retain(dg); // <rdar://problem/22318411>
	}
}

DISPATCH_NOINLINE
static long
_dispatch_group_wake(dispatch_group_t dg, bool needs_release)
{
	dispatch_continuation_t next, head, tail = NULL;
	long rval;

	// cannot use os_mpsc_capture_snapshot() because we can have concurrent
	// _dispatch_group_wake() calls
	head = os_atomic_xchg2o(dg, dg_notify_head, NULL, relaxed);
	if (head) {
		// snapshot before anything is notified/woken <rdar://problem/8554546>
		tail = os_atomic_xchg2o(dg, dg_notify_tail, NULL, release);
	}
	rval = (long)os_atomic_xchg2o(dg, dg_waiters, 0, relaxed);
	if (rval) {
		// wake group waiters
		_dispatch_sema4_create(&dg->dg_sema, _DSEMA4_POLICY_FIFO);
		_dispatch_sema4_signal(&dg->dg_sema, rval);
	}
	uint16_t refs = needs_release ? 1 : 0; // <rdar://problem/22318411>
	if (head) {
		// async group notify blocks
		do {
			next = os_mpsc_pop_snapshot_head(head, tail, do_next);
			dispatch_queue_t dsn_queue = (dispatch_queue_t)head->dc_data;
			_dispatch_continuation_async(dsn_queue, head);
			_dispatch_release(dsn_queue);
		} while ((head = next));
		refs++;
	}
	if (refs) _dispatch_release_n(dg, refs);
	return 0;
}

void
dispatch_group_leave(dispatch_group_t dg)
{
	long value = os_atomic_dec2o(dg, dg_value, release);
	if (slowpath(value == 0)) {
		return (void)_dispatch_group_wake(dg, true);
	}
	if (slowpath(value < 0)) {
		DISPATCH_CLIENT_CRASH(value,
				"Unbalanced call to dispatch_group_leave()");
	}
}

void
_dispatch_group_dispose(dispatch_object_t dou, DISPATCH_UNUSED bool *allow_free)
{
	dispatch_group_t dg = dou._dg;

	if (dg->dg_value) {
		DISPATCH_CLIENT_CRASH(dg->dg_value,
				"Group object deallocated while in use");
	}

	_dispatch_sema4_dispose(&dg->dg_sema, _DSEMA4_POLICY_FIFO);
}

size_t
_dispatch_group_debug(dispatch_object_t dou, char *buf, size_t bufsiz)
{
	dispatch_group_t dg = dou._dg;

	size_t offset = 0;
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			dx_kind(dg), dg);
	offset += _dispatch_object_debug_attr(dg, &buf[offset], bufsiz - offset);
#if USE_MACH_SEM
	offset += dsnprintf(&buf[offset], bufsiz - offset, "port = 0x%u, ",
			dg->dg_sema);
#endif
	offset += dsnprintf(&buf[offset], bufsiz - offset,
			"count = %ld, waiters = %d }", dg->dg_value, dg->dg_waiters);
	return offset;
}

DISPATCH_NOINLINE
static long
_dispatch_group_wait_slow(dispatch_group_t dg, dispatch_time_t timeout)
{
	long value;
	int orig_waiters;

	// check before we cause another signal to be sent by incrementing
	// dg->dg_waiters
	value = os_atomic_load2o(dg, dg_value, ordered); // 19296565
	if (value == 0) {
		return _dispatch_group_wake(dg, false);
	}

	(void)os_atomic_inc2o(dg, dg_waiters, relaxed);
	// check the values again in case we need to wake any threads
	value = os_atomic_load2o(dg, dg_value, ordered); // 19296565
	if (value == 0) {
		_dispatch_group_wake(dg, false);
		// Fall through to consume the extra signal, forcing timeout to avoid
		// useless setups as it won't block
		timeout = DISPATCH_TIME_FOREVER;
	}

	_dispatch_sema4_create(&dg->dg_sema, _DSEMA4_POLICY_FIFO);
	switch (timeout) {
	default:
		if (!_dispatch_sema4_timedwait(&dg->dg_sema, timeout)) {
			break;
		}
		// Fall through and try to undo the earlier change to
		// dg->dg_waiters
	case DISPATCH_TIME_NOW:
		orig_waiters = dg->dg_waiters;
		while (orig_waiters) {
			if (os_atomic_cmpxchgvw2o(dg, dg_waiters, orig_waiters,
					orig_waiters - 1, &orig_waiters, relaxed)) {
				return _DSEMA4_TIMEOUT();
			}
		}
		// Another thread is running _dispatch_group_wake()
		// Fall through and drain the wakeup.
	case DISPATCH_TIME_FOREVER:
		_dispatch_sema4_wait(&dg->dg_sema);
		break;
	}
	return 0;
}

long
dispatch_group_wait(dispatch_group_t dg, dispatch_time_t timeout)
{
	if (dg->dg_value == 0) {
		return 0;
	}
	if (timeout == 0) {
		return _DSEMA4_TIMEOUT();
	}
	return _dispatch_group_wait_slow(dg, timeout);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_group_notify(dispatch_group_t dg, dispatch_queue_t dq,
		dispatch_continuation_t dsn)
{
	dsn->dc_data = dq;
	dsn->do_next = NULL;
	_dispatch_retain(dq);
	if (os_mpsc_push_update_tail(dg, dg_notify, dsn, do_next)) {
		_dispatch_retain(dg);
		os_atomic_store2o(dg, dg_notify_head, dsn, ordered);
		// seq_cst with atomic store to notify_head <rdar://problem/11750916>
		if (os_atomic_load2o(dg, dg_value, ordered) == 0) {
			_dispatch_group_wake(dg, false);
		}
	}
}

DISPATCH_NOINLINE
void
dispatch_group_notify_f(dispatch_group_t dg, dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func)
{
	dispatch_continuation_t dsn = _dispatch_continuation_alloc();
	_dispatch_continuation_init_f(dsn, dq, ctxt, func, 0, 0,
			DISPATCH_OBJ_CONSUME_BIT);
	_dispatch_group_notify(dg, dq, dsn);
}

#ifdef __BLOCKS__
void
dispatch_group_notify(dispatch_group_t dg, dispatch_queue_t dq,
		dispatch_block_t db)
{
	dispatch_continuation_t dsn = _dispatch_continuation_alloc();
	_dispatch_continuation_init(dsn, dq, db, 0, 0, DISPATCH_OBJ_CONSUME_BIT);
	_dispatch_group_notify(dg, dq, dsn);
}
#endif
