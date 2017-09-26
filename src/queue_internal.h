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

#ifndef __DISPATCH_QUEUE_INTERNAL__
#define __DISPATCH_QUEUE_INTERNAL__

#ifndef __DISPATCH_INDIRECT__
#error "Please #include <dispatch/dispatch.h> instead of this file directly."
#include <dispatch/base.h> // for HeaderDoc
#endif

#if defined(__BLOCKS__) && !defined(DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES)
#define DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES 1 // <rdar://problem/10719357>
#endif

/* x86 & cortex-a8 have a 64 byte cacheline */
#define DISPATCH_CACHELINE_SIZE 64u
#define ROUND_UP_TO_CACHELINE_SIZE(x) \
		(((x) + (DISPATCH_CACHELINE_SIZE - 1u)) & \
		~(DISPATCH_CACHELINE_SIZE - 1u))
#define DISPATCH_CACHELINE_ALIGN \
		__attribute__((__aligned__(DISPATCH_CACHELINE_SIZE)))

#define DISPATCH_CACHELINE_PAD_SIZE(type) \
		(roundup(sizeof(type), DISPATCH_CACHELINE_SIZE) - sizeof(type))


#pragma mark -
#pragma mark dispatch_queue_t

DISPATCH_ENUM(dispatch_queue_flags, uint32_t,
	DQF_NONE                = 0x00000000,
	DQF_AUTORELEASE_ALWAYS  = 0x00010000,
	DQF_AUTORELEASE_NEVER   = 0x00020000,
#define _DQF_AUTORELEASE_MASK 0x00030000
	DQF_THREAD_BOUND        = 0x00040000, // queue is bound to a thread
	DQF_BARRIER_BIT         = 0x00080000, // queue is a barrier on its target
	DQF_TARGETED            = 0x00100000, // queue is targeted by another object
	DQF_LABEL_NEEDS_FREE    = 0x00200000, // queue label was strduped; need to free it
	DQF_CANNOT_TRYSYNC      = 0x00400000,
	DQF_RELEASED            = 0x00800000, // xref_cnt == -1
	DQF_LEGACY              = 0x01000000,

	// only applies to sources
	//
	// Assuming DSF_ARMED (a), DSF_DEFERRED_DELETE (p), DSF_DELETED (d):
	//
	// ---
	// a--
	//    source states for regular operations
	//    (delivering event / waiting for event)
	//
	// ap-
	//    Either armed for deferred deletion delivery, waiting for an EV_DELETE,
	//    and the next state will be -pd (EV_DELETE delivered),
	//    Or, a cancellation raced with an event delivery and failed
	//    (EINPROGRESS), and when the event delivery happens, the next state
	//    will be -p-.
	//
	// -pd
	//    Received EV_DELETE (from ap-), needs to unregister ds_refs, the muxnote
	//    is gone from the kernel. Next state will be --d.
	//
	// -p-
	//    Received an EV_ONESHOT event (from a--), or the delivery of an event
	//    causing the cancellation to fail with EINPROGRESS was delivered
	//    (from ap-). The muxnote still lives, next state will be --d.
	//
	// --d
	//    Final state of the source, the muxnote is gone from the kernel and
	//    ds_refs is unregistered. The source can safely be released.
	//
	// a-d (INVALID)
	// apd (INVALID)
	//    Setting DSF_DELETED should also always atomically clear DSF_ARMED. If
	//    the muxnote is gone from the kernel, it makes no sense whatsoever to
	//    have it armed. And generally speaking, once `d` or `p` has been set,
	//    `a` cannot do a cleared -> set transition anymore
	//    (see _dispatch_source_try_set_armed).
	//
	DSF_WLH_CHANGED         = 0x04000000,
	DSF_CANCEL_WAITER       = 0x08000000, // synchronous waiters for cancel
	DSF_CANCELED            = 0x10000000, // cancellation has been requested
	DSF_ARMED               = 0x20000000, // source is armed
	DSF_DEFERRED_DELETE     = 0x40000000, // source is pending delete
	DSF_DELETED             = 0x80000000, // source muxnote is deleted
#define DSF_STATE_MASK (DSF_ARMED | DSF_DEFERRED_DELETE | DSF_DELETED)

#define DQF_FLAGS_MASK        ((dispatch_queue_flags_t)0xffff0000)
#define DQF_WIDTH_MASK        ((dispatch_queue_flags_t)0x0000ffff)
#define DQF_WIDTH(n)          ((dispatch_queue_flags_t)(uint16_t)(n))
);

#define _DISPATCH_QUEUE_HEADER(x) \
	struct os_mpsc_queue_s _as_oq[0]; \
	DISPATCH_OBJECT_HEADER(x); \
	_OS_MPSC_QUEUE_FIELDS(dq, dq_state); \
	uint32_t dq_side_suspend_cnt; \
	dispatch_unfair_lock_s dq_sidelock; \
	union { \
		dispatch_queue_t dq_specific_q; \
		struct dispatch_source_refs_s *ds_refs; \
		struct dispatch_timer_source_refs_s *ds_timer_refs; \
		struct dispatch_mach_recv_refs_s *dm_recv_refs; \
	}; \
	DISPATCH_UNION_LE(uint32_t volatile dq_atomic_flags, \
		const uint16_t dq_width, \
		const uint16_t __dq_opaque \
	); \
	DISPATCH_INTROSPECTION_QUEUE_HEADER
	/* LP64: 32bit hole */

#define DISPATCH_QUEUE_HEADER(x) \
	struct dispatch_queue_s _as_dq[0]; \
	_DISPATCH_QUEUE_HEADER(x)

struct _dispatch_unpadded_queue_s {
	_DISPATCH_QUEUE_HEADER(dummy);
};

#define DISPATCH_QUEUE_CACHELINE_PAD \
		DISPATCH_CACHELINE_PAD_SIZE(struct _dispatch_unpadded_queue_s)

#define DISPATCH_QUEUE_CACHELINE_PADDING \
		char _dq_pad[DISPATCH_QUEUE_CACHELINE_PAD]

