/*
 * Copyright (c) 2016 Apple Inc. All rights reserved.
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

#define _dlock_syscall_switch(err, syscall, ...) \
	for (;;) { \
		int err; \
		switch ((err = ((syscall) < 0 ? errno : 0))) { \
		case EINTR: continue; \
		__VA_ARGS__ \
		} \
		break; \
	}

#if TARGET_OS_MAC
_Static_assert(DLOCK_LOCK_DATA_CONTENTION == ULF_WAIT_WORKQ_DATA_CONTENTION,
		"values should be the same");

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_switch(dispatch_lock value, dispatch_lock_options_t flags,
		uint32_t timeout)
{
	int option;
	if (flags & DLOCK_LOCK_DATA_CONTENTION) {
		option = SWITCH_OPTION_OSLOCK_DEPRESS;
	} else {
		option = SWITCH_OPTION_DEPRESS;
	}
	thread_switch(_dispatch_lock_owner(value), option, timeout);
}
#endif

#pragma mark - ulock wrappers
#if HAVE_UL_COMPARE_AND_WAIT

static int
_dispatch_ulock_wait(uint32_t *uaddr, uint32_t val, uint32_t timeout,
		uint32_t flags)
{
	dispatch_assert(!DISPATCH_LOCK_USE_SEMAPHORE_FALLBACK);
	int rc;
	_dlock_syscall_switch(err,
		rc = __ulock_wait(UL_COMPARE_AND_WAIT | flags, uaddr, val, timeout),
		case 0: return rc > 0 ? ENOTEMPTY : 0;
		case ETIMEDOUT: case EFAULT: return err;
		default: DISPATCH_INTERNAL_CRASH(err, "ulock_wait() failed");
	);
}

static void
_dispatch_ulock_wake(uint32_t *uaddr, uint32_t flags)
{
	dispatch_assert(!DISPATCH_LOCK_USE_SEMAPHORE_FALLBACK);
	_dlock_syscall_switch(err,
		__ulock_wake(UL_COMPARE_AND_WAIT | flags, uaddr, 0),
		case 0: case ENOENT: break;
		default: DISPATCH_INTERNAL_CRASH(err, "ulock_wake() failed");
	);
}

#endif
#if HAVE_UL_UNFAIR_LOCK

// returns 0, ETIMEDOUT, ENOTEMPTY, EFAULT
static int
_dispatch_unfair_lock_wait(uint32_t *uaddr, uint32_t val, uint32_t timeout,
		dispatch_lock_options_t flags)
{
	if (DISPATCH_LOCK_USE_SEMAPHORE_FALLBACK) {
		// <rdar://problem/25075359>
		timeout =  timeout < 1000 ? 1 : timeout / 1000;
		_dispatch_thread_switch(val, flags, timeout);
		return 0;
	}
	int rc;
	_dlock_syscall_switch(err,
		rc = __ulock_wait(UL_UNFAIR_LOCK | flags, uaddr, val, timeout),
		case 0: return rc > 0 ? ENOTEMPTY : 0;
		case ETIMEDOUT: case EFAULT: return err;
		default: DISPATCH_INTERNAL_CRASH(err, "ulock_wait() failed");
	);
}

static void
_dispatch_unfair_lock_wake(uint32_t *uaddr, uint32_t flags)
{
	if (DISPATCH_LOCK_USE_SEMAPHORE_FALLBACK) {
		// <rdar://problem/25075359>
		return;
	}
	_dlock_syscall_switch(err, __ulock_wake(UL_UNFAIR_LOCK | flags, uaddr, 0),
		case 0: case ENOENT: break;
		default: DISPATCH_INTERNAL_CRASH(err, "ulock_wake() failed");
	);
}

#endif
#pragma mark - futex wrappers
#if HAVE_FUTEX
#include <sys/time.h>
#ifdef __ANDROID__
#include <sys/syscall.h>
#else
#include <syscall.h>
#endif /* __ANDROID__ */

