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

#include <servers/bootstrap.h>
#include <sys/ioctl.h>
#include <sys/ttycom.h>
#include <sys/uio.h>
#include "internal.h"
#include "firehoseServer.h" // MiG
#include "firehose_reply.h" // MiG

#if __has_feature(c_static_assert)
_Static_assert(offsetof(struct firehose_client_s, fc_mem_sent_flushed_pos)
		% 8 == 0, "Make sure atomic fields are properly aligned");
#endif

static struct firehose_server_s {
	mach_port_t			fs_bootstrap_port;
	dispatch_mach_t		fs_mach_channel;
	dispatch_queue_t	fs_ipc_queue;
	dispatch_queue_t	fs_snapshot_gate_queue;
	dispatch_queue_t	fs_io_drain_queue;
	dispatch_queue_t	fs_mem_drain_queue;
	firehose_handler_t	fs_handler;

	firehose_snapshot_t fs_snapshot;

	int					fs_kernel_fd;
	firehose_client_t	fs_kernel_client;

	TAILQ_HEAD(, firehose_client_s) fs_clients;
} server_config = {
	.fs_clients = TAILQ_HEAD_INITIALIZER(server_config.fs_clients),
	.fs_kernel_fd = -1,
};

#pragma mark -
#pragma mark firehose client state machine

static void firehose_server_demux(firehose_client_t fc,
		mach_msg_header_t *msg_hdr);
static void firehose_client_cancel(firehose_client_t fc);
static void firehose_client_snapshot_finish(firehose_client_t fc,
		firehose_snapshot_t snapshot, bool for_io);

static void
firehose_client_notify(firehose_client_t fc, mach_port_t reply_port)
{
	firehose_push_reply_t push_reply = {
		.fpr_mem_flushed_pos = os_atomic_load2o(fc, fc_mem_flushed_pos,relaxed),
		.fpr_io_flushed_pos = os_atomic_load2o(fc, fc_io_flushed_pos, relaxed),
	};
	kern_return_t kr;

	firehose_atomic_max2o(fc, fc_mem_sent_flushed_pos,
			push_reply.fpr_mem_flushed_pos, relaxed);
	firehose_atomic_max2o(fc, fc_io_sent_flushed_pos,
			push_reply.fpr_io_flushed_pos, relaxed);

	if (fc->fc_is_kernel) {
		if (ioctl(server_config.fs_kernel_fd, LOGFLUSHED, &push_reply) < 0) {
			dispatch_assume_zero(errno);
		}
	} else {
		if (reply_port == fc->fc_sendp) {
			kr = firehose_send_push_notify_async(reply_port, push_reply, 0);
		} else {
			kr = firehose_send_push_reply(reply_port, KERN_SUCCESS, push_reply);
		}
		if (kr != MACH_SEND_INVALID_DEST) {
			DISPATCH_VERIFY_MIG(kr);
			dispatch_assume_zero(kr);
		}
	}
}

OS_ALWAYS_INLINE
static inline uint16_t
firehose_client_acquire_head(firehose_buffer_t fb, bool for_io)
{
	uint16_t head;
	if (for_io) {
		head = os_atomic_load2o(&fb->fb_header, fbh_ring_io_head, acquire);
	} else {
		head = os_atomic_load2o(&fb->fb_header, fbh_ring_mem_head, acquire);
	}
	return head;
}

OS_ALWAYS_INLINE
static inline void
firehose_client_push_async_merge(firehose_client_t fc, pthread_priority_t pp,
		bool for_io)
{
	if (for_io) {
		_dispatch_source_merge_data(fc->fc_io_source, pp, 1);
	} else {
		_dispatch_source_merge_data(fc->fc_mem_source, pp, 1);
	}
}

OS_NOINLINE OS_COLD
static void
firehose_client_mark_corrupted(firehose_client_t fc, mach_port_t reply_port)
{
	// this client is really confused, do *not* answer to asyncs anymore
	fc->fc_memory_corrupted = true;
	fc->fc_use_notifs = false;

	// XXX: do not cancel the data sources or a corrupted client could
	// prevent snapshots from being taken if unlucky with ordering

	if (reply_port) {
		kern_return_t kr = firehose_send_push_reply(reply_port, 0,
				FIREHOSE_PUSH_REPLY_CORRUPTED);
		DISPATCH_VERIFY_MIG(kr);
		dispatch_assume_zero(kr);
	}
}

OS_ALWAYS_INLINE
static inline void
firehose_client_snapshot_mark_done(firehose_client_t fc,
		firehose_snapshot_t snapshot, bool for_io)
{
	if (for_io) {
		fc->fc_needs_io_snapshot = false;
	} else {
		fc->fc_needs_mem_snapshot = false;
	}
	dispatch_group_leave(snapshot->fs_group);
}

#define DRAIN_BATCH_SIZE  4
#define FIREHOSE_DRAIN_FOR_IO 0x1
#define FIREHOSE_DRAIN_POLL 0x2