/*
 * dispatch queues `dq_state` demystified
 *
 *******************************************************************************
 *
 * Most Significant 32 bit Word
 * ----------------------------
 *
 * sc: suspend count (bits 63 - 58)
 *    The suspend count unsurprisingly holds the suspend count of the queue
 *    Only 7 bits are stored inline. Extra counts are transfered in a side
 *    suspend count and when that has happened, the ssc: bit is set.
 */
#define DISPATCH_QUEUE_SUSPEND_INTERVAL		0x0400000000000000ull
#define DISPATCH_QUEUE_SUSPEND_HALF			0x20u
/*
 * ssc: side suspend count (bit 57)
 *    This bit means that the total suspend count didn't fit in the inline
 *    suspend count, and that there are additional suspend counts stored in the
 *    `dq_side_suspend_cnt` field.
 */
#define DISPATCH_QUEUE_HAS_SIDE_SUSPEND_CNT	0x0200000000000000ull
/*
 * i: inactive bit (bit 56)
 *    This bit means that the object is inactive (see dispatch_activate)
 */
#define DISPATCH_QUEUE_INACTIVE				0x0100000000000000ull
/*
 * na: needs activation (bit 55)
 *    This bit is set if the object is created inactive. It tells
 *    dispatch_queue_wakeup to perform various tasks at first wakeup.
 *
 *    This bit is cleared as part of the first wakeup. Having that bit prevents
 *    the object from being woken up (because _dq_state_should_wakeup will say
 *    no), except in the dispatch_activate/dispatch_resume codepath.
 */
#define DISPATCH_QUEUE_NEEDS_ACTIVATION		0x0080000000000000ull
/*
 * This mask covers the suspend count (sc), side suspend count bit (ssc),
 * inactive (i) and needs activation (na) bits
 */
#define DISPATCH_QUEUE_SUSPEND_BITS_MASK	0xff80000000000000ull
/*
 * ib: in barrier (bit 54)
 *    This bit is set when the queue is currently executing a barrier
 */
#define DISPATCH_QUEUE_IN_BARRIER			0x0040000000000000ull
/*
 * qf: queue full (bit 53)
 *    This bit is a subtle hack that allows to check for any queue width whether
 *    the full width of the queue is used or reserved (depending on the context)
 *    In other words that the queue has reached or overflown its capacity.
 */
#define DISPATCH_QUEUE_WIDTH_FULL_BIT		0x0020000000000000ull
#define DISPATCH_QUEUE_WIDTH_FULL			0x1000ull
#define DISPATCH_QUEUE_WIDTH_POOL (DISPATCH_QUEUE_WIDTH_FULL - 1)
#define DISPATCH_QUEUE_WIDTH_MAX  (DISPATCH_QUEUE_WIDTH_FULL - 2)
#define DISPATCH_QUEUE_USES_REDIRECTION(width) \
		({ uint16_t _width = (width); \
		_width > 1 && _width < DISPATCH_QUEUE_WIDTH_POOL; })
/*
 * w:  width (bits 52 - 41)
 *    This encodes how many work items are in flight. Barriers hold `dq_width`
 *    of them while they run. This is encoded as a signed offset with respect,
 *    to full use, where the negative values represent how many available slots
 *    are left, and the positive values how many work items are exceeding our
 *    capacity.
 *
 *    When this value is positive, then `wo` is always set to 1.
 */
#define DISPATCH_QUEUE_WIDTH_INTERVAL		0x0000020000000000ull
#define DISPATCH_QUEUE_WIDTH_MASK			0x003ffe0000000000ull
#define DISPATCH_QUEUE_WIDTH_SHIFT			41
/*
 * pb: pending barrier (bit 40)
 *    Drainers set this bit when they couldn't run the next work item and it is
 *    a barrier. When this bit is set, `dq_width - 1` work item slots are
 *    reserved so that no wakeup happens until the last work item in flight
 *    completes.
 */
