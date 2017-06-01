/*
 * Copyright (c) 2015 Apple Inc. All rights reserved.
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

#ifndef __FIREHOSE_INLINE_INTERNAL__
#define __FIREHOSE_INLINE_INTERNAL__

#define firehose_atomic_maxv2o(p, f, v, o, m) \
		os_atomic_rmw_loop2o(p, f, *(o), (v), m, { \
			if (*(o) >= (v)) os_atomic_rmw_loop_give_up(break); \
		})

#define firehose_atomic_max2o(p, f, v, m)   ({ \
		typeof((p)->f) _old; \
		firehose_atomic_maxv2o(p, f, v, &_old, m); \
	})

#ifndef KERNEL
// caller must test for non zero first
OS_ALWAYS_INLINE
static inline uint16_t
firehose_bitmap_first_set(uint64_t bitmap)
{
	dispatch_assert(bitmap != 0);
	// this builtin returns 0 if bitmap is 0, or (first bit set + 1)
	return (uint16_t)__builtin_ffsll((long long)bitmap) - 1;
}
#endif

#pragma mark -
#pragma mark Mach Misc.
#ifndef KERNEL

OS_ALWAYS_INLINE
static inline mach_port_t
firehose_mach_port_allocate(uint32_t flags, void *ctx)
{
	mach_port_t port = MACH_PORT_NULL;
	mach_port_options_t opts = {
		.flags = flags,
	};
	kern_return_t kr = mach_port_construct(mach_task_self(), &opts,
			(mach_port_context_t)ctx, &port);
	if (unlikely(kr)) {
		DISPATCH_VERIFY_MIG(kr);
		DISPATCH_CLIENT_CRASH(kr, "Unable to allocate mach port");
	}
	return port;
}

OS_ALWAYS_INLINE
static inline kern_return_t
firehose_mach_port_recv_dispose(mach_port_t port, void *ctx)
{
	kern_return_t kr;
	kr = mach_port_destruct(mach_task_self(), port, 0,
			(mach_port_context_t)ctx);
	DISPATCH_VERIFY_MIG(kr);
	return kr;
}

OS_ALWAYS_INLINE
static inline void
firehose_mach_port_send_release(mach_port_t port)
{
	kern_return_t kr = mach_port_deallocate(mach_task_self(), port);
	DISPATCH_VERIFY_MIG(kr);
	dispatch_assume_zero(kr);
}

OS_ALWAYS_INLINE
static inline void
firehose_mach_port_guard(mach_port_t port, bool strict, void *ctx)
{
	kern_return_t kr = mach_port_guard(mach_task_self(), port,
			(mach_port_context_t)ctx, strict);
	DISPATCH_VERIFY_MIG(kr);
	dispatch_assume_zero(kr);
}

OS_ALWAYS_INLINE
static inline void
firehose_mig_server(dispatch_mig_callback_t demux, size_t maxmsgsz,
		mach_msg_header_t *hdr)
{
	mig_reply_error_t *msg_reply = (mig_reply_error_t *)alloca(maxmsgsz);
	kern_return_t rc = KERN_SUCCESS;
	bool expects_reply = false;

	if (MACH_MSGH_BITS_REMOTE(hdr->msgh_bits) == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
		expects_reply = true;
	}

	if (!fastpath(demux(hdr, &msg_reply->Head))) {
		rc = MIG_BAD_ID;
	} else if (msg_reply->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) {
		rc = KERN_SUCCESS;
	} else {
		// if MACH_MSGH_BITS_COMPLEX is _not_ set, then msg_reply->RetCode
		// is present
		rc = msg_reply->RetCode;
	}

	if (slowpath(rc == KERN_SUCCESS && expects_reply)) {
		// if crashing here, some handler returned KERN_SUCCESS
		// hoping for firehose_mig_server to perform the mach_msg()
		// call to reply, and it doesn't know how to do that
		DISPATCH_INTERNAL_CRASH(msg_reply->Head.msgh_id,
				"firehose_mig_server doesn't handle replies");
	}
	if (slowpath(rc != KERN_SUCCESS && rc != MIG_NO_REPLY)) {
		// destroy the request - but not the reply port
		hdr->msgh_remote_port = 0;
		mach_msg_destroy(hdr);
	}
}

#endif // !KERNEL
#pragma mark -
#pragma mark firehose buffer

OS_ALWAYS_INLINE
static inline firehose_chunk_t
firehose_buffer_chunk_for_address(void *addr)
{
	uintptr_t chunk_addr = (uintptr_t)addr & ~(FIREHOSE_CHUNK_SIZE - 1);
	return (firehose_chunk_t)chunk_addr;
}

OS_ALWAYS_INLINE
static inline uint16_t
firehose_buffer_chunk_to_ref(firehose_buffer_t fb, firehose_chunk_t fbc)
{
	return (uint16_t)(fbc - fb->fb_chunks);
}

OS_ALWAYS_INLINE
static inline firehose_chunk_t
firehose_buffer_ref_to_chunk(firehose_buffer_t fb, uint16_t ref)
{
	return fb->fb_chunks + ref;
}

#ifndef FIREHOSE_SERVER
#if DISPATCH_PURE_C

OS_ALWAYS_INLINE
static inline uint8_t
firehose_buffer_qos_bits_propagate(void)
{
#ifndef KERNEL
	pthread_priority_t pp = _dispatch_priority_propagate();

	pp &= _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	return (uint8_t)(pp >> _PTHREAD_PRIORITY_QOS_CLASS_SHIFT);
#else
	return 0;
#endif
}

OS_ALWAYS_INLINE
static inline void
firehose_buffer_stream_flush(firehose_buffer_t fb, firehose_stream_t stream)
{
	firehose_buffer_stream_t fbs = &fb->fb_header.fbh_stream[stream];
	firehose_stream_state_u old_state, new_state;
	firehose_chunk_t fc;
	uint64_t stamp = UINT64_MAX; // will cause the reservation to fail
	uint16_t ref;
	long result;

	old_state.fss_atomic_state =
			os_atomic_load2o(fbs, fbs_state.fss_atomic_state, relaxed);
	ref = old_state.fss_current;
	if (!ref || ref == FIREHOSE_STREAM_STATE_PRISTINE) {
		// there is no installed page, nothing to flush, go away
#ifndef KERNEL
		firehose_buffer_force_connect(fb);
#endif
		return;
	}

	fc = firehose_buffer_ref_to_chunk(fb, old_state.fss_current);
	result = firehose_chunk_tracepoint_try_reserve(fc, stamp, stream,
			firehose_buffer_qos_bits_propagate(), 1, 0, NULL);
	if (likely(result < 0)) {
		firehose_buffer_ring_enqueue(fb, old_state.fss_current);
	}
	if (unlikely(result > 0)) {
		// because we pass a silly stamp that requires a flush
		DISPATCH_INTERNAL_CRASH(result, "Allocation should always fail");
	}

	// as a best effort try to uninstall the page we just flushed
	// but failing is okay, let's not contend stupidly for something
	// allocators know how to handle in the first place
	new_state = old_state;
	new_state.fss_current = 0;
	(void)os_atomic_cmpxchg2o(fbs, fbs_state.fss_atomic_state,
			old_state.fss_atomic_state, new_state.fss_atomic_state, relaxed);
}

/**
 * @function firehose_buffer_tracepoint_reserve
 *
 * @abstract
 * Reserves space in the firehose buffer for the tracepoint with specified
 * characteristics.
 *
 * @discussion
 * This returns a slot, with the length of the tracepoint already set, so
 * that in case of a crash, we maximize our chance to be able to skip the
 * tracepoint in case of a partial write.
 *
 * Once the tracepoint has been written, firehose_buffer_tracepoint_flush()
 * must be called.
 *
 * @param fb
 * The buffer to allocate from.
 *
 * @param stream
 * The buffer stream to use.
 *
 * @param pubsize
 * The size of the public data for this tracepoint, cannot be 0, doesn't
 * take the size of the tracepoint header into account.
 *
 * @param privsize
 * The size of the private data for this tracepoint, can be 0.
 *
 * @param privptr
 * The pointer to the private buffer, can be NULL
 *
 * @result
 * The pointer to the tracepoint.
 */