OS_NOINLINE
static void
firehose_client_drain(firehose_client_t fc, mach_port_t port, uint32_t flags)
{
	firehose_buffer_t fb = fc->fc_buffer;
	firehose_buffer_chunk_t fbc;
	firehose_event_t evt;
	uint16_t volatile *fbh_ring;
	uint16_t flushed, ref, count = 0;
	uint16_t client_head, client_flushed, sent_flushed;
	firehose_snapshot_t snapshot = NULL;
	bool for_io = (flags & FIREHOSE_DRAIN_FOR_IO);

	if (for_io) {
		evt = FIREHOSE_EVENT_IO_BUFFER_RECEIVED;
		_Static_assert(FIREHOSE_EVENT_IO_BUFFER_RECEIVED ==
				FIREHOSE_SNAPSHOT_EVENT_IO_BUFFER, "");
		fbh_ring = fb->fb_header.fbh_io_ring;
		sent_flushed = (uint16_t)fc->fc_io_sent_flushed_pos;
		flushed = (uint16_t)fc->fc_io_flushed_pos;
		if (fc->fc_needs_io_snapshot) {
			snapshot = server_config.fs_snapshot;
		}
	} else {
		evt = FIREHOSE_EVENT_MEM_BUFFER_RECEIVED;
		_Static_assert(FIREHOSE_EVENT_MEM_BUFFER_RECEIVED ==
				FIREHOSE_SNAPSHOT_EVENT_MEM_BUFFER, "");
		fbh_ring = fb->fb_header.fbh_mem_ring;
		sent_flushed = (uint16_t)fc->fc_mem_sent_flushed_pos;
		flushed = (uint16_t)fc->fc_mem_flushed_pos;
		if (fc->fc_needs_mem_snapshot) {
			snapshot = server_config.fs_snapshot;
		}
	}

	if (slowpath(fc->fc_memory_corrupted)) {
		goto corrupt;
	}

	client_head = flushed;
	do {
		if ((uint16_t)(flushed + count) == client_head) {
			client_head = firehose_client_acquire_head(fb, for_io);
			if ((uint16_t)(flushed + count) == client_head) {
				break;
			}
			if ((uint16_t)(client_head - sent_flushed) >=
					FIREHOSE_BUFFER_CHUNK_COUNT) {
				goto corrupt;
			}
		}

		// see firehose_buffer_ring_enqueue
		do {
			ref = (flushed + count) & FIREHOSE_RING_POS_IDX_MASK;
			ref = os_atomic_load(&fbh_ring[ref], relaxed);
			ref &= FIREHOSE_RING_POS_IDX_MASK;
		} while (fc->fc_is_kernel && !ref);
		count++;
		if (!ref) {
			_dispatch_debug("Ignoring invalid page reference in ring: %d", ref);
			continue;
		}

		fbc = firehose_buffer_ref_to_chunk(fb, ref);
		server_config.fs_handler(fc, evt, fbc);
		if (slowpath(snapshot)) {
			snapshot->handler(fc, evt, fbc);
		}
		// clients not using notifications (single threaded) always drain fully
		// because they use all their limit, always
	} while (!fc->fc_use_notifs || count < DRAIN_BATCH_SIZE || snapshot);

	if (count) {
		// we don't load the full fbh_ring_tail because that is a 64bit quantity
		// and we only need 16bits from it. and on 32bit arm, there's no way to
		// perform an atomic load of a 64bit quantity on read-only memory.
		if (for_io) {
			os_atomic_add2o(fc, fc_io_flushed_pos, count, relaxed);
			client_flushed = os_atomic_load2o(&fb->fb_header,
				fbh_ring_tail.frp_io_flushed, relaxed);
		} else {
			os_atomic_add2o(fc, fc_mem_flushed_pos, count, relaxed);
			client_flushed = os_atomic_load2o(&fb->fb_header,
				fbh_ring_tail.frp_mem_flushed, relaxed);
		}
		if (fc->fc_is_kernel) {
			// will fire firehose_client_notify() because port is MACH_PORT_DEAD
			port = fc->fc_sendp;
		} else if (!port && client_flushed == sent_flushed && fc->fc_use_notifs) {
			port = fc->fc_sendp;
		}
	}

	if (slowpath(snapshot)) {
		firehose_client_snapshot_finish(fc, snapshot, for_io);
		firehose_client_snapshot_mark_done(fc, snapshot, for_io);
	}
	if (port) {
		firehose_client_notify(fc, port);
	}
	if (fc->fc_is_kernel) {
		if (!(flags & FIREHOSE_DRAIN_POLL)) {
			// see firehose_client_kernel_source_handle_event
			dispatch_resume(fc->fc_kernel_source);
		}
	} else {
		if (fc->fc_use_notifs && count >= DRAIN_BATCH_SIZE) {
			// if we hit the drain batch size, the client probably logs a lot
			// and there's more to drain, so optimistically schedule draining
			// again this is cheap since the queue is hot, and is fair for other
			// clients
			firehose_client_push_async_merge(fc, 0, for_io);
		}
		if (count && server_config.fs_kernel_client) {
			// the kernel is special because it can drop messages, so if we're
			// draining, poll the kernel each time while we're bound to a thread
			firehose_client_drain(server_config.fs_kernel_client,
					MACH_PORT_NULL, flags | FIREHOSE_DRAIN_POLL);
		}
	}
	return;

corrupt:
	if (snapshot) {
		firehose_client_snapshot_mark_done(fc, snapshot, for_io);
	}
	firehose_client_mark_corrupted(fc, port);
	// from now on all IO/mem drains depending on `for_io` will be no-op
	// (needs_<for_io>_snapshot: false, memory_corrupted: true). we can safely
	// silence the corresponding source of drain wake-ups.
	if (!fc->fc_is_kernel) {
		dispatch_source_cancel(for_io ? fc->fc_io_source : fc->fc_mem_source);
	}
}

static void
firehose_client_drain_io_async(void *ctx)
{
	firehose_client_drain(ctx, MACH_PORT_NULL, FIREHOSE_DRAIN_FOR_IO);
}

static void
firehose_client_drain_mem_async(void *ctx)
{
	firehose_client_drain(ctx, MACH_PORT_NULL, 0);
}