#define DISPATCH_QUEUE_PENDING_BARRIER		0x0000010000000000ull
/*
 * d: dirty bit (bit 39)
 *    This bit is set when a queue transitions from empty to not empty.
 *    This bit is set before dq_items_head is set, with appropriate barriers.
 *    Any thread looking at a queue head is responsible for unblocking any
 *    dispatch_*_sync that could be enqueued at the beginning.
 *
 *    Drainer perspective
 *    ===================
 *
 *    When done, any "Drainer", in particular for dispatch_*_sync() handoff
 *    paths, exits in 3 steps, and the point of the DIRTY bit is to make
 *    the Drainers take the slowpath at step 2 to take into account enqueuers
 *    that could have made the queue non idle concurrently.
 *
 *    <code>
 *        // drainer-exit step 1
 *        if (slowpath(dq->dq_items_tail)) { // speculative test
 *            return handle_non_empty_queue_or_wakeup(dq);
 *        }
 *        // drainer-exit step 2
 *        if (!_dispatch_queue_drain_try_unlock(dq, ${owned}, ...)) {
 *            return handle_non_empty_queue_or_wakeup(dq);
 *        }
 *        // drainer-exit step 3
 *        // no need to wake up the queue, it's really empty for sure
 *        return;
 *    </code>
 *
 *    The crux is _dispatch_queue_drain_try_unlock(), it is a function whose
 *    contract is to release everything the current thread owns from the queue
 *    state, so that when it's successful, any other thread can acquire
 *    width from that queue.
 *
 *    But, that function must fail if it sees the DIRTY bit set, leaving
 *    the state untouched. Leaving the state untouched is vital as it ensures
 *    that no other Slayer^WDrainer can rise at the same time, because the
 *    resource stays locked.
 *
 *
 *    Note that releasing the DRAIN_LOCK or ENQUEUE_LOCK (see below) currently
 *    doesn't use that pattern, and always tries to requeue. It isn't a problem
 *    because while holding either of these locks prevents *some* sync (the
 *    barrier one) codepaths to acquire the resource, the retry they perform
 *    at their step D (see just below) isn't affected by the state of these bits
 *    at all.
 *
 *
 *    Sync items perspective
 *    ======================
 *
 *    On the dispatch_*_sync() acquire side, the code must look like this:
 *
 *    <code>
 *        // step A
 *        if (try_acquire_sync(dq)) {
 *            return sync_operation_fastpath(dq, item);
 *        }
 *
 *        // step B
 *        if (queue_push_and_inline(dq, item)) {
 *            atomic_store(dq->dq_items_head, item, relaxed);
 *            // step C
 *            atomic_or(dq->dq_state, DIRTY, release);
 *
 *            // step D
 *            if (try_acquire_sync(dq)) {
 *                try_lock_transfer_or_wakeup(dq);
 *            }
 *        }
 *
 *        // step E
 *        wait_for_lock_transfer(dq);
 *    </code>
 *
 *    A. If this code can acquire the resource it needs at step A, we're good.
 *
 *    B. If the item isn't the first at enqueue time, then there is no issue
 *       At least another thread went through C, this thread isn't interesting
 *       for the possible races, responsibility to make progress is transfered
 *       to the thread which went through C-D.
 *
 *    C. The DIRTY bit is set with a release barrier, after the head/tail
 *       has been set, so that seeing the DIRTY bit means that head/tail
 *       will be visible to any drainer that has the matching acquire barrier.
 *
 *       Drainers may see the head/tail and fail to see DIRTY, in which
 *       case, their _dispatch_queue_drain_try_unlock() will clear the DIRTY
 *       bit, and fail, causing the caller to retry exactly once.
 *
 *    D. At this stage, there's two possible outcomes:
 *
 *       - either the acquire works this time, in which case this thread
 *         successfuly becomes a drainer. That's obviously the happy path.
 *         It means all drainers are after Step 2 (or there is no Drainer)
 *
 *       - or the acquire fails, which means that another drainer is before
 *         its Step 2. Since we set the DIRTY bit on the dq_state by now,
 *         and that drainers manipulate the state atomically, at least one
 *         drainer that is still before its step 2 will fail its step 2, and
 *         be responsible for making progress.
 *
 *
 *    Async items perspective
 *    ======================
 *
 *    On the async codepath, when the queue becomes non empty, the queue
 *    is always woken up. There is no point in trying to avoid that wake up
 *    for the async case, because it's required for the async()ed item to make
 *    progress: a drain of the queue must happen.
 *
 *    So on the async "acquire" side, there is no subtlety at all.
 */
#define DISPATCH_QUEUE_DIRTY				0x0000008000000000ull
/*
 * md: enqueued/draining on manager (bit 38)
 *    Set when enqueued and draining on the manager hierarchy.
 *
 *    Unlike the ENQUEUED bit, it is kept until the queue is unlocked from its
 *    invoke call on the manager. This is used to prevent stealing, and
 *    overrides to be applied down the target queue chain.
 */
#define DISPATCH_QUEUE_ENQUEUED_ON_MGR		0x0000004000000000ull
/*
 * r: queue graph role (bits 37 - 36)
 *    Queue role in the target queue graph
 *
 *    11: unused
 *    10: WLH base
 *    01: non wlh base
 *    00: inner queue
 */
#define DISPATCH_QUEUE_ROLE_MASK			0x0000003000000000ull
#define DISPATCH_QUEUE_ROLE_BASE_WLH		0x0000002000000000ull
#define DISPATCH_QUEUE_ROLE_BASE_ANON		0x0000001000000000ull
#define DISPATCH_QUEUE_ROLE_INNER			0x0000000000000000ull
/*
 * o: has override (bit 35, if role is DISPATCH_QUEUE_ROLE_BASE_ANON)
 *    Set when a queue has received a QOS override and needs to reset it.
 *    This bit is only cleared when the final drain_try_unlock() succeeds.
 *
 * sw: has received sync wait (bit 35, if role DISPATCH_QUEUE_ROLE_BASE_WLH)
 *    Set when a queue owner has been exposed to the kernel because of
 *    dispatch_sync() contention.
 */
#define DISPATCH_QUEUE_RECEIVED_OVERRIDE	0x0000000800000000ull
#define DISPATCH_QUEUE_RECEIVED_SYNC_WAIT	0x0000000800000000ull
/*
 * max_qos: max qos (bits 34 - 32)
 *   This is the maximum qos that has been enqueued on the queue
 */
#define DISPATCH_QUEUE_MAX_QOS_MASK			0x0000000700000000ull
#define DISPATCH_QUEUE_MAX_QOS_SHIFT		32
/*
 * dl: drain lock (bits 31-0)
 *    This is used by the normal drain to drain exlusively relative to other
 *    drain stealers (like the QoS Override codepath). It holds the identity
 *    (thread port) of the current drainer.
 *
 * st: sync transfer (bit 1 or 30)
 *    Set when a dispatch_sync() is transferred to
 *
 * e: enqueued bit (bit 0 or 31)
 *    Set when a queue is enqueued on its target queue
 */
#define DISPATCH_QUEUE_DRAIN_OWNER_MASK		((uint64_t)DLOCK_OWNER_MASK)
#define DISPATCH_QUEUE_SYNC_TRANSFER		((uint64_t)DLOCK_FAILED_TRYLOCK_BIT)
#define DISPATCH_QUEUE_ENQUEUED				((uint64_t)DLOCK_WAITERS_BIT)

#define DISPATCH_QUEUE_DRAIN_PRESERVED_BITS_MASK \
		(DISPATCH_QUEUE_ENQUEUED_ON_MGR | DISPATCH_QUEUE_ENQUEUED | \
		DISPATCH_QUEUE_ROLE_MASK | DISPATCH_QUEUE_MAX_QOS_MASK)

#define DISPATCH_QUEUE_DRAIN_UNLOCK_MASK \
		(DISPATCH_QUEUE_DRAIN_OWNER_MASK | DISPATCH_QUEUE_RECEIVED_OVERRIDE | \
		DISPATCH_QUEUE_RECEIVED_SYNC_WAIT | DISPATCH_QUEUE_SYNC_TRANSFER)