DISPATCH_ALWAYS_INLINE
static inline int
_dispatch_futex(uint32_t *uaddr, int op, uint32_t val,
		const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3,
		int opflags)
{
	return syscall(SYS_futex, uaddr, op | opflags, val, timeout, uaddr2, val3);
}

static int
_dispatch_futex_wait(uint32_t *uaddr, uint32_t val,
		const struct timespec *timeout, int opflags)
{
	_dlock_syscall_switch(err,
		_dispatch_futex(uaddr, FUTEX_WAIT, val, timeout, NULL, 0, opflags),
		case 0: case EWOULDBLOCK: case ETIMEDOUT: return err;
		default: DISPATCH_CLIENT_CRASH(err, "futex_wait() failed");
	);
}

static void
_dispatch_futex_wake(uint32_t *uaddr, int wake, int opflags)
{
	int rc;
	_dlock_syscall_switch(err,
		rc = _dispatch_futex(uaddr, FUTEX_WAKE, wake, NULL, NULL, 0, opflags),
		case 0: return;
		default: DISPATCH_CLIENT_CRASH(err, "futex_wake() failed");
	);
}

static void
_dispatch_futex_lock_pi(uint32_t *uaddr, struct timespec *timeout, int detect,
	      int opflags)
{
	_dlock_syscall_switch(err,
		_dispatch_futex(uaddr, FUTEX_LOCK_PI, detect, timeout,
				NULL, 0, opflags),
		case 0: return;
		default: DISPATCH_CLIENT_CRASH(errno, "futex_lock_pi() failed");
	);
}

static void
_dispatch_futex_unlock_pi(uint32_t *uaddr, int opflags)
{
	_dlock_syscall_switch(err,
		_dispatch_futex(uaddr, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0, opflags),
		case 0: return;
		default: DISPATCH_CLIENT_CRASH(errno, "futex_unlock_pi() failed");
	);
}

#endif
#pragma mark - wait for address

void
_dispatch_wait_on_address(uint32_t volatile *address, uint32_t value,
		dispatch_lock_options_t flags)
{
#if HAVE_UL_COMPARE_AND_WAIT
	_dispatch_ulock_wait((uint32_t *)address, value, 0, flags);
#elif HAVE_FUTEX
	_dispatch_futex_wait((uint32_t *)address, value, NULL, FUTEX_PRIVATE_FLAG);
#else
	mach_msg_timeout_t timeout = 1;
	while (os_atomic_load(address, relaxed) == value) {
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, timeout++);
	}
#endif
	(void)flags;
}

void
_dispatch_wake_by_address(uint32_t volatile *address)
{
#if HAVE_UL_COMPARE_AND_WAIT
	_dispatch_ulock_wake((uint32_t *)address, ULF_WAKE_ALL);
#elif HAVE_FUTEX
	_dispatch_futex_wake((uint32_t *)address, INT_MAX, FUTEX_PRIVATE_FLAG);
#else
	(void)address;
#endif
}

#pragma mark - thread event

#if DISPATCH_LOCK_USE_SEMAPHORE_FALLBACK
semaphore_t
_dispatch_thread_semaphore_create(void)
{
	semaphore_t s4;
	kern_return_t kr;
	while (unlikely(kr = semaphore_create(mach_task_self(), &s4,
			SYNC_POLICY_FIFO, 0))) {
		DISPATCH_VERIFY_MIG(kr);
		_dispatch_temporary_resource_shortage();
	}
	return s4;
}

void
_dispatch_thread_semaphore_dispose(void *ctxt)
{
	semaphore_t s4 = (semaphore_t)(uintptr_t)ctxt;
	kern_return_t kr = semaphore_destroy(mach_task_self(), s4);
	DISPATCH_VERIFY_MIG(kr);
	DISPATCH_SEMAPHORE_VERIFY_KR(kr);
}
#endif