OS_NOINLINE
static void
firehose_client_finalize(firehose_client_t fc OS_OBJECT_CONSUMED)
{
	firehose_snapshot_t snapshot = server_config.fs_snapshot;
	firehose_buffer_t fb = fc->fc_buffer;

	dispatch_assert_queue(server_config.fs_io_drain_queue);

	// if a client dies between phase 1 and 2 of starting the snapshot
	// (see firehose_snapshot_start)) there's no handler to call, but the
	// dispatch group has to be adjusted for this client going away.
	if (fc->fc_needs_io_snapshot) {
		dispatch_group_leave(snapshot->fs_group);
		fc->fc_needs_io_snapshot = false;
	}
	if (fc->fc_needs_mem_snapshot) {
		dispatch_group_leave(snapshot->fs_group);
		fc->fc_needs_mem_snapshot = false;
	}
	if (fc->fc_memory_corrupted) {
		server_config.fs_handler(fc, FIREHOSE_EVENT_CLIENT_CORRUPTED,
				&fb->fb_chunks[0]);
	}
	server_config.fs_handler(fc, FIREHOSE_EVENT_CLIENT_DIED, NULL);

	TAILQ_REMOVE(&server_config.fs_clients, fc, fc_entry);
	fc->fc_entry.tqe_next = DISPATCH_OBJECT_LISTLESS;
	fc->fc_entry.tqe_prev = DISPATCH_OBJECT_LISTLESS;
	_os_object_release(&fc->fc_as_os_object);
}

OS_NOINLINE
static void
firehose_client_handle_death(void *ctxt)
{
	firehose_client_t fc = ctxt;
	firehose_buffer_t fb = fc->fc_buffer;
	firehose_buffer_header_t fbh = &fb->fb_header;
	uint64_t mem_bitmap = 0, bitmap;

	if (fc->fc_memory_corrupted) {
		return firehose_client_finalize(fc);
	}

	dispatch_assert_queue(server_config.fs_io_drain_queue);

	// acquire to match release barriers from threads that died
	os_atomic_thread_fence(acquire);

	bitmap = fbh->fbh_bank.fbb_bitmap & ~1ULL;
	for (int for_io = 0; for_io < 2; for_io++) {
		uint16_t volatile *fbh_ring;
		uint16_t tail, flushed;

		if (for_io) {
			fbh_ring = fbh->fbh_io_ring;
			tail = fbh->fbh_ring_tail.frp_io_tail;
			flushed = (uint16_t)fc->fc_io_flushed_pos;
		} else {
			fbh_ring = fbh->fbh_mem_ring;
			tail = fbh->fbh_ring_tail.frp_mem_tail;
			flushed = (uint16_t)fc->fc_mem_flushed_pos;
		}
		if ((uint16_t)(flushed - tail) >= FIREHOSE_BUFFER_CHUNK_COUNT) {
			fc->fc_memory_corrupted = true;
			return firehose_client_finalize(fc);
		}

		// remove the pages that we flushed already from the bitmap
		for (; tail != flushed; tail++) {
			uint16_t ring_pos = tail & FIREHOSE_RING_POS_IDX_MASK;
			uint16_t ref = fbh_ring[ring_pos] & FIREHOSE_RING_POS_IDX_MASK;

			bitmap &= ~(1ULL << ref);
		}
	}

	firehose_snapshot_t snapshot = server_config.fs_snapshot;

	// Then look at all the allocated pages not seen in the ring
	while (bitmap) {
		uint16_t ref = firehose_bitmap_first_set(bitmap);
		firehose_buffer_chunk_t fbc = firehose_buffer_ref_to_chunk(fb, ref);
		uint16_t fbc_length = fbc->fbc_pos.fbc_next_entry_offs;

		bitmap &= ~(1ULL << ref);
		if (fbc->fbc_start + fbc_length <= fbc->fbc_data) {
			// this page has its "recycle-requeue" done, but hasn't gone
			// through "recycle-reuse", or it has no data, ditch it
			continue;
		}
		if (!((firehose_tracepoint_t)fbc->fbc_data)->ft_length) {
			// this thing has data, but the first tracepoint is unreadable
			// so also just ditch it
			continue;
		}
		if (!fbc->fbc_pos.fbc_flag_io) {
			mem_bitmap |= 1ULL << ref;
			continue;
		}
		server_config.fs_handler(fc, FIREHOSE_EVENT_IO_BUFFER_RECEIVED, fbc);
		if (fc->fc_needs_io_snapshot && snapshot) {
			snapshot->handler(fc, FIREHOSE_SNAPSHOT_EVENT_IO_BUFFER, fbc);
		}
	}

	if (!mem_bitmap) {
		return firehose_client_finalize(fc);
	}

	dispatch_async(server_config.fs_mem_drain_queue, ^{
		uint64_t mem_bitmap_copy = mem_bitmap;

		while (mem_bitmap_copy) {
			uint16_t ref = firehose_bitmap_first_set(mem_bitmap_copy);
			firehose_buffer_chunk_t fbc = firehose_buffer_ref_to_chunk(fb, ref);

			mem_bitmap_copy &= ~(1ULL << ref);
			server_config.fs_handler(fc, FIREHOSE_EVENT_MEM_BUFFER_RECEIVED, fbc);
			if (fc->fc_needs_mem_snapshot && snapshot) {
				snapshot->handler(fc, FIREHOSE_SNAPSHOT_EVENT_MEM_BUFFER, fbc);
			}
		}

		dispatch_async_f(server_config.fs_io_drain_queue, fc,
				(dispatch_function_t)firehose_client_finalize);
	});
}

