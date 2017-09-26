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

#include <mach/vm_statistics.h> // VM_MEMORY_GENEALOGY
#ifdef KERNEL

#define OS_VOUCHER_ACTIVITY_SPI_TYPES 1
#define OS_FIREHOSE_SPI 1
#define __OS_EXPOSE_INTERNALS_INDIRECT__ 1

#define DISPATCH_PURE_C 1
#define _safe_cast_to_long(x) \
		({ _Static_assert(sizeof(typeof(x)) <= sizeof(long), \
				"__builtin_expect doesn't support types wider than long"); \
				(long)(x); })
#define fastpath(x) ((typeof(x))__builtin_expect(_safe_cast_to_long(x), ~0l))
#define slowpath(x) ((typeof(x))__builtin_expect(_safe_cast_to_long(x), 0l))
#define os_likely(x) __builtin_expect(!!(x), 1)
#define os_unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#ifndef OS_FALLTHROUGH
#define OS_FALLTHROUGH
#endif

#define DISPATCH_INTERNAL_CRASH(ac, msg) ({ panic(msg); __builtin_trap(); })

#if defined(__x86_64__) || defined(__i386__)
#define dispatch_hardware_pause() __asm__("pause")
#elif (defined(__arm__) && defined(_ARM_ARCH_7) && defined(__thumb__)) || \
		defined(__arm64__)
#define dispatch_hardware_pause() __asm__("yield")
#define dispatch_hardware_wfe()   __asm__("wfe")
#else
#define dispatch_hardware_pause() __asm__("")
#endif

#define _dispatch_wait_until(c) ({ \
		typeof(c) _c; \
		for (;;) { \
			if (likely(_c = (c))) break; \
			dispatch_hardware_pause(); \
		} \
		_c; })
#define dispatch_compiler_barrier()  __asm__ __volatile__("" ::: "memory")

typedef uint32_t dispatch_lock;
typedef struct dispatch_gate_s {
	dispatch_lock dgl_lock;
} dispatch_gate_s, *dispatch_gate_t;
#define DLOCK_LOCK_DATA_CONTENTION 0
static void _dispatch_gate_wait(dispatch_gate_t l, uint32_t flags);

#define fcp_quarntined fcp_quarantined

#include <kern/debug.h>
#include <machine/cpu_number.h>
#include <kern/thread.h>
#include <mach/port.h>
#include <stdbool.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <vm/vm_kern.h>
#include <internal/atomic.h> // os/internal/atomic.h
#include <firehose_types_private.h> // <firehose/firehose_types_private.h>
#include <tracepoint_private.h> // <firehose/tracepoint_private.h>
#include <chunk_private.h> // <firehose/chunk_private.h>
#include "os/firehose_buffer_private.h"
#include "firehose_buffer_internal.h"
#include "firehose_inline_internal.h"
#else
#include "internal.h"
#include "firehose.h" // MiG
#include "firehose_replyServer.h" // MiG
#endif

#if OS_FIREHOSE_SPI

#if __has_feature(c_static_assert)
_Static_assert(sizeof(((firehose_stream_state_u *)NULL)->fss_gate) ==
		sizeof(((firehose_stream_state_u *)NULL)->fss_allocator),
		"fss_gate and fss_allocator alias");
_Static_assert(offsetof(firehose_stream_state_u, fss_gate) ==
		offsetof(firehose_stream_state_u, fss_allocator),
		"fss_gate and fss_allocator alias");
_Static_assert(sizeof(struct firehose_buffer_header_s) ==
				FIREHOSE_CHUNK_SIZE,
		"firehose buffer header must be 4k");
_Static_assert(offsetof(struct firehose_buffer_header_s, fbh_unused) <=
				FIREHOSE_CHUNK_SIZE - FIREHOSE_BUFFER_LIBTRACE_HEADER_SIZE,
		"we must have enough space for the libtrace header");
_Static_assert(powerof2(FIREHOSE_BUFFER_CHUNK_COUNT),
		"CHUNK_COUNT Must be a power of two");
_Static_assert(FIREHOSE_BUFFER_CHUNK_COUNT <= 64,
		"CHUNK_COUNT must be less than 64 (bitmap in uint64_t)");
#ifdef FIREHOSE_BUFFER_MADVISE_CHUNK_COUNT
_Static_assert(powerof2(FIREHOSE_BUFFER_MADVISE_CHUNK_COUNT),
		"madvise chunk count must be a power of two");
#endif
_Static_assert(sizeof(struct firehose_buffer_stream_s) == 128,
		"firehose buffer stream must be small (single cacheline if possible)");
_Static_assert(sizeof(struct firehose_tracepoint_s) == 24,
		"tracepoint header should be exactly 24 bytes");
#endif

#ifdef KERNEL
static firehose_buffer_t kernel_firehose_buffer = NULL;
#endif

#pragma mark -
#pragma mark Client IPC to the log daemon
#ifndef KERNEL