OS_ALWAYS_INLINE
static inline firehose_tracepoint_t
firehose_buffer_tracepoint_reserve(firehose_buffer_t fb, uint64_t stamp,
		firehose_stream_t stream, uint16_t pubsize,
		uint16_t privsize, uint8_t **privptr)
{
	firehose_buffer_stream_t fbs = &fb->fb_header.fbh_stream[stream];
	firehose_stream_state_u old_state, new_state;
	firehose_chunk_t fc;
#if KERNEL
	bool failable = false;
#endif
	bool success;
	long result;
	uint16_t ref;

	// cannot use os_atomic_rmw_loop2o, _page_try_reserve does a store
	old_state.fss_atomic_state =
			os_atomic_load2o(fbs, fbs_state.fss_atomic_state, relaxed);
	for (;;) {
		new_state = old_state;

		ref = old_state.fss_current;
		if (likely(ref && ref != FIREHOSE_STREAM_STATE_PRISTINE)) {
			fc = firehose_buffer_ref_to_chunk(fb, ref);
			result = firehose_chunk_tracepoint_try_reserve(fc, stamp, stream,
					firehose_buffer_qos_bits_propagate(),
					pubsize, privsize, privptr);
			if (likely(result > 0)) {
				uint64_t thread;
#ifdef KERNEL
				thread = thread_tid(current_thread());
#else
				thread = _pthread_threadid_self_np_direct();
#endif
				return firehose_chunk_tracepoint_begin(fc,
						stamp, pubsize, thread, result);
			}
			if (likely(result < 0)) {
				firehose_buffer_ring_enqueue(fb, old_state.fss_current);
			}
			new_state.fss_current = 0;
		}
#if KERNEL
		if (failable) {
			return NULL;
		}
#endif

		if (unlikely(old_state.fss_allocator)) {
			_dispatch_gate_wait(&fbs->fbs_state.fss_gate,
					DLOCK_LOCK_DATA_CONTENTION);
			old_state.fss_atomic_state =
					os_atomic_load2o(fbs, fbs_state.fss_atomic_state, relaxed);
#if KERNEL
			failable = true;
#endif
			continue;
		}

		// if the thread doing the allocation is a low priority one
		// we may starve high priority ones.
		// so disable preemption before we become an allocator
		// the reenabling of the preemption is in
		// firehose_buffer_stream_chunk_install
		__firehose_critical_region_enter();
#if KERNEL
		new_state.fss_allocator = (uint32_t)cpu_number();
#else
		new_state.fss_allocator = _dispatch_lock_value_for_self();
#endif
		success = os_atomic_cmpxchgv2o(fbs, fbs_state.fss_atomic_state,
				old_state.fss_atomic_state, new_state.fss_atomic_state,
				&old_state.fss_atomic_state, relaxed);
		if (likely(success)) {
			break;
		}
		__firehose_critical_region_leave();
	}

	struct firehose_tracepoint_query_s ask = {
		.pubsize = pubsize,
		.privsize = privsize,
		.stream = stream,
		.for_io = (firehose_stream_uses_io_bank & (1UL << stream)) != 0,
#ifndef KERNEL
		.quarantined = fb->fb_header.fbh_quarantined,
#endif
		.stamp = stamp,
	};
	return firehose_buffer_tracepoint_reserve_slow(fb, &ask, privptr);
}