static void
firehose_client_handle_mach_event(void *ctx, dispatch_mach_reason_t reason,
		dispatch_mach_msg_t dmsg, mach_error_t error OS_UNUSED)
{
	mach_msg_header_t *msg_hdr;
	firehose_client_t fc = ctx;
	mach_port_t oldsendp, oldrecvp;

	if (dmsg) {
		msg_hdr = dispatch_mach_msg_get_msg(dmsg, NULL);
		oldsendp = msg_hdr->msgh_remote_port;
		oldrecvp = msg_hdr->msgh_local_port;
	}

	switch (reason) {
	case DISPATCH_MACH_MESSAGE_RECEIVED:
		if (msg_hdr->msgh_id == MACH_NOTIFY_NO_SENDERS) {
			_dispatch_debug("FIREHOSE NO_SENDERS (unique_pid: 0x%llx)",
					firehose_client_get_unique_pid(fc, NULL));
			dispatch_mach_cancel(fc->fc_mach_channel);
		} else {
			firehose_server_demux(fc, msg_hdr);
		}
		break;

	case DISPATCH_MACH_DISCONNECTED:
		if (oldsendp) {
			if (slowpath(oldsendp != fc->fc_sendp)) {
				DISPATCH_INTERNAL_CRASH(oldsendp,
						"disconnect event about unknown send-right");
			}
			firehose_mach_port_send_release(fc->fc_sendp);
			fc->fc_sendp = MACH_PORT_NULL;
		}
		if (oldrecvp) {
			if (slowpath(oldrecvp != fc->fc_recvp)) {
				DISPATCH_INTERNAL_CRASH(oldrecvp,
						"disconnect event about unknown receive-right");
			}
			firehose_mach_port_recv_dispose(fc->fc_recvp, fc);
			fc->fc_recvp = MACH_PORT_NULL;
		}
		if (fc->fc_recvp == MACH_PORT_NULL && fc->fc_sendp == MACH_PORT_NULL) {
			firehose_client_cancel(fc);
		}
		break;
	}
}

#if !TARGET_OS_SIMULATOR
static void
firehose_client_kernel_source_handle_event(void *ctxt)
{
	firehose_client_t fc = ctxt;

	// resumed in firehose_client_drain for both memory and I/O
	dispatch_suspend(fc->fc_kernel_source);
	dispatch_suspend(fc->fc_kernel_source);
	dispatch_async_f(server_config.fs_mem_drain_queue,
			fc, firehose_client_drain_mem_async);
	dispatch_async_f(server_config.fs_io_drain_queue,
			fc, firehose_client_drain_io_async);
}
#endif

static inline void
firehose_client_resume(firehose_client_t fc,
		const struct firehose_client_connected_info_s *fcci)
{
	dispatch_assert_queue(server_config.fs_io_drain_queue);
	TAILQ_INSERT_TAIL(&server_config.fs_clients, fc, fc_entry);
	server_config.fs_handler(fc, FIREHOSE_EVENT_CLIENT_CONNECTED, (void *)fcci);
	if (fc->fc_is_kernel) {
		dispatch_activate(fc->fc_kernel_source);
	} else {
		dispatch_mach_connect(fc->fc_mach_channel,
				fc->fc_recvp, fc->fc_sendp, NULL);
		dispatch_activate(fc->fc_io_source);
		dispatch_activate(fc->fc_mem_source);
	}
}

static void
firehose_client_cancel(firehose_client_t fc)
{
	dispatch_mach_t dm;
	dispatch_block_t block;

	_dispatch_debug("client died (unique_pid: 0x%llx",
			firehose_client_get_unique_pid(fc, NULL));

	dm = fc->fc_mach_channel;
	fc->fc_mach_channel = NULL;
	dispatch_release(dm);

	fc->fc_use_notifs = false;
	dispatch_source_cancel(fc->fc_io_source);
	dispatch_source_cancel(fc->fc_mem_source);

	block = dispatch_block_create(DISPATCH_BLOCK_DETACHED, ^{
		dispatch_async_f(server_config.fs_io_drain_queue, fc,
				firehose_client_handle_death);
	});
	dispatch_async(server_config.fs_mem_drain_queue, block);
	_Block_release(block);
}

static firehose_client_t
_firehose_client_create(firehose_buffer_t fb)
{
	firehose_client_t fc;

	fc = (firehose_client_t)_os_object_alloc_realized(FIREHOSE_CLIENT_CLASS,
			sizeof(struct firehose_client_s));
	fc->fc_buffer = fb;
	fc->fc_mem_flushed_pos = fb->fb_header.fbh_bank.fbb_mem_flushed;
	fc->fc_mem_sent_flushed_pos = fc->fc_mem_flushed_pos;
	fc->fc_io_flushed_pos = fb->fb_header.fbh_bank.fbb_io_flushed;
	fc->fc_io_sent_flushed_pos = fc->fc_io_flushed_pos;
	return fc;
}