static mach_port_t
firehose_client_reconnect(firehose_buffer_t fb, mach_port_t oldsendp)
{
	mach_port_t sendp = MACH_PORT_NULL;
	mach_port_t mem_port = MACH_PORT_NULL, extra_info_port = MACH_PORT_NULL;
	mach_vm_size_t extra_info_size = 0;
	kern_return_t kr;

	dispatch_assert(fb->fb_header.fbh_logd_port);
	dispatch_assert(fb->fb_header.fbh_recvp);
	dispatch_assert(fb->fb_header.fbh_uniquepid != 0);

	_dispatch_unfair_lock_lock(&fb->fb_header.fbh_logd_lock);
	sendp = fb->fb_header.fbh_sendp;
	if (sendp != oldsendp || sendp == MACH_PORT_DEAD) {
		// someone beat us to reconnecting or logd was unloaded, just go away
		goto unlock;
	}

	if (oldsendp) {
		// same trick as _xpc_pipe_dispose: keeping a send right
		// maintains the name, so that we can destroy the receive right
		// in case we still have it.
		(void)firehose_mach_port_recv_dispose(oldsendp, fb);
		firehose_mach_port_send_release(oldsendp);
		fb->fb_header.fbh_sendp = MACH_PORT_NULL;
	}

	/* Create a memory port for the buffer VM region */
	vm_prot_t flags = VM_PROT_READ | MAP_MEM_VM_SHARE;
	memory_object_size_t size = sizeof(union firehose_buffer_u);
	mach_vm_address_t addr = (vm_address_t)fb;

	kr = mach_make_memory_entry_64(mach_task_self(), &size, addr,
			flags, &mem_port, MACH_PORT_NULL);
	if (size < sizeof(union firehose_buffer_u)) {
		DISPATCH_CLIENT_CRASH(size, "Invalid size for the firehose buffer");
	}
	if (unlikely(kr)) {
		// the client probably has some form of memory corruption
		// and/or a port leak
		DISPATCH_CLIENT_CRASH(kr, "Unable to make memory port");
	}

	/* Create a communication port to the logging daemon */
	uint32_t opts = MPO_CONTEXT_AS_GUARD | MPO_TEMPOWNER | MPO_INSERT_SEND_RIGHT;
	sendp = firehose_mach_port_allocate(opts, fb);

	if (oldsendp && _voucher_libtrace_hooks->vah_get_reconnect_info) {
		kr = _voucher_libtrace_hooks->vah_get_reconnect_info(&addr, &size);
		if (likely(kr == KERN_SUCCESS) && addr && size) {
			extra_info_size = size;
			kr = mach_make_memory_entry_64(mach_task_self(), &size, addr,
					flags, &extra_info_port, MACH_PORT_NULL);
			if (unlikely(kr)) {
				// the client probably has some form of memory corruption
				// and/or a port leak
				DISPATCH_CLIENT_CRASH(kr, "Unable to make memory port");
			}
			kr = mach_vm_deallocate(mach_task_self(), addr, size);
			(void)dispatch_assume_zero(kr);
		}
	}

	/* Call the firehose_register() MIG routine */
	kr = firehose_send_register(fb->fb_header.fbh_logd_port, mem_port,
			sizeof(union firehose_buffer_u), sendp, fb->fb_header.fbh_recvp,
			extra_info_port, extra_info_size);
	if (likely(kr == KERN_SUCCESS)) {
		fb->fb_header.fbh_sendp = sendp;
	} else if (unlikely(kr == MACH_SEND_INVALID_DEST)) {
		// MACH_SEND_INVALID_DEST here means that logd's boostrap port
		// turned into a dead name, which in turn means that logd has been
		// unloaded. The only option here, is to give up permanently.
		//
		// same trick as _xpc_pipe_dispose: keeping a send right
		// maintains the name, so that we can destroy the receive right
		// in case we still have it.
		(void)firehose_mach_port_recv_dispose(sendp, fb);
		firehose_mach_port_send_release(sendp);
		firehose_mach_port_send_release(mem_port);
		if (extra_info_port) firehose_mach_port_send_release(extra_info_port);
		sendp = fb->fb_header.fbh_sendp = MACH_PORT_DEAD;
	} else {
		// the client probably has some form of memory corruption
		// and/or a port leak
		DISPATCH_CLIENT_CRASH(kr, "Unable to register with logd");
	}

unlock:
	_dispatch_unfair_lock_unlock(&fb->fb_header.fbh_logd_lock);
	return sendp;
}

static void
firehose_buffer_update_limits_unlocked(firehose_buffer_t fb)
{
	firehose_bank_state_u old, new;
	firehose_buffer_bank_t fbb = &fb->fb_header.fbh_bank;
	unsigned long fbb_flags = fbb->fbb_flags;
	uint16_t io_streams = 0, mem_streams = 0;
	uint16_t total = 0;

	for (size_t i = 0; i < countof(fb->fb_header.fbh_stream); i++) {
		firehose_buffer_stream_t fbs = fb->fb_header.fbh_stream + i;

		if (fbs->fbs_state.fss_current == FIREHOSE_STREAM_STATE_PRISTINE) {
			continue;
		}
		if ((1UL << i) & firehose_stream_uses_io_bank) {
			io_streams++;
		} else {
			mem_streams++;
		}
	}

	if (fbb_flags & FIREHOSE_BUFFER_BANK_FLAG_LOW_MEMORY) {
		if (fbb_flags & FIREHOSE_BUFFER_BANK_FLAG_HIGH_RATE) {
			total = 1 + 4 * mem_streams + io_streams;		// usually 10
		} else {
			total = 1 + 2 + mem_streams + io_streams;		// usually 6
		}
	} else {
		if (fbb_flags & FIREHOSE_BUFFER_BANK_FLAG_HIGH_RATE) {
			total = 1 + 6 * mem_streams + 3 * io_streams;	// usually 16
		} else {
			total = 1 + 2 * (mem_streams + io_streams);		// usually 7
		}
	}

	uint16_t ratio = (uint16_t)(PAGE_SIZE / FIREHOSE_CHUNK_SIZE);
	if (ratio > 1) {
		total = roundup(total, ratio);
	}
	total = MAX(total, FIREHOSE_BUFFER_CHUNK_PREALLOCATED_COUNT);
	if (!(fbb_flags & FIREHOSE_BUFFER_BANK_FLAG_LOW_MEMORY)) {
		total = MAX(total, TARGET_OS_EMBEDDED ? 8 : 12);
	}

	new.fbs_max_ref  = total;
	new.fbs_mem_bank = FIREHOSE_BANK_UNAVAIL_BIT - (total - 1);
	new.fbs_io_bank  = FIREHOSE_BANK_UNAVAIL_BIT -
			MAX(3 * total / 8, 2 * io_streams);
	new.fbs_unused   = 0;

	old = fbb->fbb_limits;
	fbb->fbb_limits = new;
	if (old.fbs_atomic_state == new.fbs_atomic_state) {
		return;
	}
	os_atomic_add2o(&fb->fb_header, fbh_bank.fbb_state.fbs_atomic_state,
			new.fbs_atomic_state - old.fbs_atomic_state, relaxed);
}
#endif // !KERNEL

