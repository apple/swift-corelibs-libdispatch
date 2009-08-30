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

#include "internal.h"

// semaphores are too fundamental to use the dispatch_assume*() macros
#define DISPATCH_SEMAPHORE_VERIFY_KR(x)	do {	\
		if (x) {	\
			DISPATCH_CRASH("flawed group/semaphore logic");	\
		}	\
	} while (0)

struct dispatch_semaphore_vtable_s {
	DISPATCH_VTABLE_HEADER(dispatch_semaphore_s);
};

static void _dispatch_semaphore_dispose(dispatch_semaphore_t dsema);
static size_t _dispatch_semaphore_debug(dispatch_semaphore_t dsema, char *buf, size_t bufsiz);
static long _dispatch_group_wake(dispatch_semaphore_t dsema);

const struct dispatch_semaphore_vtable_s _dispatch_semaphore_vtable = {
	.do_type = DISPATCH_SEMAPHORE_TYPE,
	.do_kind = "semaphore",
	.do_dispose = _dispatch_semaphore_dispose,
	.do_debug = _dispatch_semaphore_debug,
};

dispatch_semaphore_t
_dispatch_get_thread_semaphore(void)
{
	dispatch_semaphore_t dsema;
	
	dsema = fastpath(_dispatch_thread_getspecific(dispatch_sema4_key));
	if (!dsema) {
		while (!(dsema = dispatch_semaphore_create(0))) {
			sleep(1);
		}
	}
	_dispatch_thread_setspecific(dispatch_sema4_key, NULL);
	return dsema;
}

void
_dispatch_put_thread_semaphore(dispatch_semaphore_t dsema)
{
	dispatch_semaphore_t old_sema = _dispatch_thread_getspecific(dispatch_sema4_key);
	_dispatch_thread_setspecific(dispatch_sema4_key, dsema);
	if (old_sema) {
		dispatch_release(old_sema);
	}
}

dispatch_group_t
dispatch_group_create(void)
{
	return (dispatch_group_t)dispatch_semaphore_create(LONG_MAX);
}

dispatch_semaphore_t
dispatch_semaphore_create(long value)
{
	dispatch_semaphore_t dsema;
	
	// If the internal value is negative, then the absolute of the value is
	// equal to the number of waiting threads. Therefore it is bogus to
	// initialize the semaphore with a negative value.
	if (value < 0) {
		return NULL;
	}
	
	dsema = calloc(1, sizeof(struct dispatch_semaphore_s));
	
	if (fastpath(dsema)) {
		dsema->do_vtable = &_dispatch_semaphore_vtable;
		dsema->do_next = DISPATCH_OBJECT_LISTLESS;
		dsema->do_ref_cnt = 1;
		dsema->do_xref_cnt = 1;
		dsema->do_targetq = dispatch_get_global_queue(0, 0);
		dsema->dsema_value = value;
		dsema->dsema_orig = value;
	}
	
	return dsema;
}

static void
_dispatch_semaphore_create_port(semaphore_t *s4)
{
	kern_return_t kr;
	semaphore_t tmp;

	if (*s4) {
		return;
	}
	
	// lazily allocate the semaphore port
	
	// Someday:
	// 1) Switch to a doubly-linked FIFO in user-space.
	// 2) User-space timers for the timeout.
	// 3) Use the per-thread semaphore port.
	
	while (dispatch_assume_zero(kr = semaphore_create(mach_task_self(), &tmp, SYNC_POLICY_FIFO, 0))) {
		DISPATCH_VERIFY_MIG(kr);
		sleep(1);
	}
	
	if (!dispatch_atomic_cmpxchg(s4, 0, tmp)) {
		kr = semaphore_destroy(mach_task_self(), tmp);
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
	}

	_dispatch_safe_fork = false;
}