static firehose_client_t
firehose_client_create(firehose_buffer_t fb,
		mach_port_t comm_recvp, mach_port_t comm_sendp)
{
	uint64_t unique_pid = fb->fb_header.fbh_uniquepid;
	firehose_client_t fc = _firehose_client_create(fb);
	dispatch_mach_t dm;
	dispatch_source_t ds;

	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_DATA_OR, 0, 0,
			server_config.fs_mem_drain_queue);
	_os_object_retain_internal_inline(&fc->fc_as_os_object);
	dispatch_set_context(ds, fc);
	dispatch_set_finalizer_f(ds,
			(dispatch_function_t)_os_object_release_internal);
	dispatch_source_set_event_handler_f(ds, firehose_client_drain_mem_async);
	fc->fc_mem_source = ds;

	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_DATA_OR, 0, 0,
			server_config.fs_io_drain_queue);
	_os_object_retain_internal_inline(&fc->fc_as_os_object);
	dispatch_set_context(ds, fc);
	dispatch_set_finalizer_f(ds,
			(dispatch_function_t)_os_object_release_internal);
	dispatch_source_set_event_handler_f(ds, firehose_client_drain_io_async);
	fc->fc_io_source = ds;

	_dispatch_debug("FIREHOSE_REGISTER (unique_pid: 0x%llx)", unique_pid);
	fc->fc_recvp = comm_recvp;
	fc->fc_sendp = comm_sendp;
	firehose_mach_port_guard(comm_recvp, true, fc);
	dm = dispatch_mach_create_f("com.apple.firehose.peer",
			server_config.fs_ipc_queue,
			fc, firehose_client_handle_mach_event);
	fc->fc_mach_channel = dm;
	return fc;
}

static void
firehose_kernel_client_create(void)
{
#if !TARGET_OS_SIMULATOR
	struct firehose_server_s *fs = &server_config;
	firehose_buffer_map_info_t fb_map;
	firehose_client_t fc;
	dispatch_source_t ds;
	int fd;

	while ((fd = open("/dev/oslog", O_RDWR)) < 0) {
		if (errno == EINTR) {
			continue;
		}
		if (errno == ENOENT) {
			return;
		}
		DISPATCH_INTERNAL_CRASH(errno, "Unable to open /dev/oslog");
	}

	while (ioctl(fd, LOGBUFFERMAP, &fb_map) < 0) {
		if (errno == EINTR) {
			continue;
		}
		DISPATCH_INTERNAL_CRASH(errno, "Unable to map kernel buffer");
	}
	if (fb_map.fbmi_size !=
			FIREHOSE_BUFFER_KERNEL_CHUNK_COUNT * FIREHOSE_BUFFER_CHUNK_SIZE) {
		DISPATCH_INTERNAL_CRASH(fb_map.fbmi_size, "Unexpected kernel buffer size");
	}

	fc = _firehose_client_create((firehose_buffer_t)(uintptr_t)fb_map.fbmi_addr);
	fc->fc_is_kernel = true;
	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0,
			fs->fs_ipc_queue);
	dispatch_set_context(ds, fc);
	dispatch_source_set_event_handler_f(ds,
			firehose_client_kernel_source_handle_event);
	fc->fc_kernel_source = ds;
	fc->fc_use_notifs = true;
	fc->fc_sendp = MACH_PORT_DEAD; // causes drain() to call notify

	fs->fs_kernel_fd = fd;
	fs->fs_kernel_client = fc;
#endif
}

void
_firehose_client_dispose(firehose_client_t fc)
{
	vm_deallocate(mach_task_self(), (vm_address_t)fc->fc_buffer,
			sizeof(*fc->fc_buffer));
	fc->fc_buffer = NULL;
	server_config.fs_handler(fc, FIREHOSE_EVENT_CLIENT_FINALIZE, NULL);
}

void
_firehose_client_xref_dispose(firehose_client_t fc)
{
	_dispatch_debug("Cleaning up client info for unique_pid 0x%llx",
			firehose_client_get_unique_pid(fc, NULL));

	dispatch_release(fc->fc_io_source);
	fc->fc_io_source = NULL;

	dispatch_release(fc->fc_mem_source);
	fc->fc_mem_source = NULL;
}

uint64_t
firehose_client_get_unique_pid(firehose_client_t fc, pid_t *pid_out)
{
	firehose_buffer_header_t fbh = &fc->fc_buffer->fb_header;
	if (fc->fc_is_kernel) {
		if (pid_out) *pid_out = 0;
		return 0;
	}
	if (pid_out) *pid_out = fbh->fbh_pid ?: ~(pid_t)0;
	return fbh->fbh_uniquepid ?: ~0ull;
}

void *
firehose_client_get_metadata_buffer(firehose_client_t client, size_t *size)
{
	firehose_buffer_header_t fbh = &client->fc_buffer->fb_header;

	*size = FIREHOSE_BUFFER_LIBTRACE_HEADER_SIZE;
	return (void *)((uintptr_t)(fbh + 1) - *size);
}

void *
firehose_client_get_context(firehose_client_t fc)
{
	return os_atomic_load2o(fc, fc_ctxt, relaxed);
}

void *
firehose_client_set_context(firehose_client_t fc, void *ctxt)
{
	return os_atomic_xchg2o(fc, fc_ctxt, ctxt, relaxed);
}

#pragma mark -
#pragma mark firehose server

/*
 * The current_message context stores the client info for the current message
 * being handled. The only reason this works is because currently the message
 * processing is serial. If that changes, this would not work.
 */
static firehose_client_t cur_client_info;

static void
firehose_server_handle_mach_event(void *ctx OS_UNUSED,
		dispatch_mach_reason_t reason, dispatch_mach_msg_t dmsg,
		mach_error_t error OS_UNUSED)
{
	mach_msg_header_t *msg_hdr = NULL;

	if (reason == DISPATCH_MACH_MESSAGE_RECEIVED) {
		msg_hdr = dispatch_mach_msg_get_msg(dmsg, NULL);
		/* TODO: Assert this should be a register message */
		firehose_server_demux(NULL, msg_hdr);
	}
}