firehose_buffer_t
firehose_buffer_create(mach_port_t logd_port, uint64_t unique_pid,
		unsigned long bank_flags)
{
	firehose_buffer_header_t fbh;
	firehose_buffer_t fb;

#ifndef KERNEL
	mach_vm_address_t vm_addr = 0;
	kern_return_t kr;

	vm_addr = vm_page_size;
	const size_t madvise_bytes = FIREHOSE_BUFFER_MADVISE_CHUNK_COUNT *
			FIREHOSE_CHUNK_SIZE;
	if (slowpath(madvise_bytes % PAGE_SIZE)) {
		DISPATCH_INTERNAL_CRASH(madvise_bytes,
				"Invalid values for MADVISE_CHUNK_COUNT / CHUNK_SIZE");
	}

	kr = mach_vm_map(mach_task_self(), &vm_addr, sizeof(*fb), 0,
			VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE |
			VM_MAKE_TAG(VM_MEMORY_GENEALOGY), MEMORY_OBJECT_NULL, 0, FALSE,
			VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_NONE);
	if (slowpath(kr)) {
		if (kr != KERN_NO_SPACE) dispatch_assume_zero(kr);
		firehose_mach_port_send_release(logd_port);
		return NULL;
	}

	uint32_t opts = MPO_CONTEXT_AS_GUARD | MPO_STRICT | MPO_INSERT_SEND_RIGHT;
#else
	vm_offset_t vm_addr = 0;
	vm_size_t size;

	size = FIREHOSE_BUFFER_KERNEL_CHUNK_COUNT * FIREHOSE_CHUNK_SIZE;
	__firehose_allocate(&vm_addr, size);

	(void)logd_port; (void)unique_pid;
#endif // KERNEL

	fb = (firehose_buffer_t)vm_addr;
	fbh = &fb->fb_header;
#ifndef KERNEL
	fbh->fbh_logd_port = logd_port;
	fbh->fbh_pid = getpid();
	fbh->fbh_uniquepid = unique_pid;
	fbh->fbh_recvp = firehose_mach_port_allocate(opts, fb);
#endif // !KERNEL
	fbh->fbh_spi_version = OS_FIREHOSE_SPI_VERSION;
	fbh->fbh_bank.fbb_flags = bank_flags;

#ifndef KERNEL
	for (size_t i = 0; i < countof(fbh->fbh_stream); i++) {
		firehose_buffer_stream_t fbs = fbh->fbh_stream + i;
		if (i != firehose_stream_metadata) {
			fbs->fbs_state.fss_current = FIREHOSE_STREAM_STATE_PRISTINE;
		}
	}
	firehose_buffer_update_limits_unlocked(fb);
#else
	uint16_t total = FIREHOSE_BUFFER_CHUNK_PREALLOCATED_COUNT + 1;
	const uint16_t num_kernel_io_pages = 8;
	uint16_t io_pages = num_kernel_io_pages;
	fbh->fbh_bank.fbb_state = (firehose_bank_state_u){
		.fbs_max_ref = total,
		.fbs_io_bank = FIREHOSE_BANK_UNAVAIL_BIT - io_pages,
		.fbs_mem_bank = FIREHOSE_BANK_UNAVAIL_BIT - (total - io_pages - 1),
	};
	fbh->fbh_bank.fbb_limits = fbh->fbh_bank.fbb_state;
#endif // KERNEL

	// now pre-allocate some chunks in the ring directly
#ifdef KERNEL
	const uint16_t pre_allocated = FIREHOSE_BUFFER_CHUNK_PREALLOCATED_COUNT - 1;
#else
	const uint16_t pre_allocated = FIREHOSE_BUFFER_CHUNK_PREALLOCATED_COUNT;
#endif

	fbh->fbh_bank.fbb_bitmap = (1U << (1 + pre_allocated)) - 1;

	for (uint16_t i = 0; i < pre_allocated; i++) {
		fbh->fbh_mem_ring[i] = i + 1;
	}
	fbh->fbh_bank.fbb_mem_flushed = pre_allocated;
	fbh->fbh_ring_mem_head = pre_allocated;


#ifdef KERNEL
	// install the early boot page as the current one for persist
	fbh->fbh_stream[firehose_stream_persist].fbs_state.fss_current =
			FIREHOSE_BUFFER_CHUNK_PREALLOCATED_COUNT;
	fbh->fbh_bank.fbb_state.fbs_io_bank += 1;
#endif

	fbh->fbh_ring_tail = (firehose_ring_tail_u){
		.frp_mem_flushed = pre_allocated,
	};
	return fb;
}

#ifndef KERNEL
static void
firehose_notify_source_invoke(mach_msg_header_t *hdr)
{
	const size_t reply_size =
			sizeof(union __ReplyUnion__firehose_client_firehoseReply_subsystem);

	firehose_mig_server(firehoseReply_server, reply_size, hdr);
}

static void
firehose_client_register_for_notifications(firehose_buffer_t fb)
{
	static const struct dispatch_continuation_s dc = {
		.dc_func = (void *)firehose_notify_source_invoke,
	};
	firehose_buffer_header_t fbh = &fb->fb_header;

	dispatch_once(&fbh->fbh_notifs_pred, ^{
		dispatch_source_t ds = _dispatch_source_create_mach_msg_direct_recv(
				fbh->fbh_recvp, &dc);
		dispatch_set_context(ds, fb);
		dispatch_activate(ds);
		fbh->fbh_notifs_source = ds;
	});
}

static void
firehose_client_send_push_async(firehose_buffer_t fb, qos_class_t qos,
		bool for_io)
{
	bool ask_for_notifs = fb->fb_header.fbh_notifs_source != NULL;
	mach_port_t sendp = fb->fb_header.fbh_sendp;
	kern_return_t kr = KERN_FAILURE;

	if (!ask_for_notifs && _dispatch_is_multithreaded_inline()) {
		firehose_client_register_for_notifications(fb);
		ask_for_notifs = true;
	}

	if (slowpath(sendp == MACH_PORT_DEAD)) {
		return;
	}

	if (fastpath(sendp)) {
		kr = firehose_send_push_async(sendp, qos, for_io, ask_for_notifs);
		if (likely(kr == KERN_SUCCESS)) {
			return;
		}
		if (kr != MACH_SEND_INVALID_DEST) {
			DISPATCH_VERIFY_MIG(kr);
			dispatch_assume_zero(kr);
		}
	}

	sendp = firehose_client_reconnect(fb, sendp);
	if (fastpath(MACH_PORT_VALID(sendp))) {
		kr = firehose_send_push_async(sendp, qos, for_io, ask_for_notifs);
		if (likely(kr == KERN_SUCCESS)) {
			return;
		}
		if (kr != MACH_SEND_INVALID_DEST) {
			DISPATCH_VERIFY_MIG(kr);
			dispatch_assume_zero(kr);
		}
	}
}