/*
 *******************************************************************************
 *
 * `Drainers`
 *
 * Drainers are parts of the code that hold the drain lock by setting its value
 * to their thread port. There are two kinds:
 * 1. async drainers,
 * 2. lock transfer handlers.
 *
 * Drainers from the first category are _dispatch_queue_class_invoke and its
 * stealers. Those drainers always try to reserve width at the same time they
 * acquire the drain lock, to make sure they can make progress, and else exit
 * quickly.
 *
 * Drainers from the second category are `slow` work items. Those run on the
 * calling thread, and when done, try to transfer the width they own to the
 * possible next `slow` work item, and if there is no such item, they reliquish
 * that right. To do so, prior to taking any decision, they also try to own
 * the full "barrier" width on the given queue.
 *
 *******************************************************************************
 *
 * Enqueuing and wakeup rules
 *
 * Nobody should enqueue any dispatch object if it has no chance to make any
 * progress. That means that queues that:
 * - are suspended
 * - have reached or overflown their capacity
 * - are currently draining
 * - are already enqueued
 *
 * should not try to be enqueued.
 *
 *******************************************************************************
 *
 * Lock transfer
 *
 * The point of the lock transfer code is to allow pure dispatch_*_sync()
 * callers to make progress without requiring the bring up of a drainer.
 * There are two reason for that:
 *
 * - performance, as draining has to give up for dispatch_*_sync() work items,
 *   so waking up a queue for this is wasteful.
 *
 * - liveness, as with dispatch_*_sync() you burn threads waiting, you're more
 *   likely to hit various thread limits and may not have any drain being
 *   brought up if the process hits a limit.
 *
 *
 * Lock transfer happens at the end on the dispatch_*_sync() codepaths:
 *
 * - obviously once a dispatch_*_sync() work item finishes, it owns queue
 *   width and it should try to transfer that ownership to the possible next
 *   queued item if it is a dispatch_*_sync() item
 *
 * - just before such a work item blocks to make sure that that work item
 *   itself isn't its own last chance to be woken up. That can happen when
 *   a Drainer pops up everything from the queue, and that a dispatch_*_sync()
 *   work item has taken the slow path then was preempted for a long time.
 *
 *   That's why such work items, if first in the queue, must try a lock
 *   transfer procedure.
 *
 *
 * For transfers where a partial width is owned, we give back that width.
 * If the queue state is "idle" again, we attempt to acquire the full width.
 * If that succeeds, this falls back to the full barrier lock
 * transfer, else it wakes up the queue according to its state.
 *
 * For full barrier transfers, if items eligible for lock transfer are found,
 * then they are woken up and the lock transfer is successful.
 *
 * If none are found, the full barrier width is released. If by doing so the
 * DIRTY bit is found, releasing the full barrier width fails and transferring
 * the lock is retried from scratch.
 */

#define DISPATCH_QUEUE_STATE_INIT_VALUE(width) \
		((DISPATCH_QUEUE_WIDTH_FULL - (width)) << DISPATCH_QUEUE_WIDTH_SHIFT)

/* Magic dq_state values for global queues: they have QUEUE_FULL and IN_BARRIER
 * set to force the slowpath in both dispatch_barrier_sync() and dispatch_sync()
 */
#define DISPATCH_ROOT_QUEUE_STATE_INIT_VALUE \
		(DISPATCH_QUEUE_WIDTH_FULL_BIT | DISPATCH_QUEUE_IN_BARRIER)

#define DISPATCH_QUEUE_SERIAL_DRAIN_OWNED \
		(DISPATCH_QUEUE_IN_BARRIER | DISPATCH_QUEUE_WIDTH_INTERVAL)

DISPATCH_CLASS_DECL(queue);

#if !defined(__cplusplus) || !DISPATCH_INTROSPECTION
struct dispatch_queue_s {
	_DISPATCH_QUEUE_HEADER(queue);
	DISPATCH_QUEUE_CACHELINE_PADDING; // for static queues only
} DISPATCH_ATOMIC64_ALIGN;

#if __has_feature(c_static_assert) && !DISPATCH_INTROSPECTION
_Static_assert(sizeof(struct dispatch_queue_s) <= 128, "dispatch queue size");
#endif
#endif // !defined(__cplusplus) || !DISPATCH_INTROSPECTION

DISPATCH_INTERNAL_SUBCLASS_DECL(queue_serial, queue);
DISPATCH_INTERNAL_SUBCLASS_DECL(queue_concurrent, queue);
DISPATCH_INTERNAL_SUBCLASS_DECL(queue_main, queue);
DISPATCH_INTERNAL_SUBCLASS_DECL(queue_root, queue);
DISPATCH_INTERNAL_SUBCLASS_DECL(queue_runloop, queue);
DISPATCH_INTERNAL_SUBCLASS_DECL(queue_mgr, queue);

OS_OBJECT_INTERNAL_CLASS_DECL(dispatch_queue_specific_queue, dispatch_queue,
		DISPATCH_OBJECT_VTABLE_HEADER(dispatch_queue_specific_queue));

typedef union {
	struct os_mpsc_queue_s *_oq;
	struct dispatch_queue_s *_dq;
	struct dispatch_source_s *_ds;
	struct dispatch_mach_s *_dm;
	struct dispatch_queue_specific_queue_s *_dqsq;
#if USE_OBJC
	os_mpsc_queue_t _ojbc_oq;
	dispatch_queue_t _objc_dq;
	dispatch_source_t _objc_ds;
	dispatch_mach_t _objc_dm;
	dispatch_queue_specific_queue_t _objc_dqsq;
#endif
} dispatch_queue_class_t DISPATCH_TRANSPARENT_UNION;

typedef struct dispatch_thread_context_s *dispatch_thread_context_t;
typedef struct dispatch_thread_context_s {
	dispatch_thread_context_t dtc_prev;
	const void *dtc_key;
	union {
		size_t dtc_apply_nesting;
		dispatch_io_t dtc_io_in_barrier;
	};
} dispatch_thread_context_s;