void
firehose_server_init(mach_port_t comm_port, firehose_handler_t handler)
{
	struct firehose_server_s *fs = &server_config;
	dispatch_queue_attr_t attr;
	dispatch_mach_t dm;

	// just reference the string so that it's captured
	(void)os_atomic_load(&__libfirehose_serverVersionString[0], relaxed);

	attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
			QOS_CLASS_USER_INITIATED, 0);
	fs->fs_ipc_queue = dispatch_queue_create_with_target(
			"com.apple.firehose.ipc", attr, NULL);
	fs->fs_snapshot_gate_queue = dispatch_queue_create_with_target(
			"com.apple.firehose.snapshot-gate", DISPATCH_QUEUE_SERIAL, NULL);
	fs->fs_io_drain_queue = dispatch_queue_create_with_target(
			"com.apple.firehose.drain-io", DISPATCH_QUEUE_SERIAL, NULL);
	fs->fs_mem_drain_queue = dispatch_queue_create_with_target(
			"com.apple.firehose.drain-mem", DISPATCH_QUEUE_SERIAL, NULL);

	dm = dispatch_mach_create_f("com.apple.firehose.listener",
			fs->fs_ipc_queue, NULL, firehose_server_handle_mach_event);
	fs->fs_bootstrap_port = comm_port;
	fs->fs_mach_channel = dm;
	fs->fs_handler = _Block_copy(handler);
	firehose_kernel_client_create();
}

void
firehose_server_assert_spi_version(uint32_t spi_version)
{
	if (spi_version != OS_FIREHOSE_SPI_VERSION) {
		DISPATCH_CLIENT_CRASH(spi_version, "firehose server version mismatch ("
				OS_STRINGIFY(OS_FIREHOSE_SPI_VERSION) ")");
	}
	if (_firehose_spi_version != OS_FIREHOSE_SPI_VERSION) {
		DISPATCH_CLIENT_CRASH(_firehose_spi_version,
				"firehose libdispatch version mismatch ("
				OS_STRINGIFY(OS_FIREHOSE_SPI_VERSION) ")");

	}
}

void
firehose_server_resume(void)
{
	struct firehose_server_s *fs = &server_config;

	if (fs->fs_kernel_client) {
		dispatch_async(fs->fs_io_drain_queue, ^{
			struct firehose_client_connected_info_s fcci = {
				.fcci_version = FIREHOSE_CLIENT_CONNECTED_INFO_VERSION,
			};
			firehose_client_resume(fs->fs_kernel_client, &fcci);
		});
	}
	dispatch_mach_connect(fs->fs_mach_channel, fs->fs_bootstrap_port,
			MACH_PORT_NULL, NULL);
}

#pragma mark -
#pragma mark firehose snapshot and peeking

void
firehose_client_metadata_stream_peek(firehose_client_t fc,
		firehose_event_t context, bool (^peek_should_start)(void),
		bool (^peek)(firehose_buffer_chunk_t fbc))
{
	if (context != FIREHOSE_EVENT_MEM_BUFFER_RECEIVED) {
		return dispatch_sync(server_config.fs_mem_drain_queue, ^{
			firehose_client_metadata_stream_peek(fc,
					FIREHOSE_EVENT_MEM_BUFFER_RECEIVED, peek_should_start, peek);
		});
	}

	if (peek_should_start && !peek_should_start()) {
		return;
	}

	firehose_buffer_t fb = fc->fc_buffer;
	firehose_buffer_header_t fbh = &fb->fb_header;
	uint64_t bitmap = fbh->fbh_bank.fbb_metadata_bitmap;

	while (bitmap) {
		uint16_t ref = firehose_bitmap_first_set(bitmap);
		firehose_buffer_chunk_t fbc = firehose_buffer_ref_to_chunk(fb, ref);
		uint16_t fbc_length = fbc->fbc_pos.fbc_next_entry_offs;

		bitmap &= ~(1ULL << ref);
		if (fbc->fbc_start + fbc_length <= fbc->fbc_data) {
			// this page has its "recycle-requeue" done, but hasn't gone
			// through "recycle-reuse", or it has no data, ditch it
			continue;
		}
		if (!((firehose_tracepoint_t)fbc->fbc_data)->ft_length) {
			// this thing has data, but the first tracepoint is unreadable
			// so also just ditch it
			continue;
		}
		if (fbc->fbc_pos.fbc_stream != firehose_stream_metadata) {
			continue;
		}
		if (!peek(fbc)) {
			break;
		}
	}
}