DISPATCH_NOINLINE
static long
_dispatch_semaphore_wait_slow(dispatch_semaphore_t dsema, dispatch_time_t timeout)
{
	mach_timespec_t _timeout;
	kern_return_t kr;
	uint64_t nsec;
	long orig;
	
again:
	// Mach semaphores appear to sometimes spuriously wake up.  Therefore,
	// we keep a parallel count of the number of times a Mach semaphore is
	// signaled.
	while ((orig = dsema->dsema_sent_ksignals)) {
		if (dispatch_atomic_cmpxchg(&dsema->dsema_sent_ksignals, orig, orig - 1)) {
			return 0;
		}
	}

	_dispatch_semaphore_create_port(&dsema->dsema_port);

	// From xnu/osfmk/kern/sync_sema.c:
	// wait_semaphore->count = -1;  /* we don't keep an actual count */
	//
	// The code above does not match the documentation, and that fact is
	// not surprising. The documented semantics are clumsy to use in any
	// practical way. The above hack effectively tricks the rest of the
	// Mach semaphore logic to behave like the libdispatch algorithm.

	switch (timeout) {
	default:
		do {
			// timeout() already calculates relative time left
			nsec = _dispatch_timeout(timeout);
			_timeout.tv_sec = (typeof(_timeout.tv_sec))(nsec / NSEC_PER_SEC);
			_timeout.tv_nsec = (typeof(_timeout.tv_nsec))(nsec % NSEC_PER_SEC);
			kr = slowpath(semaphore_timedwait(dsema->dsema_port, _timeout));
		} while (kr == KERN_ABORTED);

		if (kr != KERN_OPERATION_TIMED_OUT) {
			DISPATCH_SEMAPHORE_VERIFY_KR(kr);
			break;
		}
		// Fall through and try to undo what the fast path did to dsema->dsema_value
	case DISPATCH_TIME_NOW:
		while ((orig = dsema->dsema_value) < 0) {
			if (dispatch_atomic_cmpxchg(&dsema->dsema_value, orig, orig + 1)) {
				return KERN_OPERATION_TIMED_OUT;
			}
		}
		// Another thread called semaphore_signal().
		// Fall through and drain the wakeup.
	case DISPATCH_TIME_FOREVER:
		do {
			kr = semaphore_wait(dsema->dsema_port);
		} while (kr == KERN_ABORTED);
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
		break;
	}

	goto again;
}

DISPATCH_NOINLINE
void
dispatch_group_enter(dispatch_group_t dg)
{
	dispatch_semaphore_t dsema = (dispatch_semaphore_t)dg;
#if defined(__OPTIMIZE__) && defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
	// This assumes:
	// 1) Way too much about the optimizer of GCC.
	// 2) There will never be more than LONG_MAX threads.
	//    Therefore: no overflow detection
	asm(
#ifdef __LP64__ 
		"lock decq	%0\n\t"
#else
		"lock decl	%0\n\t"
#endif
		"js 	1f\n\t"
		"xor	%%eax, %%eax\n\t"
		"ret\n\t"
		"1:"
		: "+m" (dsema->dsema_value)
		:
		: "cc"
	);
	_dispatch_semaphore_wait_slow(dsema, DISPATCH_TIME_FOREVER);
#else
	dispatch_semaphore_wait(dsema, DISPATCH_TIME_FOREVER);
#endif
}

DISPATCH_NOINLINE
long
dispatch_semaphore_wait(dispatch_semaphore_t dsema, dispatch_time_t timeout)
{
#if defined(__OPTIMIZE__) && defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
	// This assumes:
	// 1) Way too much about the optimizer of GCC.
	// 2) There will never be more than LONG_MAX threads.
	//    Therefore: no overflow detection
	asm(
#ifdef __LP64__ 
		"lock decq	%0\n\t"
#else
		"lock decl	%0\n\t"
#endif
		"js 	1f\n\t"
		"xor	%%eax, %%eax\n\t"
		"ret\n\t"
		"1:"
		: "+m" (dsema->dsema_value)
		:
		: "cc"
	);
#else
	if (dispatch_atomic_dec(&dsema->dsema_value) >= 0) {
		return 0;
	}
#endif
	return _dispatch_semaphore_wait_slow(dsema, timeout);
}

DISPATCH_NOINLINE
static long
_dispatch_semaphore_signal_slow(dispatch_semaphore_t dsema)
{
	kern_return_t kr;
	
	_dispatch_semaphore_create_port(&dsema->dsema_port);

	// Before dsema_sent_ksignals is incremented we can rely on the reference
	// held by the waiter. However, once this value is incremented the waiter
	// may return between the atomic increment and the semaphore_signal(),
	// therefore an explicit reference must be held in order to safely access
	// dsema after the atomic increment.
	_dispatch_retain(dsema);
	
	dispatch_atomic_inc(&dsema->dsema_sent_ksignals);
	
	kr = semaphore_signal(dsema->dsema_port);
	DISPATCH_SEMAPHORE_VERIFY_KR(kr);

	_dispatch_release(dsema);
	
	return 1;
}