void
_dispatch_thread_event_signal_slow(dispatch_thread_event_t dte)
{
#if DISPATCH_LOCK_USE_SEMAPHORE_FALLBACK
	if (DISPATCH_LOCK_USE_SEMAPHORE_FALLBACK) {
		kern_return_t kr = semaphore_signal(dte->dte_semaphore);
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
		return;
	}
#endif
#if HAVE_UL_COMPARE_AND_WAIT
	_dispatch_ulock_wake(&dte->dte_value, 0);
#elif HAVE_FUTEX
	_dispatch_futex_wake(&dte->dte_value, 1, FUTEX_PRIVATE_FLAG);
#elif USE_POSIX_SEM
	int rc = sem_post(&dte->dte_sem);
	DISPATCH_SEMAPHORE_VERIFY_RET(ret);
#endif
}

void
_dispatch_thread_event_wait_slow(dispatch_thread_event_t dte)
{
#if DISPATCH_LOCK_USE_SEMAPHORE_FALLBACK
	if (DISPATCH_LOCK_USE_SEMAPHORE_FALLBACK) {
		kern_return_t kr;
		do {
			kr = semaphore_wait(dte->dte_semaphore);
		} while (unlikely(kr == KERN_ABORTED));
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
		return;
	}
#endif
#if HAVE_UL_COMPARE_AND_WAIT || HAVE_FUTEX
	for (;;) {
		uint32_t value = os_atomic_load(&dte->dte_value, acquire);
		if (likely(value == 0)) return;
		if (unlikely(value != UINT32_MAX)) {
			DISPATCH_CLIENT_CRASH(value, "Corrupt thread event value");
		}
#if HAVE_UL_COMPARE_AND_WAIT
		int rc = _dispatch_ulock_wait(&dte->dte_value, UINT32_MAX, 0, 0);
		dispatch_assert(rc == 0 || rc == EFAULT);
#elif HAVE_FUTEX
		_dispatch_futex_wait(&dte->dte_value, UINT32_MAX,
				NULL, FUTEX_PRIVATE_FLAG);
#endif
	}
#elif USE_POSIX_SEM
	int rc;
	do {
		rc = sem_wait(&dte->dte_sem);
	} while (unlikely(rc != 0));
	DISPATCH_SEMAPHORE_VERIFY_RET(rc);
#endif
}

#pragma mark - unfair lock

#if HAVE_UL_UNFAIR_LOCK
void
_dispatch_unfair_lock_lock_slow(dispatch_unfair_lock_t dul,
		dispatch_lock_options_t flags)
{
	dispatch_lock tid_self = _dispatch_tid_self(), next = tid_self;
	dispatch_lock tid_old, tid_new;
	int rc;

	for (;;) {
		os_atomic_rmw_loop(&dul->dul_lock, tid_old, tid_new, acquire, {
			if (likely(!_dispatch_lock_is_locked(tid_old))) {
				tid_new = next;
			} else {
				tid_new = tid_old & ~DLOCK_NOWAITERS_BIT;
				if (tid_new == tid_old) os_atomic_rmw_loop_give_up(break);
			}
		});
		if (unlikely(_dispatch_lock_is_locked_by(tid_old, tid_self))) {
			DISPATCH_CLIENT_CRASH(0, "trying to lock recursively");
		}
		if (tid_new == next) {
			return;
		}
		rc = _dispatch_unfair_lock_wait(&dul->dul_lock, tid_new, 0, flags);
		if (rc == ENOTEMPTY) {
			next = tid_self & ~DLOCK_NOWAITERS_BIT;
		} else {
			next = tid_self;
		}
	}
}
#elif HAVE_FUTEX
void
_dispatch_unfair_lock_lock_slow(dispatch_unfair_lock_t dul,
		dispatch_lock_options_t flags)
{
	(void)flags;
	_dispatch_futex_lock_pi(&dul->dul_lock, NULL, 1, FUTEX_PRIVATE_FLAG);
}
#else
void
_dispatch_unfair_lock_lock_slow(dispatch_unfair_lock_t dul,
		dispatch_lock_options_t flags)
{
	dispatch_lock tid_cur, tid_self = _dispatch_tid_self();
	uint32_t timeout = 1;

	while (unlikely(!os_atomic_cmpxchgv(&dul->dul_lock,
			DLOCK_OWNER_NULL, tid_self, &tid_cur, acquire))) {
		if (unlikely(_dispatch_lock_is_locked_by(tid_cur, tid_self))) {
			DISPATCH_CLIENT_CRASH(0, "trying to lock recursively");
		}
		_dispatch_thread_switch(tid_cur, flags, timeout++);
	}
}
#endif