OS_NOINLINE
static void
firehose_client_start_quarantine(firehose_buffer_t fb)
{
	if (_voucher_libtrace_hooks->vah_version < 5) return;
	if (!_voucher_libtrace_hooks->vah_quarantine_starts) return;

	_voucher_libtrace_hooks->vah_quarantine_starts();

	fb->fb_header.fbh_quarantined = true;
	firehose_buffer_stream_flush(fb, firehose_stream_special);
	firehose_buffer_stream_flush(fb, firehose_stream_persist);
	firehose_buffer_stream_flush(fb, firehose_stream_memory);
}
#endif // !KERNEL

static void
firehose_client_merge_updates(firehose_buffer_t fb, bool async_notif,
		firehose_push_reply_t reply, bool quarantined,
		firehose_bank_state_u *state_out)
{
	firehose_buffer_header_t fbh = &fb->fb_header;
	firehose_bank_state_u state;
	firehose_ring_tail_u otail, ntail;
	uint64_t old_flushed_pos, bank_updates;
	uint16_t io_delta = 0;
	uint16_t mem_delta = 0;

	if (quarantined) {
#ifndef KERNEL
		// this isn't a dispatch_once so that the upcall to libtrace
		// can actually log itself without blocking on the gate.
		if (async_notif) {
			if (os_atomic_xchg(&fbh->fbh_quarantined_state,
					FBH_QUARANTINE_STARTED, relaxed) !=
					FBH_QUARANTINE_STARTED) {
				firehose_client_start_quarantine(fb);
			}
		} else if (os_atomic_load(&fbh->fbh_quarantined_state, relaxed) ==
				FBH_QUARANTINE_NONE) {
			os_atomic_cmpxchg(&fbh->fbh_quarantined_state, FBH_QUARANTINE_NONE,
					FBH_QUARANTINE_PENDING, relaxed);
		}
#endif
	}

	if (firehose_atomic_maxv2o(fbh, fbh_bank.fbb_mem_flushed,
			reply.fpr_mem_flushed_pos, &old_flushed_pos, relaxed)) {
		mem_delta = (uint16_t)(reply.fpr_mem_flushed_pos - old_flushed_pos);
	}
	if (firehose_atomic_maxv2o(fbh, fbh_bank.fbb_io_flushed,
			reply.fpr_io_flushed_pos, &old_flushed_pos, relaxed)) {
		io_delta = (uint16_t)(reply.fpr_io_flushed_pos - old_flushed_pos);
	}
#ifndef KERNEL
	_dispatch_debug("client side: mem: +%d->%llx, io: +%d->%llx",
			mem_delta, reply.fpr_mem_flushed_pos,
			io_delta, reply.fpr_io_flushed_pos);
#endif

	if (!mem_delta && !io_delta) {
		if (state_out) {
			state_out->fbs_atomic_state = os_atomic_load2o(fbh,
					fbh_bank.fbb_state.fbs_atomic_state, relaxed);
		}
		return;
	}

	__firehose_critical_region_enter();
	os_atomic_rmw_loop2o(fbh, fbh_ring_tail.frp_atomic_tail,
			otail.frp_atomic_tail, ntail.frp_atomic_tail, relaxed, {
		ntail = otail;
		// overflow handles the generation wraps
		ntail.frp_io_flushed += io_delta;
		ntail.frp_mem_flushed += mem_delta;
	});

	bank_updates = ((uint64_t)mem_delta << FIREHOSE_BANK_SHIFT(0)) |
			((uint64_t)io_delta << FIREHOSE_BANK_SHIFT(1));
	state.fbs_atomic_state = os_atomic_sub2o(fbh,
			fbh_bank.fbb_state.fbs_atomic_state, bank_updates, release);
	__firehose_critical_region_leave();

	if (state_out) *state_out = state;

	if (async_notif) {
		if (io_delta) {
			os_atomic_inc2o(fbh, fbh_bank.fbb_io_notifs, relaxed);
		}
		if (mem_delta) {
			os_atomic_inc2o(fbh, fbh_bank.fbb_mem_notifs, relaxed);
		}
	}
}

#ifndef KERNEL
OS_NOT_TAIL_CALLED OS_NOINLINE
static void
firehose_client_send_push_and_wait(firehose_buffer_t fb, bool for_io,
		firehose_bank_state_u *state_out)
{
	mach_port_t sendp = fb->fb_header.fbh_sendp;
	firehose_push_reply_t push_reply = { };
	qos_class_t qos = qos_class_self();
	boolean_t quarantined = false;
	kern_return_t kr;

	if (slowpath(sendp == MACH_PORT_DEAD)) {
		return;
	}
	if (fastpath(sendp)) {
		kr = firehose_send_push_and_wait(sendp, qos, for_io,
				&push_reply, &quarantined);
		if (likely(kr == KERN_SUCCESS)) {
			goto success;
		}
		if (kr != MACH_SEND_INVALID_DEST) {
			DISPATCH_VERIFY_MIG(kr);
			dispatch_assume_zero(kr);
		}
	}

	sendp = firehose_client_reconnect(fb, sendp);
	if (fastpath(MACH_PORT_VALID(sendp))) {
		kr = firehose_send_push_and_wait(sendp, qos, for_io,
				&push_reply, &quarantined);
		if (likely(kr == KERN_SUCCESS)) {
			goto success;
		}
		if (kr != MACH_SEND_INVALID_DEST) {
			DISPATCH_VERIFY_MIG(kr);
			dispatch_assume_zero(kr);
		}
	}

	if (state_out) {
		state_out->fbs_atomic_state = os_atomic_load2o(&fb->fb_header,
				fbh_bank.fbb_state.fbs_atomic_state, relaxed);
	}
	return;

success:
	if (memcmp(&push_reply, &FIREHOSE_PUSH_REPLY_CORRUPTED,
			sizeof(push_reply)) == 0) {
		// TODO: find out the actual cause and log it
		DISPATCH_CLIENT_CRASH(0, "Memory corruption in the logging buffers");
	}

	if (for_io) {
		os_atomic_inc2o(&fb->fb_header, fbh_bank.fbb_io_sync_pushes, relaxed);
	} else {
		os_atomic_inc2o(&fb->fb_header, fbh_bank.fbb_mem_sync_pushes, relaxed);
	}
	// TODO <rdar://problem/22963876>
	//
	// use fbb_*_flushes and fbb_*_sync_pushes to decide to dynamically
	// allow using more buffers, if not under memory pressure.
	//
	// There only is a point for multithreaded clients if:
	// - enough samples (total_flushes above some limits)
	// - the ratio is really bad (a push per cycle is definitely a problem)
	return firehose_client_merge_updates(fb, false, push_reply, quarantined,
			state_out);
}