void
dispatch_group_leave(dispatch_group_t dg)
{
	dispatch_semaphore_t dsema = (dispatch_semaphore_t)dg;

	dispatch_semaphore_signal(dsema);

	if (dsema->dsema_value == dsema->dsema_orig) {
		_dispatch_group_wake(dsema);
	}
}

DISPATCH_NOINLINE
long
dispatch_semaphore_signal(dispatch_semaphore_t dsema)
{
#if defined(__OPTIMIZE__) && defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
	// overflow detection
	// this assumes way too much about the optimizer of GCC
	asm(
#ifdef __LP64__ 
		"lock incq	%0\n\t"
#else
		"lock incl	%0\n\t"
#endif
		"jo 	1f\n\t"
		"jle 	2f\n\t"
		"xor	%%eax, %%eax\n\t"
		"ret\n\t"
		"1:\n\t"
		"int	$4\n\t"
		"2:"
		: "+m" (dsema->dsema_value)
		:
		: "cc"
	);
#else
	if (dispatch_atomic_inc(&dsema->dsema_value) > 0) {
		return 0;
	}
#endif
	return _dispatch_semaphore_signal_slow(dsema);
}

DISPATCH_NOINLINE
long
_dispatch_group_wake(dispatch_semaphore_t dsema)
{
	struct dispatch_sema_notify_s *tmp, *head = dispatch_atomic_xchg(&dsema->dsema_notify_head, NULL);
	long rval = dispatch_atomic_xchg(&dsema->dsema_group_waiters, 0);
	bool do_rel = head;
	long kr;

	// wake any "group" waiter or notify blocks
	
	if (rval) {
		_dispatch_semaphore_create_port(&dsema->dsema_waiter_port);
		do {
			kr = semaphore_signal(dsema->dsema_waiter_port);
			DISPATCH_SEMAPHORE_VERIFY_KR(kr);
		} while (--rval);
	}
	while (head) {
		dispatch_async_f(head->dsn_queue, head->dsn_ctxt, head->dsn_func);
		_dispatch_release(head->dsn_queue);
		do {
			tmp = head->dsn_next;
		} while (!tmp && !dispatch_atomic_cmpxchg(&dsema->dsema_notify_tail, head, NULL));
		free(head);
		head = tmp;
	}
	if (do_rel) {
		_dispatch_release(dsema);
	}
	return 0;
}

DISPATCH_NOINLINE
static long
_dispatch_group_wait_slow(dispatch_semaphore_t dsema, dispatch_time_t timeout)
{
	mach_timespec_t _timeout;
	kern_return_t kr;
	uint64_t nsec;
	long orig;
	
again:
	// check before we cause another signal to be sent by incrementing dsema->dsema_group_waiters
	if (dsema->dsema_value == dsema->dsema_orig) {
		return _dispatch_group_wake(dsema);
	}
	// Mach semaphores appear to sometimes spuriously wake up.  Therefore,
	// we keep a parallel count of the number of times a Mach semaphore is
	// signaled.
	dispatch_atomic_inc(&dsema->dsema_group_waiters);
	// check the values again in case we need to wake any threads
	if (dsema->dsema_value == dsema->dsema_orig) {
		return _dispatch_group_wake(dsema);
	}

	_dispatch_semaphore_create_port(&dsema->dsema_waiter_port);
	
	// From xnu/osfmk/kern/sync_sema.c:
	// wait_semaphore->count = -1;  /* we don't keep an actual count */
	//
	// The code above does not match the documentation, and that fact is
	// not surprising. The documented semantics are clumsy to use in any
	// practical way. The above hack effectively tricks the rest of the
	// Mach semaphore logic to behave like the libdispatch algorithm.
	
	switch (timeout) {
	default:
		do {
			nsec = _dispatch_timeout(timeout);
			_timeout.tv_sec = (typeof(_timeout.tv_sec))(nsec / NSEC_PER_SEC);
			_timeout.tv_nsec = (typeof(_timeout.tv_nsec))(nsec % NSEC_PER_SEC);
			kr = slowpath(semaphore_timedwait(dsema->dsema_waiter_port, _timeout));
		} while (kr == KERN_ABORTED);
		if (kr != KERN_OPERATION_TIMED_OUT) {
			DISPATCH_SEMAPHORE_VERIFY_KR(kr);
			break;
		}
		// Fall through and try to undo the earlier change to dsema->dsema_group_waiters
	case DISPATCH_TIME_NOW:
		while ((orig = dsema->dsema_group_waiters)) {
			if (dispatch_atomic_cmpxchg(&dsema->dsema_group_waiters, orig, orig - 1)) {
				return KERN_OPERATION_TIMED_OUT;
			}
		}
		// Another thread called semaphore_signal().
		// Fall through and drain the wakeup.
	case DISPATCH_TIME_FOREVER:
		do {
			kr = semaphore_wait(dsema->dsema_waiter_port);
		} while (kr == KERN_ABORTED);
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
		break;
	}

	goto again;
}