OS_NOINLINE OS_COLD
static void
firehose_client_snapshot_finish(firehose_client_t fc,
		firehose_snapshot_t snapshot, bool for_io)
{
	firehose_buffer_t fb = fc->fc_buffer;
	firehose_buffer_header_t fbh = &fb->fb_header;
	firehose_snapshot_event_t evt;
	uint16_t volatile *fbh_ring;
	uint16_t tail, flushed;
	uint64_t bitmap;

	bitmap = ~1ULL;

	if (for_io) {
		fbh_ring = fbh->fbh_io_ring;
		tail = fbh->fbh_ring_tail.frp_io_tail;
		flushed = (uint16_t)fc->fc_io_flushed_pos;
		evt = FIREHOSE_SNAPSHOT_EVENT_IO_BUFFER;
	} else {
		fbh_ring = fbh->fbh_mem_ring;
		tail = fbh->fbh_ring_tail.frp_mem_tail;
		flushed = (uint16_t)fc->fc_mem_flushed_pos;
		evt = FIREHOSE_SNAPSHOT_EVENT_MEM_BUFFER;
	}
	if ((uint16_t)(flushed - tail) >= FIREHOSE_BUFFER_CHUNK_COUNT) {
		fc->fc_memory_corrupted = true;
		return;
	}

	// remove the pages that we flushed already from the bitmap
	for (; tail != flushed; tail++) {
		uint16_t idx = tail & FIREHOSE_RING_POS_IDX_MASK;
		uint16_t ref = fbh_ring[idx] & FIREHOSE_RING_POS_IDX_MASK;

		bitmap &= ~(1ULL << ref);
	}

	// Remove pages that are free by AND-ing with the allocating bitmap.
	// The load of fbb_bitmap may not be atomic, but it's ok because bits
	// being flipped are pages we don't care about snapshotting. The worst thing
	// that can happen is that we go peek at an unmapped page and we fault it in
	bitmap &= fbh->fbh_bank.fbb_bitmap;

	// Then look at all the allocated pages not seen in the ring
	while (bitmap) {
		uint16_t ref = firehose_bitmap_first_set(bitmap);
		firehose_buffer_chunk_t fbc = firehose_buffer_ref_to_chunk(fb, ref);
		uint16_t fbc_length = fbc->fbc_pos.fbc_next_entry_offs;

		bitmap &= ~(1ULL << ref);
		if (fbc->fbc_start + fbc_length <= fbc->fbc_data) {
			// this page has its "recycle-requeue" done, but hasn't gone
			// through "recycle-reuse", or it has no data, ditch it
			continue;
		}
		if (!((firehose_tracepoint_t)fbc->fbc_data)->ft_length) {
			// this thing has data, but the first tracepoint is unreadable
			// so also just ditch it
			continue;
		}
		if (fbc->fbc_pos.fbc_flag_io != for_io) {
			continue;
		}
		snapshot->handler(fc, evt, fbc);
	}
}

static void
firehose_snapshot_start(void *ctxt)
{
	firehose_snapshot_t snapshot = ctxt;
	firehose_client_t fci;
	long n = 0;

	// 0. we need to be on the IO queue so that client connection and/or death
	//    cannot happen concurrently
	dispatch_assert_queue(server_config.fs_io_drain_queue);

	// 1. mark all the clients participating in the current snapshot
	//    and enter the group for each bit set
	TAILQ_FOREACH(fci, &server_config.fs_clients, fc_entry) {
		if (fci->fc_is_kernel) {
#if TARGET_OS_SIMULATOR
			continue;
#endif
		}
		if (slowpath(fci->fc_memory_corrupted)) {
			continue;
		}
		fci->fc_needs_io_snapshot = true;
		fci->fc_needs_mem_snapshot = true;
		n += 2;
	}
	if (n) {
		// cheating: equivalent to dispatch_group_enter() n times
		// without the acquire barriers that we don't need
		os_atomic_add2o(snapshot->fs_group, dg_value, n, relaxed);
	}

	dispatch_async(server_config.fs_mem_drain_queue, ^{
		// 2. make fs_snapshot visible, this is what triggers the snapshot
		//    logic from _drain() or handle_death(). until fs_snapshot is
		//    published, the bits set above are mostly ignored
		server_config.fs_snapshot = snapshot;

		snapshot->handler(NULL, FIREHOSE_SNAPSHOT_EVENT_MEM_START, NULL);

		dispatch_async(server_config.fs_io_drain_queue, ^{
			firehose_client_t fcj;

			snapshot->handler(NULL, FIREHOSE_SNAPSHOT_EVENT_IO_START, NULL);

			// match group_enter from firehose_snapshot() after MEM+IO_START
			dispatch_group_leave(snapshot->fs_group);

			// 3. tickle all the clients. the list of clients may have changed
			//    since step 1, but worry not - new clients don't have
			//    fc_needs_*_snapshot set so drain is harmless; clients that
			//    were removed from the list have already left the group
			//    (see firehose_client_finalize())
			TAILQ_FOREACH(fcj, &server_config.fs_clients, fc_entry) {
				if (fcj->fc_is_kernel) {
#if !TARGET_OS_SIMULATOR
					firehose_client_kernel_source_handle_event(fcj);
#endif
				} else {
					dispatch_source_merge_data(fcj->fc_io_source, 1);
					dispatch_source_merge_data(fcj->fc_mem_source, 1);
				}
			}
		});
	});
}

static void
firehose_snapshot_finish(void *ctxt)
{
	firehose_snapshot_t fs = ctxt;

	fs->handler(NULL, FIREHOSE_SNAPSHOT_EVENT_COMPLETE, NULL);
	server_config.fs_snapshot = NULL;

	dispatch_release(fs->fs_group);
	Block_release(fs->handler);
	free(fs);

	// resume the snapshot gate queue to maybe handle the next snapshot
	dispatch_resume(server_config.fs_snapshot_gate_queue);
}

static void
firehose_snapshot_gate(void *ctxt)
{
	// prevent other snapshots from running until done
	dispatch_suspend(server_config.fs_snapshot_gate_queue);
	dispatch_async_f(server_config.fs_io_drain_queue, ctxt,
			firehose_snapshot_start);
}

void
firehose_snapshot(firehose_snapshot_handler_t handler)
{
	firehose_snapshot_t snapshot = malloc(sizeof(struct firehose_snapshot_s));

	snapshot->handler = Block_copy(handler);
	snapshot->fs_group = dispatch_group_create();

	// keep the group entered until IO_START and MEM_START have been sent
	// See firehose_snapshot_start()
	dispatch_group_enter(snapshot->fs_group);
	dispatch_group_notify_f(snapshot->fs_group, server_config.fs_io_drain_queue,
			snapshot, firehose_snapshot_finish);

	dispatch_async_f(server_config.fs_snapshot_gate_queue, snapshot,
			firehose_snapshot_gate);
}