OS_NOT_TAIL_CALLED OS_NOINLINE
static void
__FIREHOSE_CLIENT_THROTTLED_DUE_TO_HEAVY_LOGGING__(firehose_buffer_t fb,
		bool for_io, firehose_bank_state_u *state_out)
{
	firehose_client_send_push_and_wait(fb, for_io, state_out);
}

kern_return_t
firehose_client_push_reply(mach_port_t req_port OS_UNUSED,
	kern_return_t rtc, firehose_push_reply_t push_reply OS_UNUSED,
	boolean_t quarantined OS_UNUSED)
{
	DISPATCH_INTERNAL_CRASH(rtc, "firehose_push_reply should never be sent "
			"to the buffer receive port");
}

kern_return_t
firehose_client_push_notify_async(mach_port_t server_port OS_UNUSED,
	firehose_push_reply_t push_reply, boolean_t quarantined)
{
	// see _dispatch_source_merge_mach_msg_direct
	dispatch_queue_t dq = _dispatch_queue_get_current();
	firehose_buffer_t fb = dispatch_get_context(dq);
	firehose_client_merge_updates(fb, true, push_reply, quarantined, NULL);
	return KERN_SUCCESS;
}

#endif // !KERNEL
#pragma mark -
#pragma mark Buffer handling

#ifndef KERNEL
void
firehose_buffer_update_limits(firehose_buffer_t fb)
{
	dispatch_unfair_lock_t fbb_lock = &fb->fb_header.fbh_bank.fbb_lock;
	_dispatch_unfair_lock_lock(fbb_lock);
	firehose_buffer_update_limits_unlocked(fb);
	_dispatch_unfair_lock_unlock(fbb_lock);
}
#endif // !KERNEL

OS_ALWAYS_INLINE
static inline firehose_tracepoint_t
firehose_buffer_chunk_init(firehose_chunk_t fc,
		firehose_tracepoint_query_t ask, uint8_t **privptr)
{
	const uint16_t ft_size = offsetof(struct firehose_tracepoint_s, ft_data);

	uint16_t pub_offs = offsetof(struct firehose_chunk_s, fc_data);
	uint16_t priv_offs = FIREHOSE_CHUNK_SIZE;

	pub_offs += roundup(ft_size + ask->pubsize, 8);
	priv_offs -= ask->privsize;

	if (fc->fc_pos.fcp_atomic_pos) {
		// Needed for process death handling (recycle-reuse):
		// No atomic fences required, we merely want to make sure the observers
		// will see memory effects in program (asm) order.
		// 1. the payload part of the chunk is cleared completely
		// 2. the chunk is marked as reused
		// This ensures that if we don't see a reference to a chunk in the ring
		// and it is dirty, when crawling the chunk, we don't see remnants of
		// other tracepoints
		//
		// We only do that when the fc_pos is non zero, because zero means
		// we just faulted the chunk, and the kernel already bzero-ed it.
		bzero(fc->fc_data, sizeof(fc->fc_data));
	}
	dispatch_compiler_barrier();
	// <rdar://problem/23562733> boot starts mach absolute time at 0, and
	// wrapping around to values above UINT64_MAX - FIREHOSE_STAMP_SLOP
	// breaks firehose_buffer_stream_flush() assumptions
	if (ask->stamp > FIREHOSE_STAMP_SLOP) {
		fc->fc_timestamp = ask->stamp - FIREHOSE_STAMP_SLOP;
	} else {
		fc->fc_timestamp = 0;
	}
	fc->fc_pos = (firehose_chunk_pos_u){
		.fcp_next_entry_offs = pub_offs,
		.fcp_private_offs = priv_offs,
		.fcp_refcnt = 1,
		.fcp_qos = firehose_buffer_qos_bits_propagate(),
		.fcp_stream = ask->stream,
		.fcp_flag_io = ask->for_io,
		.fcp_quarantined = ask->quarantined,
	};

	if (privptr) {
		*privptr = fc->fc_start + priv_offs;
	}
	return (firehose_tracepoint_t)fc->fc_data;
}