typedef struct dispatch_thread_frame_s *dispatch_thread_frame_t;
typedef struct dispatch_thread_frame_s {
	// must be in the same order as our TSD keys!
	dispatch_queue_t dtf_queue;
	dispatch_thread_frame_t dtf_prev;
} dispatch_thread_frame_s;

typedef dispatch_queue_t dispatch_queue_wakeup_target_t;
#define DISPATCH_QUEUE_WAKEUP_NONE           ((dispatch_queue_wakeup_target_t)0)
#define DISPATCH_QUEUE_WAKEUP_TARGET         ((dispatch_queue_wakeup_target_t)1)
#define DISPATCH_QUEUE_WAKEUP_MGR            (&_dispatch_mgr_q)
#define DISPATCH_QUEUE_WAKEUP_WAIT_FOR_EVENT ((dispatch_queue_wakeup_target_t)-1)

void _dispatch_queue_class_wakeup(dispatch_queue_t dqu, dispatch_qos_t qos,
		dispatch_wakeup_flags_t flags, dispatch_queue_wakeup_target_t target);
dispatch_priority_t _dispatch_queue_compute_priority_and_wlh(
		dispatch_queue_t dq, dispatch_wlh_t *wlh_out);
void _dispatch_queue_destroy(dispatch_queue_t dq, bool *allow_free);
void _dispatch_queue_dispose(dispatch_queue_t dq, bool *allow_free);
void _dispatch_queue_xref_dispose(struct dispatch_queue_s *dq);
void _dispatch_queue_set_target_queue(dispatch_queue_t dq, dispatch_queue_t tq);
void _dispatch_queue_suspend(dispatch_queue_t dq);
void _dispatch_queue_resume(dispatch_queue_t dq, bool activate);
void _dispatch_queue_finalize_activation(dispatch_queue_t dq,
		bool *allow_resume);
void _dispatch_queue_invoke(dispatch_queue_t dq,
		dispatch_invoke_context_t dic, dispatch_invoke_flags_t flags);
void _dispatch_global_queue_poke(dispatch_queue_t dq, int n, int floor);
void _dispatch_queue_push(dispatch_queue_t dq, dispatch_object_t dou,
		dispatch_qos_t qos);
void _dispatch_queue_wakeup(dispatch_queue_t dq, dispatch_qos_t qos,
		dispatch_wakeup_flags_t flags);
dispatch_queue_wakeup_target_t _dispatch_queue_serial_drain(dispatch_queue_t dq,
		dispatch_invoke_context_t dic, dispatch_invoke_flags_t flags,
		uint64_t *owned);
void _dispatch_queue_drain_sync_waiter(dispatch_queue_t dq,
		dispatch_invoke_context_t dic, dispatch_invoke_flags_t flags,
		uint64_t owned);
void _dispatch_queue_specific_queue_dispose(
		dispatch_queue_specific_queue_t dqsq, bool *allow_free);
void _dispatch_root_queue_wakeup(dispatch_queue_t dq, dispatch_qos_t qos,
		dispatch_wakeup_flags_t flags);
void _dispatch_root_queue_push(dispatch_queue_t dq, dispatch_object_t dou,
		dispatch_qos_t qos);
#if DISPATCH_USE_KEVENT_WORKQUEUE
void _dispatch_root_queue_drain_deferred_item(dispatch_deferred_items_t ddi
		DISPATCH_PERF_MON_ARGS_PROTO);
void _dispatch_root_queue_drain_deferred_wlh(dispatch_deferred_items_t ddi
		DISPATCH_PERF_MON_ARGS_PROTO);
#endif
void _dispatch_pthread_root_queue_dispose(dispatch_queue_t dq,
		bool *allow_free);
void _dispatch_main_queue_wakeup(dispatch_queue_t dq, dispatch_qos_t qos,
		dispatch_wakeup_flags_t flags);
void _dispatch_runloop_queue_wakeup(dispatch_queue_t dq, dispatch_qos_t qos,
		dispatch_wakeup_flags_t flags);
void _dispatch_runloop_queue_xref_dispose(dispatch_queue_t dq);
void _dispatch_runloop_queue_dispose(dispatch_queue_t dq, bool *allow_free);
void _dispatch_mgr_queue_drain(void);
#if DISPATCH_USE_MGR_THREAD && DISPATCH_ENABLE_PTHREAD_ROOT_QUEUES
void _dispatch_mgr_priority_init(void);
#else
static inline void _dispatch_mgr_priority_init(void) {}
#endif
#if DISPATCH_USE_KEVENT_WORKQUEUE
void _dispatch_kevent_workqueue_init(void);
#else
static inline void _dispatch_kevent_workqueue_init(void) {}
#endif
void _dispatch_apply_invoke(void *ctxt);
void _dispatch_apply_redirect_invoke(void *ctxt);
void _dispatch_barrier_async_detached_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func);
#define DISPATCH_BARRIER_TRYSYNC_SUSPEND 0x1
void _dispatch_barrier_trysync_or_async_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func, uint32_t flags);
void _dispatch_queue_atfork_child(void);

#if DISPATCH_DEBUG
void dispatch_debug_queue(dispatch_queue_t dq, const char* str);
#else
static inline void dispatch_debug_queue(dispatch_queue_t dq DISPATCH_UNUSED,
		const char* str DISPATCH_UNUSED) {}
#endif

size_t dispatch_queue_debug(dispatch_queue_t dq, char* buf, size_t bufsiz);
size_t _dispatch_queue_debug_attr(dispatch_queue_t dq, char* buf,
		size_t bufsiz);

#define DISPATCH_ROOT_QUEUE_COUNT (DISPATCH_QOS_MAX * 2)

// must be in lowest to highest qos order (as encoded in dispatch_qos_t)
// overcommit qos index values need bit 1 set
enum {
	DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS = 0,
	DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS_OVERCOMMIT,
	DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS,
	DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS_OVERCOMMIT,
	DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS,
	DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS_OVERCOMMIT,
	DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS,
	DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS_OVERCOMMIT,
	DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS,
	DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS_OVERCOMMIT,
	DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS,
	DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS_OVERCOMMIT,
	_DISPATCH_ROOT_QUEUE_IDX_COUNT,
};