#pragma mark -
#pragma mark MiG handler routines

kern_return_t
firehose_server_register(mach_port_t server_port OS_UNUSED,
		mach_port_t mem_port, mach_vm_size_t mem_size,
		mach_port_t comm_recvp, mach_port_t comm_sendp,
		mach_port_t extra_info_port, mach_vm_size_t extra_info_size)
{
	mach_vm_address_t base_addr = 0;
	firehose_client_t fc = NULL;
	kern_return_t kr;
	struct firehose_client_connected_info_s fcci = {
		.fcci_version = FIREHOSE_CLIENT_CONNECTED_INFO_VERSION,
	};

	if (mem_size != sizeof(union firehose_buffer_u)) {
		return KERN_INVALID_VALUE;
	}

	/*
	 * Request a MACH_NOTIFY_NO_SENDERS notification for recvp. That should
	 * indicate the client going away.
	 */
	mach_port_t previous = MACH_PORT_NULL;
	kr = mach_port_request_notification(mach_task_self(), comm_recvp,
			MACH_NOTIFY_NO_SENDERS, 0, comm_recvp,
			MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
	DISPATCH_VERIFY_MIG(kr);
	if (dispatch_assume_zero(kr)) {
		return KERN_FAILURE;
	}
	dispatch_assert(previous == MACH_PORT_NULL);

	/* Map the memory handle into the server address space */
	kr = mach_vm_map(mach_task_self(), &base_addr, mem_size, 0,
			VM_FLAGS_ANYWHERE, mem_port, 0, FALSE,
			VM_PROT_READ, VM_PROT_READ, VM_INHERIT_NONE);
	DISPATCH_VERIFY_MIG(kr);
	if (dispatch_assume_zero(kr)) {
		return KERN_NO_SPACE;
	}

	if (extra_info_port && extra_info_size) {
		mach_vm_address_t addr = 0;
		kr = mach_vm_map(mach_task_self(), &addr, extra_info_size, 0,
				VM_FLAGS_ANYWHERE, extra_info_port, 0, FALSE,
				VM_PROT_READ, VM_PROT_READ, VM_INHERIT_NONE);
		if (dispatch_assume_zero(kr)) {
			mach_vm_deallocate(mach_task_self(), base_addr, mem_size);
			return KERN_NO_SPACE;
		}
		fcci.fcci_data = (void *)(uintptr_t)addr;
		fcci.fcci_size = (size_t)extra_info_size;
	}

	fc = firehose_client_create((firehose_buffer_t)base_addr,
			comm_recvp, comm_sendp);
	dispatch_async(server_config.fs_io_drain_queue, ^{
		firehose_client_resume(fc, &fcci);
		if (fcci.fcci_size) {
			vm_deallocate(mach_task_self(), (vm_address_t)fcci.fcci_data,
					fcci.fcci_size);
		}
	});

	if (extra_info_port) firehose_mach_port_send_release(extra_info_port);
	firehose_mach_port_send_release(mem_port);
	return KERN_SUCCESS;
}

kern_return_t
firehose_server_push_async(mach_port_t server_port OS_UNUSED,
		qos_class_t qos, boolean_t for_io, boolean_t expects_notifs)
{
	firehose_client_t fc = cur_client_info;
	pthread_priority_t pp = _pthread_qos_class_encode(qos, 0,
			_PTHREAD_PRIORITY_ENFORCE_FLAG);

	_dispatch_debug("FIREHOSE_PUSH_ASYNC (unique_pid %llx)",
			firehose_client_get_unique_pid(fc, NULL));
	if (!slowpath(fc->fc_memory_corrupted)) {
		if (expects_notifs && !fc->fc_use_notifs) {
			fc->fc_use_notifs = true;
		}
		firehose_client_push_async_merge(fc, pp, for_io);
	}
	return KERN_SUCCESS;
}

kern_return_t
firehose_server_push(mach_port_t server_port OS_UNUSED,
		mach_port_t reply_port, qos_class_t qos, boolean_t for_io,
		firehose_push_reply_t *push_reply OS_UNUSED)
{
	firehose_client_t fc = cur_client_info;
	dispatch_block_flags_t flags = DISPATCH_BLOCK_ENFORCE_QOS_CLASS;
	dispatch_block_t block;
	dispatch_queue_t q;

	_dispatch_debug("FIREHOSE_PUSH (unique_pid %llx)",
			firehose_client_get_unique_pid(fc, NULL));

	if (slowpath(fc->fc_memory_corrupted)) {
		firehose_client_mark_corrupted(fc, reply_port);
		return MIG_NO_REPLY;
	}

	if (for_io) {
		q = server_config.fs_io_drain_queue;
	} else {
		q = server_config.fs_mem_drain_queue;
	}

	block = dispatch_block_create_with_qos_class(flags, qos, 0, ^{
		firehose_client_drain(fc, reply_port,
				for_io ? FIREHOSE_DRAIN_FOR_IO : 0);
	});
	dispatch_async(q, block);
	_Block_release(block);
	return MIG_NO_REPLY;
}

static void
firehose_server_demux(firehose_client_t fc, mach_msg_header_t *msg_hdr)
{
	const size_t reply_size =
			sizeof(union __ReplyUnion__firehose_server_firehose_subsystem);

	cur_client_info = fc;
	firehose_mig_server(firehose_server, reply_size, msg_hdr);
}