OS_NOINLINE
static firehose_tracepoint_t
firehose_buffer_stream_chunk_install(firehose_buffer_t fb,
		firehose_tracepoint_query_t ask, uint8_t **privptr, uint16_t ref)
{
	firehose_stream_state_u state, new_state;
	firehose_tracepoint_t ft;
	firehose_buffer_header_t fbh = &fb->fb_header;
	firehose_buffer_stream_t fbs = &fbh->fbh_stream[ask->stream];
	uint64_t stamp_and_len;

	if (fastpath(ref)) {
		firehose_chunk_t fc = firehose_buffer_ref_to_chunk(fb, ref);
		ft = firehose_buffer_chunk_init(fc, ask, privptr);
		// Needed for process death handling (tracepoint-begin):
		// write the length before making the chunk visible
		stamp_and_len  = ask->stamp - fc->fc_timestamp;
		stamp_and_len |= (uint64_t)ask->pubsize << 48;
		os_atomic_store2o(ft, ft_stamp_and_length, stamp_and_len, relaxed);
#ifdef KERNEL
		ft->ft_thread = thread_tid(current_thread());
#else
		ft->ft_thread = _pthread_threadid_self_np_direct();
#endif
		if (ask->stream == firehose_stream_metadata) {
			os_atomic_or2o(fbh, fbh_bank.fbb_metadata_bitmap,
					1ULL << ref, relaxed);
		}
		// release barrier to make the chunk init visible
		os_atomic_rmw_loop2o(fbs, fbs_state.fss_atomic_state,
				state.fss_atomic_state, new_state.fss_atomic_state, release, {
			// We use a generation counter to prevent a theoretical ABA problem:
			// a thread could try to acquire a tracepoint in a chunk, fail to
			// do so mark it as to be pushed, enqueue it, and then be preempted
			//
			// It sleeps for a long time, and then tries to acquire the
			// allocator bit and uninstalling the chunk. Succeeds in doing so,
			// but because the chunk actually happened to have cycled all the
			// way back to being installed. That thread would effectively hide
			// that unflushed chunk and leak it.
			//
			// Having a generation counter prevents the uninstallation of the
			// chunk to spuriously succeed when it was a re-incarnation of it.
			new_state = (firehose_stream_state_u){
				.fss_current = ref,
				.fss_generation = state.fss_generation + 1,
			};
		});
	} else {
		// the allocator gave up just clear the allocator + waiter bits
		firehose_stream_state_u mask = { .fss_allocator = ~0u, };
		state.fss_atomic_state = os_atomic_and_orig2o(fbs,
				fbs_state.fss_atomic_state, ~mask.fss_atomic_state, relaxed);
		ft = NULL;
	}

	// pairs with the one in firehose_buffer_tracepoint_reserve()
	__firehose_critical_region_leave();

#ifndef KERNEL
	if (unlikely(_dispatch_lock_is_locked_by_self(state.fss_gate.dgl_lock))) {
		_dispatch_gate_broadcast_slow(&fbs->fbs_state.fss_gate,
				state.fss_gate.dgl_lock);
	}

	if (unlikely(state.fss_current == FIREHOSE_STREAM_STATE_PRISTINE)) {
		firehose_buffer_update_limits(fb);
	}

	if (unlikely(os_atomic_load2o(fbh, fbh_quarantined_state, relaxed) ==
			FBH_QUARANTINE_PENDING)) {
		if (os_atomic_cmpxchg2o(fbh, fbh_quarantined_state,
				FBH_QUARANTINE_PENDING, FBH_QUARANTINE_STARTED, relaxed)) {
			firehose_client_start_quarantine(fb);
		}
	}
#endif // KERNEL

	return ft;
}

#ifndef KERNEL
OS_ALWAYS_INLINE
static inline uint16_t
firehose_buffer_ring_try_grow(firehose_buffer_bank_t fbb, uint16_t limit)
{
	uint16_t ref = 0;
	uint64_t bitmap;

	_dispatch_unfair_lock_lock(&fbb->fbb_lock);
	bitmap = ~(fbb->fbb_bitmap | (~0ULL << limit));
	if (bitmap) {
		ref = firehose_bitmap_first_set(bitmap);
		fbb->fbb_bitmap |= 1U << ref;
	}
	_dispatch_unfair_lock_unlock(&fbb->fbb_lock);
	return ref;
}

OS_ALWAYS_INLINE
static inline uint16_t
firehose_buffer_ring_shrink(firehose_buffer_t fb, uint16_t ref)
{
	const size_t madv_size =
			FIREHOSE_CHUNK_SIZE * FIREHOSE_BUFFER_MADVISE_CHUNK_COUNT;
	const size_t madv_mask =
			(1ULL << FIREHOSE_BUFFER_MADVISE_CHUNK_COUNT) - 1;

	dispatch_unfair_lock_t fbb_lock = &fb->fb_header.fbh_bank.fbb_lock;
	uint64_t bitmap;

	_dispatch_unfair_lock_lock(fbb_lock);
	if (ref < fb->fb_header.fbh_bank.fbb_limits.fbs_max_ref) {
		goto done;
	}

	bitmap = (fb->fb_header.fbh_bank.fbb_bitmap &= ~(1UL << ref));
	ref &= ~madv_mask;
	if ((bitmap & (madv_mask << ref)) == 0) {
		// if MADVISE_WIDTH consecutive chunks are free, madvise them free
		madvise(firehose_buffer_ref_to_chunk(fb, ref), madv_size, MADV_FREE);
	}
	ref = 0;
done:
	_dispatch_unfair_lock_unlock(fbb_lock);
	return ref;
}
#endif // !KERNEL