// skip zero
// 1 - main_q
// 2 - mgr_q
// 3 - mgr_root_q
// 4,5,6,7,8,9,10,11,12,13,14,15 - global queues
// we use 'xadd' on Intel, so the initial value == next assigned
#define DISPATCH_QUEUE_SERIAL_NUMBER_INIT 16
extern unsigned long volatile _dispatch_queue_serial_numbers;
extern struct dispatch_queue_s _dispatch_root_queues[];
extern struct dispatch_queue_s _dispatch_mgr_q;
void _dispatch_root_queues_init(void);

#if DISPATCH_DEBUG
#define DISPATCH_ASSERT_ON_MANAGER_QUEUE() \
       dispatch_assert_queue(&_dispatch_mgr_q)
#else
#define DISPATCH_ASSERT_ON_MANAGER_QUEUE()
#endif

#pragma mark -
#pragma mark dispatch_queue_attr_t

typedef enum {
	_dispatch_queue_attr_overcommit_unspecified = 0,
	_dispatch_queue_attr_overcommit_enabled,
	_dispatch_queue_attr_overcommit_disabled,
} _dispatch_queue_attr_overcommit_t;

DISPATCH_CLASS_DECL(queue_attr);
struct dispatch_queue_attr_s {
	OS_OBJECT_STRUCT_HEADER(dispatch_queue_attr);
	dispatch_priority_requested_t dqa_qos_and_relpri;
	uint16_t dqa_overcommit:2;
	uint16_t dqa_autorelease_frequency:2;
	uint16_t dqa_concurrent:1;
	uint16_t dqa_inactive:1;
};

enum {
	DQA_INDEX_UNSPECIFIED_OVERCOMMIT = 0,
	DQA_INDEX_NON_OVERCOMMIT,
	DQA_INDEX_OVERCOMMIT,
};

#define DISPATCH_QUEUE_ATTR_OVERCOMMIT_COUNT 3

enum {
	DQA_INDEX_AUTORELEASE_FREQUENCY_INHERIT =
			DISPATCH_AUTORELEASE_FREQUENCY_INHERIT,
	DQA_INDEX_AUTORELEASE_FREQUENCY_WORK_ITEM =
			DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM,
	DQA_INDEX_AUTORELEASE_FREQUENCY_NEVER =
			DISPATCH_AUTORELEASE_FREQUENCY_NEVER,
};

#define DISPATCH_QUEUE_ATTR_AUTORELEASE_FREQUENCY_COUNT 3

enum {
	DQA_INDEX_CONCURRENT = 0,
	DQA_INDEX_SERIAL,
};

#define DISPATCH_QUEUE_ATTR_CONCURRENCY_COUNT 2

enum {
	DQA_INDEX_ACTIVE = 0,
	DQA_INDEX_INACTIVE,
};

#define DISPATCH_QUEUE_ATTR_INACTIVE_COUNT 2

typedef enum {
	DQA_INDEX_QOS_CLASS_UNSPECIFIED = 0,
	DQA_INDEX_QOS_CLASS_MAINTENANCE,
	DQA_INDEX_QOS_CLASS_BACKGROUND,
	DQA_INDEX_QOS_CLASS_UTILITY,
	DQA_INDEX_QOS_CLASS_DEFAULT,
	DQA_INDEX_QOS_CLASS_USER_INITIATED,
	DQA_INDEX_QOS_CLASS_USER_INTERACTIVE,
} _dispatch_queue_attr_index_qos_class_t;

#define DISPATCH_QUEUE_ATTR_PRIO_COUNT (1 - QOS_MIN_RELATIVE_PRIORITY)

extern const struct dispatch_queue_attr_s _dispatch_queue_attrs[]
		[DISPATCH_QUEUE_ATTR_PRIO_COUNT]
		[DISPATCH_QUEUE_ATTR_OVERCOMMIT_COUNT]
		[DISPATCH_QUEUE_ATTR_AUTORELEASE_FREQUENCY_COUNT]
		[DISPATCH_QUEUE_ATTR_CONCURRENCY_COUNT]
		[DISPATCH_QUEUE_ATTR_INACTIVE_COUNT];

dispatch_queue_attr_t _dispatch_get_default_queue_attr(void);

#pragma mark -
#pragma mark dispatch_continuation_t

// If dc_flags is less than 0x1000, then the object is a continuation.
// Otherwise, the object has a private layout and memory management rules. The
// layout until after 'do_next' must align with normal objects.
#if __LP64__
#define DISPATCH_CONTINUATION_HEADER(x) \
	union { \
		const void *do_vtable; \
		uintptr_t dc_flags; \
	}; \
	union { \
		pthread_priority_t dc_priority; \
		int dc_cache_cnt; \
		uintptr_t dc_pad; \
	}; \
	struct dispatch_##x##_s *volatile do_next; \
	struct voucher_s *dc_voucher; \
	dispatch_function_t dc_func; \
	void *dc_ctxt; \
	void *dc_data; \
	void *dc_other
#elif OS_OBJECT_HAVE_OBJC1
#define DISPATCH_CONTINUATION_HEADER(x) \
	dispatch_function_t dc_func; \
	union { \
		pthread_priority_t dc_priority; \
		int dc_cache_cnt; \
		uintptr_t dc_pad; \
	}; \
	struct voucher_s *dc_voucher; \
	union { \
		const void *do_vtable; \
		uintptr_t dc_flags; \
	}; \
	struct dispatch_##x##_s *volatile do_next; \
	void *dc_ctxt; \
	void *dc_data; \
	void *dc_other
#else
#define DISPATCH_CONTINUATION_HEADER(x) \
	union { \
		const void *do_vtable; \
		uintptr_t dc_flags; \
	}; \
	union { \
		pthread_priority_t dc_priority; \
		int dc_cache_cnt; \
		uintptr_t dc_pad; \
	}; \
	struct voucher_s *dc_voucher; \
	struct dispatch_##x##_s *volatile do_next; \
	dispatch_function_t dc_func; \
	void *dc_ctxt; \
	void *dc_data; \
	void *dc_other