void
_dispatch_unfair_lock_unlock_slow(dispatch_unfair_lock_t dul,
		dispatch_lock tid_cur)
{
	dispatch_lock_owner tid_self = _dispatch_tid_self();
	if (unlikely(!_dispatch_lock_is_locked_by(tid_cur, tid_self))) {
		DISPATCH_CLIENT_CRASH(tid_cur, "lock not owned by current thread");
	}

#if HAVE_UL_UNFAIR_LOCK
	if (!(tid_cur & DLOCK_NOWAITERS_BIT)) {
		_dispatch_unfair_lock_wake(&dul->dul_lock, 0);
	}
#elif HAVE_FUTEX
	// futex_unlock_pi() handles both OWNER_DIED which we abuse & WAITERS
	_dispatch_futex_unlock_pi(&dul->dul_lock, FUTEX_PRIVATE_FLAG);
#else
	(void)dul;
#endif
}

#pragma mark - gate lock

void
_dispatch_gate_wait_slow(dispatch_gate_t dgl, dispatch_lock value,
		dispatch_lock_options_t flags)
{
	dispatch_lock tid_self = _dispatch_tid_self(), tid_old, tid_new;
	uint32_t timeout = 1;

	for (;;) {
		os_atomic_rmw_loop(&dgl->dgl_lock, tid_old, tid_new, acquire, {
			if (likely(tid_old == value)) {
				os_atomic_rmw_loop_give_up_with_fence(acquire, return);
			}
#ifdef DLOCK_NOWAITERS_BIT
			tid_new = tid_old & ~DLOCK_NOWAITERS_BIT;
#else
			tid_new = tid_old | DLOCK_WAITERS_BIT;
#endif
			if (tid_new == tid_old) os_atomic_rmw_loop_give_up(break);
		});
		if (unlikely(_dispatch_lock_is_locked_by(tid_old, tid_self))) {
			DISPATCH_CLIENT_CRASH(0, "trying to lock recursively");
		}
#if HAVE_UL_UNFAIR_LOCK
		_dispatch_unfair_lock_wait(&dgl->dgl_lock, tid_new, 0, flags);
#elif HAVE_FUTEX
		_dispatch_futex_wait(&dgl->dgl_lock, tid_new, NULL, FUTEX_PRIVATE_FLAG);
#else
		_dispatch_thread_switch(tid_new, flags, timeout++);
#endif
		(void)timeout;
	}
}

void
_dispatch_gate_broadcast_slow(dispatch_gate_t dgl, dispatch_lock tid_cur)
{
	dispatch_lock_owner tid_self = _dispatch_tid_self();
	if (unlikely(!_dispatch_lock_is_locked_by(tid_cur, tid_self))) {
		DISPATCH_CLIENT_CRASH(tid_cur, "lock not owned by current thread");
	}

#if HAVE_UL_UNFAIR_LOCK
	_dispatch_unfair_lock_wake(&dgl->dgl_lock, ULF_WAKE_ALL);
#elif HAVE_FUTEX
	_dispatch_futex_wake(&dgl->dgl_lock, INT_MAX, FUTEX_PRIVATE_FLAG);
#else
	(void)dgl;
#endif
}