OS_NOINLINE
void
firehose_buffer_ring_enqueue(firehose_buffer_t fb, uint16_t ref)
{
	firehose_chunk_t fc = firehose_buffer_ref_to_chunk(fb, ref);
	uint16_t volatile *fbh_ring;
	uint16_t volatile *fbh_ring_head;
	uint16_t head, gen, dummy, idx;
	firehose_chunk_pos_u fc_pos = fc->fc_pos;
	bool for_io = fc_pos.fcp_flag_io;

	if (for_io) {
		fbh_ring = fb->fb_header.fbh_io_ring;
		fbh_ring_head = &fb->fb_header.fbh_ring_io_head;
	} else {
		fbh_ring = fb->fb_header.fbh_mem_ring;
		fbh_ring_head = &fb->fb_header.fbh_ring_mem_head;
	}

#ifdef KERNEL
	// The algorithm in the kernel is simpler:
	//  1. reserve a write position for the head
	//  2. store the new reference at that position
	// Enqueuers can't starve each other that way.
	//
	// However, the dequeuers now have to sometimes wait for the value written
	// in the ring to appear and have to spin, which is okay since the kernel
	// disables preemption around these two consecutive atomic operations.
	// See firehose_client_drain.
	__firehose_critical_region_enter();
	head = os_atomic_inc_orig(fbh_ring_head, relaxed);
	gen = head & FIREHOSE_RING_POS_GEN_MASK;
	idx = head & FIREHOSE_RING_POS_IDX_MASK;

	while (unlikely(!os_atomic_cmpxchgv(&fbh_ring[idx], gen, gen | ref, &dummy,
			relaxed))) {
		// can only ever happen if a recycler is slow, this requires having
		// enough cores (>5 for I/O e.g.)
		_dispatch_wait_until(fbh_ring[idx] == gen);
	}
	__firehose_critical_region_leave();
	__firehose_buffer_push_to_logd(fb, for_io);
#else
	// The algorithm is:
	//   1. read the head position
	//   2. cmpxchg head.gen with the (head.gen | ref) at head.idx
	//   3. if it fails wait until either the head cursor moves,
	//      or the cell becomes free
	//
	// The most likely stall at (3) is because another enqueuer raced us
	// and made the cell non empty.
	//
	// The alternative is to reserve the enqueue slot with an atomic inc.
	// Then write the ref into the ring. This would be much simpler as the
	// generation packing wouldn't be required (though setting the ring cell
	// would still need a cmpxchg loop to avoid clobbering values of slow
	// dequeuers)
	//
	// But then that means that flushers (logd) could be starved until that
	// finishes, and logd cannot be held forever (that could even be a logd
	// DoS from malicious programs). Meaning that logd would stop draining
	// buffer queues when encountering that issue, leading the program to be
	// stuck in firehose_client_push() apparently waiting on logd, while
	// really it's waiting on itself. It's better for the scheduler if we
	// make it clear that we're waiting on ourselves!

	head = os_atomic_load(fbh_ring_head, relaxed);
	for (;;) {
		gen = head & FIREHOSE_RING_POS_GEN_MASK;
		idx = head & FIREHOSE_RING_POS_IDX_MASK;

		// a thread being preempted here for GEN_MASK worth of ring rotations,
		// it could lead to the cmpxchg succeed, and have a bogus enqueue
		// (confused enqueuer)
		if (fastpath(os_atomic_cmpxchgv(&fbh_ring[idx], gen, gen | ref, &dummy,
				relaxed))) {
			if (fastpath(os_atomic_cmpxchgv(fbh_ring_head, head, head + 1,
					&head, release))) {
				__firehose_critical_region_leave();
				break;
			}
			// this thread is a confused enqueuer, need to undo enqueue
			os_atomic_store(&fbh_ring[idx], gen, relaxed);
			continue;
		}

		_dispatch_wait_until(({
			// wait until either the head moves (another enqueuer is done)
			// or (not very likely) a recycler is very slow
			// or (very unlikely) the confused thread undoes its enqueue
			uint16_t old_head = head;
			head = *fbh_ring_head;
			head != old_head || fbh_ring[idx] == gen;
		}));
	}

	pthread_priority_t pp = fc_pos.fcp_qos;
	pp <<= _PTHREAD_PRIORITY_QOS_CLASS_SHIFT;
	firehose_client_send_push_async(fb, _pthread_qos_class_decode(pp, NULL, NULL),
			for_io);
#endif
}

#ifndef KERNEL
void
firehose_buffer_force_connect(firehose_buffer_t fb)
{
	mach_port_t sendp = fb->fb_header.fbh_sendp;
	if (sendp == MACH_PORT_NULL) firehose_client_reconnect(fb, MACH_PORT_NULL);
}
#endif

OS_ALWAYS_INLINE
static inline uint16_t
firehose_buffer_ring_try_recycle(firehose_buffer_t fb)
{
	firehose_ring_tail_u pos, old;
	uint16_t volatile *fbh_ring;
	uint16_t gen, ref, entry, tail;
	firehose_chunk_t fc;
	bool for_io;

	os_atomic_rmw_loop2o(&fb->fb_header, fbh_ring_tail.frp_atomic_tail,
			old.frp_atomic_tail, pos.frp_atomic_tail, relaxed, {
		pos = old;
		if (fastpath(old.frp_mem_tail != old.frp_mem_flushed)) {
			pos.frp_mem_tail++;
		} else if (fastpath(old.frp_io_tail != old.frp_io_flushed)) {
			pos.frp_io_tail++;
		} else {
			os_atomic_rmw_loop_give_up(return 0);
		}
	});

	// there's virtually no chance that the lack of acquire barrier above
	// lets us read a value from the ring so stale that it's still an Empty
	// marker. For correctness purposes have a cheap loop that should never
	// really loop, instead of an acquire barrier in the cmpxchg above.
	for_io = (pos.frp_io_tail != old.frp_io_tail);
	if (for_io) {
		fbh_ring = fb->fb_header.fbh_io_ring;
		tail = old.frp_io_tail & FIREHOSE_RING_POS_IDX_MASK;
	} else {
		fbh_ring = fb->fb_header.fbh_mem_ring;
		tail = old.frp_mem_tail & FIREHOSE_RING_POS_IDX_MASK;
	}
	_dispatch_wait_until((entry = fbh_ring[tail]) & FIREHOSE_RING_POS_IDX_MASK);

	// Needed for process death handling (recycle-dequeue):
	// No atomic fences required, we merely want to make sure the observers
	// will see memory effects in program (asm) order.
	// 1. the chunk is marked as "void&full" (clobbering the pos with FULL_BIT)
	// 2. then we remove any reference to the chunk from the ring
	// This ensures that if we don't see a reference to a chunk in the ring
	// and it is dirty, it is a chunk being written to that needs a flush
	gen = (entry & FIREHOSE_RING_POS_GEN_MASK) + FIREHOSE_RING_POS_GEN_INC;
	ref = entry & FIREHOSE_RING_POS_IDX_MASK;
	fc = firehose_buffer_ref_to_chunk(fb, ref);

	if (!for_io && fc->fc_pos.fcp_stream == firehose_stream_metadata) {
		os_atomic_and2o(fb, fb_header.fbh_bank.fbb_metadata_bitmap,
				~(1ULL << ref), relaxed);
	}
	os_atomic_store2o(fc, fc_pos.fcp_atomic_pos,
			FIREHOSE_CHUNK_POS_FULL_BIT, relaxed);
	dispatch_compiler_barrier();
	os_atomic_store(&fbh_ring[tail], gen | 0, relaxed);
	return ref;
}