#endif
#define _DISPATCH_CONTINUATION_PTRS 8
#if DISPATCH_HW_CONFIG_UP
// UP devices don't contend on continuations so we don't need to force them to
// occupy a whole cacheline (which is intended to avoid contention)
#define DISPATCH_CONTINUATION_SIZE \
		(_DISPATCH_CONTINUATION_PTRS * DISPATCH_SIZEOF_PTR)
#else
#define DISPATCH_CONTINUATION_SIZE  ROUND_UP_TO_CACHELINE_SIZE( \
		(_DISPATCH_CONTINUATION_PTRS * DISPATCH_SIZEOF_PTR))
#endif
#define ROUND_UP_TO_CONTINUATION_SIZE(x) \
		(((x) + (DISPATCH_CONTINUATION_SIZE - 1u)) & \
		~(DISPATCH_CONTINUATION_SIZE - 1u))

// continuation is a dispatch_sync or dispatch_barrier_sync
#define DISPATCH_OBJ_SYNC_WAITER_BIT		0x001ul
// continuation acts as a barrier
#define DISPATCH_OBJ_BARRIER_BIT			0x002ul
// continuation resources are freed on run
// this is set on async or for non event_handler source handlers
#define DISPATCH_OBJ_CONSUME_BIT			0x004ul
// continuation has a group in dc_data
#define DISPATCH_OBJ_GROUP_BIT				0x008ul
// continuation function is a block (copied in dc_ctxt)
#define DISPATCH_OBJ_BLOCK_BIT				0x010ul
// continuation function is a block with private data, implies BLOCK_BIT
#define DISPATCH_OBJ_BLOCK_PRIVATE_DATA_BIT	0x020ul
// source handler requires fetching context from source
#define DISPATCH_OBJ_CTXT_FETCH_BIT			0x040ul
// use the voucher from the continuation even if the queue has voucher set
#define DISPATCH_OBJ_ENFORCE_VOUCHER		0x080ul
// never set on continuations, used by mach.c only
#define DISPATCH_OBJ_MACH_BARRIER		0x1000000ul

typedef struct dispatch_continuation_s {
	struct dispatch_object_s _as_do[0];
	DISPATCH_CONTINUATION_HEADER(continuation);
} *dispatch_continuation_t;

typedef struct dispatch_sync_context_s {
	struct dispatch_object_s _as_do[0];
	struct dispatch_continuation_s _as_dc[0];
	DISPATCH_CONTINUATION_HEADER(continuation);
	dispatch_function_t dsc_func;
	void *dsc_ctxt;
#if DISPATCH_COCOA_COMPAT
	dispatch_thread_frame_s dsc_dtf;
#endif
	dispatch_thread_event_s dsc_event;
	dispatch_tid dsc_waiter;
	dispatch_qos_t dsc_override_qos_floor;
	dispatch_qos_t dsc_override_qos;
	bool dsc_wlh_was_first;
	bool dsc_release_storage;
} *dispatch_sync_context_t;

typedef struct dispatch_continuation_vtable_s {
	_OS_OBJECT_CLASS_HEADER();
	DISPATCH_INVOKABLE_VTABLE_HEADER(dispatch_continuation);
} const *dispatch_continuation_vtable_t;

#ifndef DISPATCH_CONTINUATION_CACHE_LIMIT
#if TARGET_OS_EMBEDDED
#define DISPATCH_CONTINUATION_CACHE_LIMIT 112 // one 256k heap for 64 threads
#define DISPATCH_CONTINUATION_CACHE_LIMIT_MEMORYPRESSURE_PRESSURE_WARN 16
#else
#define DISPATCH_CONTINUATION_CACHE_LIMIT 1024
#define DISPATCH_CONTINUATION_CACHE_LIMIT_MEMORYPRESSURE_PRESSURE_WARN 128
#endif
#endif

dispatch_continuation_t _dispatch_continuation_alloc_from_heap(void);
void _dispatch_continuation_free_to_heap(dispatch_continuation_t c);
void _dispatch_continuation_async(dispatch_queue_t dq,
	dispatch_continuation_t dc);
void _dispatch_continuation_pop(dispatch_object_t dou,
		dispatch_invoke_context_t dic, dispatch_invoke_flags_t flags,
		dispatch_queue_t dq);
void _dispatch_continuation_invoke(dispatch_object_t dou,
		voucher_t override_voucher, dispatch_invoke_flags_t flags);

#if DISPATCH_USE_MEMORYPRESSURE_SOURCE
extern int _dispatch_continuation_cache_limit;
void _dispatch_continuation_free_to_cache_limit(dispatch_continuation_t c);
#else
#define _dispatch_continuation_cache_limit DISPATCH_CONTINUATION_CACHE_LIMIT
#define _dispatch_continuation_free_to_cache_limit(c) \
		_dispatch_continuation_free_to_heap(c)
#endif

#pragma mark -
#pragma mark dispatch_continuation vtables

enum {
	_DC_USER_TYPE = 0,
	DC_ASYNC_REDIRECT_TYPE,
	DC_MACH_SEND_BARRRIER_DRAIN_TYPE,
	DC_MACH_SEND_BARRIER_TYPE,
	DC_MACH_RECV_BARRIER_TYPE,
	DC_MACH_ASYNC_REPLY_TYPE,
#if HAVE_PTHREAD_WORKQUEUE_QOS
	DC_OVERRIDE_STEALING_TYPE,
	DC_OVERRIDE_OWNING_TYPE,
#endif
	_DC_MAX_TYPE,
};

DISPATCH_ALWAYS_INLINE
static inline unsigned long
dc_type(dispatch_continuation_t dc)
{
	return dx_type(dc->_as_do);
}

DISPATCH_ALWAYS_INLINE
static inline unsigned long
dc_subtype(dispatch_continuation_t dc)
{
	return dx_subtype(dc->_as_do);
}