long
dispatch_group_wait(dispatch_group_t dg, dispatch_time_t timeout)
{
	dispatch_semaphore_t dsema = (dispatch_semaphore_t)dg;

	if (dsema->dsema_value == dsema->dsema_orig) {
		return 0;
	}
	if (timeout == 0) {
		return KERN_OPERATION_TIMED_OUT;
	}
	return _dispatch_group_wait_slow(dsema, timeout);
}

#ifdef __BLOCKS__
void
dispatch_group_notify(dispatch_group_t dg, dispatch_queue_t dq, dispatch_block_t db)
{
	dispatch_group_notify_f(dg, dq, _dispatch_Block_copy(db), _dispatch_call_block_and_release);
}
#endif

void
dispatch_group_notify_f(dispatch_group_t dg, dispatch_queue_t dq, void *ctxt, void (*func)(void *))
{
	dispatch_semaphore_t dsema = (dispatch_semaphore_t)dg;
	struct dispatch_sema_notify_s *dsn, *prev;

	// FIXME -- this should be updated to use the continuation cache
	while (!(dsn = malloc(sizeof(*dsn)))) {
		sleep(1);
	}

	dsn->dsn_next = NULL;
	dsn->dsn_queue = dq;
	dsn->dsn_ctxt = ctxt;
	dsn->dsn_func = func;
	_dispatch_retain(dq);

	prev = dispatch_atomic_xchg(&dsema->dsema_notify_tail, dsn);
	if (fastpath(prev)) {
		prev->dsn_next = dsn;
	} else {
		_dispatch_retain(dg);
		dsema->dsema_notify_head = dsn;
		if (dsema->dsema_value == dsema->dsema_orig) {
			_dispatch_group_wake(dsema);
		}
	}
}

void
_dispatch_semaphore_dispose(dispatch_semaphore_t dsema)
{
	kern_return_t kr;
	
	if (dsema->dsema_value < dsema->dsema_orig) {
		DISPATCH_CLIENT_CRASH("Semaphore/group object deallocated while in use");
	}
	
	if (dsema->dsema_port) {
		kr = semaphore_destroy(mach_task_self(), dsema->dsema_port);
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
	}
	if (dsema->dsema_waiter_port) {
		kr = semaphore_destroy(mach_task_self(), dsema->dsema_waiter_port);
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
	}
	
	_dispatch_dispose(dsema);
}

size_t
_dispatch_semaphore_debug(dispatch_semaphore_t dsema, char *buf, size_t bufsiz)
{
	size_t offset = 0;
	offset += snprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ", dx_kind(dsema), dsema);
	offset += dispatch_object_debug_attr(dsema, &buf[offset], bufsiz - offset);
	offset += snprintf(&buf[offset], bufsiz - offset, "port = 0x%u, value = %ld, orig = %ld }",
					   dsema->dsema_port, dsema->dsema_value, dsema->dsema_orig);
	return offset;
}

#ifdef __BLOCKS__
void
dispatch_group_async(dispatch_group_t dg, dispatch_queue_t dq, dispatch_block_t db)
{
	dispatch_group_async_f(dg, dq, _dispatch_Block_copy(db), _dispatch_call_block_and_release);
}
#endif

DISPATCH_NOINLINE
void
dispatch_group_async_f(dispatch_group_t dg, dispatch_queue_t dq, void *ctxt, void (*func)(void *))
{
	dispatch_continuation_t dc;

	_dispatch_retain(dg);
	dispatch_group_enter(dg);

	dc = _dispatch_continuation_alloc_cacheonly() ?: _dispatch_continuation_alloc_from_heap();

	dc->do_vtable = (void *)(DISPATCH_OBJ_ASYNC_BIT|DISPATCH_OBJ_GROUP_BIT);
	dc->dc_func = func;
	dc->dc_ctxt = ctxt;
	dc->dc_group = dg;

	_dispatch_queue_push(dq, dc);
}