#ifndef KERNEL
OS_NOINLINE
static firehose_tracepoint_t
firehose_buffer_tracepoint_reserve_wait_for_chunks_from_logd(firehose_buffer_t fb,
		firehose_tracepoint_query_t ask, uint8_t **privptr, uint16_t ref)
{
	const uint64_t bank_unavail_mask = FIREHOSE_BANK_UNAVAIL_MASK(ask->for_io);
	const uint64_t bank_inc = FIREHOSE_BANK_INC(ask->for_io);
	firehose_buffer_bank_t const fbb = &fb->fb_header.fbh_bank;
	firehose_bank_state_u state;
	uint16_t fbs_max_ref;

	// first wait for our bank to have space, if needed
	if (!fastpath(ask->is_bank_ok)) {
		state.fbs_atomic_state =
				os_atomic_load2o(fbb, fbb_state.fbs_atomic_state, relaxed);
		while ((state.fbs_atomic_state - bank_inc) & bank_unavail_mask) {
			if (ask->quarantined) {
				__FIREHOSE_CLIENT_THROTTLED_DUE_TO_HEAVY_LOGGING__(fb,
						ask->for_io, &state);
			} else {
				firehose_client_send_push_and_wait(fb, ask->for_io, &state);
			}
			if (slowpath(fb->fb_header.fbh_sendp == MACH_PORT_DEAD)) {
				// logd was unloaded, give up
				return NULL;
			}
		}
		ask->is_bank_ok = true;
		fbs_max_ref = state.fbs_max_ref;
	} else {
		fbs_max_ref = fbb->fbb_state.fbs_max_ref;
	}

	// second, if we were passed a chunk, we may need to shrink
	if (slowpath(ref)) {
		goto try_shrink;
	}

	// third, wait for a chunk to come up, and if not, wait on the daemon
	for (;;) {
		if (fastpath(ref = firehose_buffer_ring_try_recycle(fb))) {
		try_shrink:
			if (slowpath(ref >= fbs_max_ref)) {
				ref = firehose_buffer_ring_shrink(fb, ref);
				if (!ref) {
					continue;
				}
			}
			break;
		}
		if (fastpath(ref = firehose_buffer_ring_try_grow(fbb, fbs_max_ref))) {
			break;
		}
		if (ask->quarantined) {
			__FIREHOSE_CLIENT_THROTTLED_DUE_TO_HEAVY_LOGGING__(fb,
					ask->for_io, &state);
		} else {
			firehose_client_send_push_and_wait(fb, ask->for_io, NULL);
		}
		if (slowpath(fb->fb_header.fbh_sendp == MACH_PORT_DEAD)) {
			// logd was unloaded, give up
			break;
		}
	}

	return firehose_buffer_stream_chunk_install(fb, ask, privptr, ref);
}
#else
static inline dispatch_lock
_dispatch_gate_lock_load_seq_cst(dispatch_gate_t l)
{
	return os_atomic_load(&l->dgl_lock, seq_cst);
}
OS_NOINLINE
static void
_dispatch_gate_wait(dispatch_gate_t l, uint32_t flags)
{
	(void)flags;
	_dispatch_wait_until(_dispatch_gate_lock_load_seq_cst(l) == 0);
}
#endif // KERNEL

firehose_tracepoint_t
firehose_buffer_tracepoint_reserve_slow(firehose_buffer_t fb,
		firehose_tracepoint_query_t ask, uint8_t **privptr)
{
	const unsigned for_io = ask->for_io;
	const firehose_buffer_bank_t fbb = &fb->fb_header.fbh_bank;
	firehose_bank_state_u state;
	uint16_t ref = 0;

	uint64_t unavail_mask = FIREHOSE_BANK_UNAVAIL_MASK(for_io);
#ifndef KERNEL
	state.fbs_atomic_state = os_atomic_add_orig2o(fbb,
			fbb_state.fbs_atomic_state, FIREHOSE_BANK_INC(for_io), acquire);
	if (fastpath(!(state.fbs_atomic_state & unavail_mask))) {
		ask->is_bank_ok = true;
		if (fastpath(ref = firehose_buffer_ring_try_recycle(fb))) {
			if (fastpath(ref < state.fbs_max_ref)) {
				return firehose_buffer_stream_chunk_install(fb, ask,
						privptr, ref);
			}
		}
	}
	return firehose_buffer_tracepoint_reserve_wait_for_chunks_from_logd(fb, ask,
			privptr, ref);
#else
	firehose_bank_state_u value;
	ask->is_bank_ok = os_atomic_rmw_loop2o(fbb, fbb_state.fbs_atomic_state,
			state.fbs_atomic_state, value.fbs_atomic_state, acquire, {
		value = state;
		if (slowpath((value.fbs_atomic_state & unavail_mask) != 0)) {
			os_atomic_rmw_loop_give_up(break);
		}
		value.fbs_atomic_state += FIREHOSE_BANK_INC(for_io);
	});
	if (ask->is_bank_ok) {
		ref = firehose_buffer_ring_try_recycle(fb);
		if (slowpath(ref == 0)) {
			// the kernel has no overlap between I/O and memory chunks,
			// having an available bank slot means we should be able to recycle
			DISPATCH_INTERNAL_CRASH(0, "Unable to recycle a chunk");
		}
	}
	// rdar://25137005 installing `0` unlocks the allocator
	return firehose_buffer_stream_chunk_install(fb, ask, privptr, ref);
#endif // KERNEL
}

#ifdef KERNEL
firehose_tracepoint_t
__firehose_buffer_tracepoint_reserve(uint64_t stamp, firehose_stream_t stream,
		uint16_t pubsize, uint16_t privsize, uint8_t **privptr)
{
	firehose_buffer_t fb = kernel_firehose_buffer;
	if (!fastpath(fb)) {
		return NULL;
	}
	return firehose_buffer_tracepoint_reserve(fb, stamp, stream, pubsize,
			privsize, privptr);
}

firehose_buffer_t
__firehose_buffer_create(size_t *size)
{
	if (!kernel_firehose_buffer) {
		kernel_firehose_buffer = firehose_buffer_create(MACH_PORT_NULL, 0, 0);
	}

	if (size) {
		*size = FIREHOSE_BUFFER_KERNEL_CHUNK_COUNT * FIREHOSE_CHUNK_SIZE;
	}
	return kernel_firehose_buffer;
}

void
__firehose_buffer_tracepoint_flush(firehose_tracepoint_t ft,
		firehose_tracepoint_id_u ftid)
{
	return firehose_buffer_tracepoint_flush(kernel_firehose_buffer, ft, ftid);
}

void
__firehose_merge_updates(firehose_push_reply_t update)
{
	firehose_buffer_t fb = kernel_firehose_buffer;
	if (fastpath(fb)) {
		firehose_client_merge_updates(fb, true, update, false, NULL);
	}
}
#endif // KERNEL

#endif // OS_FIREHOSE_SPI