extern const struct dispatch_continuation_vtable_s
		_dispatch_continuation_vtables[_DC_MAX_TYPE];

void
_dispatch_async_redirect_invoke(dispatch_continuation_t dc,
		dispatch_invoke_context_t dic, dispatch_invoke_flags_t flags);

#if HAVE_PTHREAD_WORKQUEUE_QOS
void
_dispatch_queue_override_invoke(dispatch_continuation_t dc,
		dispatch_invoke_context_t dic, dispatch_invoke_flags_t flags);
#endif

#define DC_VTABLE(name)  (&_dispatch_continuation_vtables[DC_##name##_TYPE])

#define DC_VTABLE_ENTRY(name, ...)  \
	[DC_##name##_TYPE] = { \
		.do_type = DISPATCH_CONTINUATION_TYPE(name), \
		__VA_ARGS__ \
	}

#pragma mark -
#pragma mark _dispatch_set_priority_and_voucher
#if HAVE_PTHREAD_WORKQUEUE_QOS

void _dispatch_set_priority_and_mach_voucher_slow(pthread_priority_t pri,
		mach_voucher_t kv);
voucher_t _dispatch_set_priority_and_voucher_slow(pthread_priority_t pri,
		voucher_t voucher, dispatch_thread_set_self_t flags);
#else
static inline void
_dispatch_set_priority_and_mach_voucher_slow(pthread_priority_t pri,
		mach_voucher_t kv)
{
	(void)pri; (void)kv;
}
#endif
#pragma mark -
#pragma mark dispatch_apply_t

struct dispatch_apply_s {
	size_t volatile da_index, da_todo;
	size_t da_iterations, da_nested;
	dispatch_continuation_t da_dc;
	dispatch_thread_event_s da_event;
	dispatch_invoke_flags_t da_flags;
	int32_t da_thr_cnt;
};
typedef struct dispatch_apply_s *dispatch_apply_t;

#pragma mark -
#pragma mark dispatch_block_t

#ifdef __BLOCKS__

#define DISPATCH_BLOCK_API_MASK (0x100u - 1)
#define DISPATCH_BLOCK_HAS_VOUCHER (1u << 31)
#define DISPATCH_BLOCK_HAS_PRIORITY (1u << 30)

#define DISPATCH_BLOCK_PRIVATE_DATA_HEADER() \
	unsigned long dbpd_magic; \
	dispatch_block_flags_t dbpd_flags; \
	unsigned int volatile dbpd_atomic_flags; \
	int volatile dbpd_performed; \
	pthread_priority_t dbpd_priority; \
	voucher_t dbpd_voucher; \
	dispatch_block_t dbpd_block; \
	dispatch_group_t dbpd_group; \
	os_mpsc_queue_t volatile dbpd_queue; \
	mach_port_t dbpd_thread;

#if !defined(__cplusplus)
struct dispatch_block_private_data_s {
	DISPATCH_BLOCK_PRIVATE_DATA_HEADER();
};
#endif
typedef struct dispatch_block_private_data_s *dispatch_block_private_data_t;

// dbpd_atomic_flags bits
#define DBF_CANCELED 1u // block has been cancelled
#define DBF_WAITING 2u // dispatch_block_wait has begun
#define DBF_WAITED 4u // dispatch_block_wait has finished without timeout
#define DBF_PERFORM 8u // dispatch_block_perform: don't group_leave

#define DISPATCH_BLOCK_PRIVATE_DATA_MAGIC 0xD159B10C // 0xDISPatch_BLOCk

// struct for synchronous perform: no group_leave at end of invoke
#define DISPATCH_BLOCK_PRIVATE_DATA_PERFORM_INITIALIZER(flags, block) \
		{ \
			.dbpd_magic = DISPATCH_BLOCK_PRIVATE_DATA_MAGIC, \
			.dbpd_flags = (flags), \
			.dbpd_atomic_flags = DBF_PERFORM, \
			.dbpd_block = (block), \
		}

dispatch_block_t _dispatch_block_create(dispatch_block_flags_t flags,
		voucher_t voucher, pthread_priority_t priority, dispatch_block_t block);
void _dispatch_block_invoke_direct(const struct dispatch_block_private_data_s *dbcpd);
void _dispatch_block_sync_invoke(void *block);

void _dispatch_continuation_init_slow(dispatch_continuation_t dc,
		dispatch_queue_class_t dqu, dispatch_block_flags_t flags);

long _dispatch_barrier_trysync_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t func);

/* exported for tests in dispatch_trysync.c */
DISPATCH_EXPORT DISPATCH_NOTHROW
long _dispatch_trysync_f(dispatch_queue_t dq, void *ctxt,
		dispatch_function_t f);

#endif /* __BLOCKS__ */

typedef struct dispatch_pthread_root_queue_observer_hooks_s {
	void (*queue_will_execute)(dispatch_queue_t queue);
	void (*queue_did_execute)(dispatch_queue_t queue);
} dispatch_pthread_root_queue_observer_hooks_s;
typedef dispatch_pthread_root_queue_observer_hooks_s
		*dispatch_pthread_root_queue_observer_hooks_t;

#ifdef __APPLE__
#define DISPATCH_IOHID_SPI 1

DISPATCH_EXPORT DISPATCH_MALLOC DISPATCH_RETURNS_RETAINED DISPATCH_WARN_RESULT
DISPATCH_NOTHROW DISPATCH_NONNULL4
dispatch_queue_t
_dispatch_pthread_root_queue_create_with_observer_hooks_4IOHID(
	const char *label, unsigned long flags, const pthread_attr_t *attr,
	dispatch_pthread_root_queue_observer_hooks_t observer_hooks,
	dispatch_block_t configure);

DISPATCH_EXPORT DISPATCH_PURE DISPATCH_WARN_RESULT DISPATCH_NOTHROW
bool
_dispatch_queue_is_exclusively_owned_by_current_thread_4IOHID(
		dispatch_queue_t queue);

#endif // __APPLE__

#endif