/**
 * @function firehose_buffer_tracepoint_flush
 *
 * @abstract
 * Flushes a firehose tracepoint, and sends the chunk to the daemon when full
 * and this was the last tracepoint writer for this chunk.
 *
 * @param fb
 * The buffer the tracepoint belongs to.
 *
 * @param ft
 * The tracepoint to flush.
 *
 * @param ftid
 * The firehose tracepoint ID for that tracepoint.
 * It is written last, preventing compiler reordering, so that its absence
 * on crash recovery means the tracepoint is partial.
 */
OS_ALWAYS_INLINE
static inline void
firehose_buffer_tracepoint_flush(firehose_buffer_t fb,
		firehose_tracepoint_t ft, firehose_tracepoint_id_u ftid)
{
	firehose_chunk_t fc = firehose_buffer_chunk_for_address(ft);

	// Needed for process death handling (tracepoint-flush):
	// We want to make sure the observers
	// will see memory effects in program (asm) order.
	// 1. write all the data to the tracepoint
	// 2. write the tracepoint ID, so that seeing it means the tracepoint
	//    is valid
	if (firehose_chunk_tracepoint_end(fc, ft, ftid)) {
		firehose_buffer_ring_enqueue(fb, firehose_buffer_chunk_to_ref(fb, fc));
	}
}

#ifndef KERNEL
OS_ALWAYS_INLINE
static inline void
firehose_buffer_clear_bank_flags(firehose_buffer_t fb, unsigned long bits)
{
	firehose_buffer_bank_t fbb = &fb->fb_header.fbh_bank;
	unsigned long orig_flags;

	orig_flags = os_atomic_and_orig2o(fbb, fbb_flags, ~bits, relaxed);
	if (orig_flags != (orig_flags & ~bits)) {
		firehose_buffer_update_limits(fb);
	}
}

OS_ALWAYS_INLINE
static inline void
firehose_buffer_set_bank_flags(firehose_buffer_t fb, unsigned long bits)
{
	firehose_buffer_bank_t fbb = &fb->fb_header.fbh_bank;
	unsigned long orig_flags;

	orig_flags = os_atomic_or_orig2o(fbb, fbb_flags, bits, relaxed);
	if (orig_flags != (orig_flags | bits)) {
		firehose_buffer_update_limits(fb);
	}
}
#endif // !KERNEL

#endif // !defined(FIREHOSE_SERVER)

#endif // DISPATCH_PURE_C

#endif // __FIREHOSE_INLINE_INTERNAL__
