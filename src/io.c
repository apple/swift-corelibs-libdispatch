/*
 * Copyright (c) 2009-2013 Apple Inc. All rights reserved.
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

#if defined(__FreeBSD__)
#include <fcntl.h>
#define F_RDADVISE F_RDAHEAD
#endif

#ifndef DISPATCH_IO_DEBUG
#define DISPATCH_IO_DEBUG DISPATCH_DEBUG
#endif

#ifndef PAGE_SIZE
#if defined(_WIN32)
static DWORD
getpagesize(void)
{
	SYSTEM_INFO siInfo;
	GetSystemInfo(&siInfo);
	return siInfo.dwPageSize;
}
#endif
#define PAGE_SIZE ((size_t)getpagesize())
#endif

#if DISPATCH_DATA_IS_BRIDGED_TO_NSDATA
#define _dispatch_io_data_retain(x) _dispatch_objc_retain(x)
#define _dispatch_io_data_release(x) _dispatch_objc_release(x)
#else
#define _dispatch_io_data_retain(x) dispatch_retain(x)
#define _dispatch_io_data_release(x) dispatch_release(x)
#endif

typedef void (^dispatch_fd_entry_init_callback_t)(dispatch_fd_entry_t fd_entry);

DISPATCH_EXPORT DISPATCH_NOTHROW
void _dispatch_iocntl(uint32_t param, uint64_t value);

static dispatch_operation_t _dispatch_operation_create(
		dispatch_op_direction_t direction, dispatch_io_t channel, off_t offset,
		size_t length, dispatch_data_t data, dispatch_queue_t queue,
		dispatch_io_handler_t handler);
static void _dispatch_operation_enqueue(dispatch_operation_t op,
		dispatch_op_direction_t direction, dispatch_data_t data);
static dispatch_source_t _dispatch_operation_timer(dispatch_queue_t tq,
		dispatch_operation_t op);
static inline void _dispatch_fd_entry_retain(dispatch_fd_entry_t fd_entry);
static inline void _dispatch_fd_entry_release(dispatch_fd_entry_t fd_entry);
static void _dispatch_fd_entry_init_async(dispatch_fd_t fd,
		dispatch_fd_entry_init_callback_t completion_callback);
static dispatch_fd_entry_t _dispatch_fd_entry_create_with_fd(dispatch_fd_t fd,
		uintptr_t hash);
static dispatch_fd_entry_t _dispatch_fd_entry_create_with_path(
		dispatch_io_path_data_t path_data, dev_t dev, mode_t mode);
static int _dispatch_fd_entry_open(dispatch_fd_entry_t fd_entry,
		dispatch_io_t channel);
static void _dispatch_fd_entry_cleanup_operations(dispatch_fd_entry_t fd_entry,
		dispatch_io_t channel);
static void _dispatch_stream_init(dispatch_fd_entry_t fd_entry,
		dispatch_queue_t tq);
static void _dispatch_stream_dispose(dispatch_fd_entry_t fd_entry,
		dispatch_op_direction_t direction);
static void _dispatch_disk_init(dispatch_fd_entry_t fd_entry, dev_t dev);
static void _dispatch_stream_enqueue_operation(dispatch_stream_t stream,
		dispatch_operation_t operation, dispatch_data_t data);
static void _dispatch_disk_enqueue_operation(dispatch_disk_t dsk,
		dispatch_operation_t operation, dispatch_data_t data);
static void _dispatch_stream_cleanup_operations(dispatch_stream_t stream,
		dispatch_io_t channel);
static void _dispatch_disk_cleanup_inactive_operations(dispatch_disk_t disk,
		dispatch_io_t channel);
static void _dispatch_stream_source_handler(void *ctx);
static void _dispatch_stream_queue_handler(void *ctx);
static void _dispatch_stream_handler(void *ctx);
static void _dispatch_disk_handler(void *ctx);
static void _dispatch_disk_perform(void *ctxt);
static void _dispatch_operation_advise(dispatch_operation_t op,
		size_t chunk_size);
static int _dispatch_operation_perform(dispatch_operation_t op);
static void _dispatch_operation_deliver_data(dispatch_operation_t op,
		dispatch_op_flags_t flags);

// Macros to wrap syscalls which return -1 on error, and retry on EINTR
#define _dispatch_io_syscall_switch_noerr(_err, _syscall, ...) do { \
		switch (((_err) = (((_syscall) == -1) ? errno : 0))) { \
		case EINTR: continue; \
		__VA_ARGS__ \
		} \
		break; \
	} while (1)
#define _dispatch_io_syscall_switch(__err, __syscall, ...) do { \
		_dispatch_io_syscall_switch_noerr(__err, __syscall, \
		case 0: break; \
		__VA_ARGS__ \
		); \
	} while (0)
#define _dispatch_io_syscall(__syscall) do { int __err; \
		_dispatch_io_syscall_switch(__err, __syscall); \
	} while (0)

enum {
	DISPATCH_OP_COMPLETE = 1,
	DISPATCH_OP_DELIVER,
	DISPATCH_OP_DELIVER_AND_COMPLETE,
	DISPATCH_OP_COMPLETE_RESUME,
	DISPATCH_OP_RESUME,
	DISPATCH_OP_ERR,
	DISPATCH_OP_FD_ERR,
};

#define _dispatch_io_Block_copy(x) \
		((__typeof__(x))_dispatch_Block_copy((dispatch_block_t)(x)))

#pragma mark -
#pragma mark dispatch_io_debug

#if DISPATCH_IO_DEBUG
#if !DISPATCH_DEBUG
#define _dispatch_io_log(x, ...) do { \
			_dispatch_log("%llu\t%p\t" x, _dispatch_uptime(), \
			(void *)_dispatch_thread_self(), ##__VA_ARGS__); \
		} while (0)
#ifdef _dispatch_object_debug
#undef _dispatch_object_debug
#define _dispatch_object_debug dispatch_debug
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#else
#define _dispatch_io_log(x, ...) _dispatch_debug(x, ##__VA_ARGS__)
#endif // DISPATCH_DEBUG
#else
#define _dispatch_io_log(x, ...)
#endif // DISPATCH_IO_DEBUG

#define _dispatch_fd_debug(msg, fd, ...) \
		_dispatch_io_log("fd[0x%" PRIx64 "]: " msg, fd, ##__VA_ARGS__)
#define _dispatch_op_debug(msg, op, ...) \
		_dispatch_io_log("op[%p]: " msg, op, ##__VA_ARGS__)
#define _dispatch_channel_debug(msg, channel, ...) \
		_dispatch_io_log("channel[%p]: " msg, channel, ##__VA_ARGS__)
#define _dispatch_fd_entry_debug(msg, fd_entry, ...) \
		_dispatch_io_log("fd_entry[%p]: " msg, fd_entry, ##__VA_ARGS__)
#define _dispatch_disk_debug(msg, disk, ...) \
		_dispatch_io_log("disk[%p]: " msg, disk, ##__VA_ARGS__)

#pragma mark -
#pragma mark dispatch_io_hashtables

LIST_HEAD(dispatch_disk_head_s, dispatch_disk_s);
LIST_HEAD(dispatch_fd_entry_head_s, dispatch_fd_entry_s);

// Global hashtable of dev_t -> disk_s mappings
DISPATCH_STATIC_GLOBAL(struct dispatch_disk_head_s _dispatch_io_devs[DIO_HASH_SIZE]);
DISPATCH_STATIC_GLOBAL(dispatch_queue_t _dispatch_io_devs_lockq);

// Global hashtable of fd -> fd_entry_s mappings
DISPATCH_STATIC_GLOBAL(struct dispatch_fd_entry_head_s _dispatch_io_fds[DIO_HASH_SIZE]);
DISPATCH_STATIC_GLOBAL(dispatch_queue_t _dispatch_io_fds_lockq);

DISPATCH_STATIC_GLOBAL(dispatch_once_t _dispatch_io_init_pred);

static char const * const _dispatch_io_key = "io";

static void
_dispatch_io_queues_init(void *context DISPATCH_UNUSED)
{
	_dispatch_io_fds_lockq = dispatch_queue_create(
			"com.apple.libdispatch-io.fd_lockq", NULL);
	_dispatch_io_devs_lockq = dispatch_queue_create(
			"com.apple.libdispatch-io.dev_lockq", NULL);
}

#pragma mark -
#pragma mark dispatch_io_defaults

enum {
	DISPATCH_IOCNTL_CHUNK_PAGES = 1,
	DISPATCH_IOCNTL_LOW_WATER_CHUNKS,
	DISPATCH_IOCNTL_INITIAL_DELIVERY,
	DISPATCH_IOCNTL_MAX_PENDING_IO_REQS,
};

extern struct dispatch_io_defaults_s {
	size_t chunk_size, low_water_chunks, max_pending_io_reqs;
	bool initial_delivery;
} dispatch_io_defaults;

DISPATCH_GLOBAL_INIT(struct dispatch_io_defaults_s dispatch_io_defaults, {
	.chunk_size = DIO_MAX_CHUNK_SIZE,
	.low_water_chunks = DIO_DEFAULT_LOW_WATER_CHUNKS,
	.max_pending_io_reqs = DIO_MAX_PENDING_IO_REQS,
});

#define _dispatch_iocntl_set_default(p, v) do { \
		dispatch_io_defaults.p = (__typeof__(dispatch_io_defaults.p))(v); \
	} while (0)

void
_dispatch_iocntl(uint32_t param, uint64_t value)
{
	switch (param) {
	case DISPATCH_IOCNTL_CHUNK_PAGES:
		_dispatch_iocntl_set_default(chunk_size, value * PAGE_SIZE);
		break;
	case DISPATCH_IOCNTL_LOW_WATER_CHUNKS:
		_dispatch_iocntl_set_default(low_water_chunks, value);
		break;
	case DISPATCH_IOCNTL_INITIAL_DELIVERY:
		_dispatch_iocntl_set_default(initial_delivery, value);
		break;
	case DISPATCH_IOCNTL_MAX_PENDING_IO_REQS:
		_dispatch_iocntl_set_default(max_pending_io_reqs, value);
		break;
	}
}

#pragma mark -
#pragma mark dispatch_io_t

static dispatch_io_t
_dispatch_io_create(dispatch_io_type_t type)
{
	dispatch_io_t channel = _dispatch_object_alloc(DISPATCH_VTABLE(io),
			sizeof(struct dispatch_io_s));
	channel->do_next = DISPATCH_OBJECT_LISTLESS;
	channel->do_targetq = _dispatch_get_default_queue(true);
	channel->params.type = type;
	channel->params.high = SIZE_MAX;
	channel->params.low = dispatch_io_defaults.low_water_chunks *
			dispatch_io_defaults.chunk_size;
	channel->queue = dispatch_queue_create("com.apple.libdispatch-io.channelq",
			NULL);
	return channel;
}

static void
_dispatch_io_init(dispatch_io_t channel, dispatch_fd_entry_t fd_entry,
		dispatch_queue_t queue, int err, void (^cleanup_handler)(int))
{
	// Enqueue the cleanup handler on the suspended close queue
	if (cleanup_handler) {
		_dispatch_retain(queue);
		dispatch_async(!err ? fd_entry->close_queue : channel->queue, ^{
			dispatch_async(queue, ^{
				_dispatch_channel_debug("cleanup handler invoke: err %d",
						channel, err);
				cleanup_handler(err);
			});
			_dispatch_release(queue);
		});
	}
	if (fd_entry) {
		channel->fd_entry = fd_entry;
		dispatch_retain(fd_entry->barrier_queue);
		dispatch_retain(fd_entry->barrier_group);
		channel->barrier_queue = fd_entry->barrier_queue;
		channel->barrier_group = fd_entry->barrier_group;
	} else {
		// Still need to create a barrier queue, since all operations go
		// through it
		channel->barrier_queue = dispatch_queue_create(
				"com.apple.libdispatch-io.barrierq", NULL);
		channel->barrier_group = dispatch_group_create();
	}
}

void
_dispatch_io_dispose(dispatch_io_t channel, DISPATCH_UNUSED bool *allow_free)
{
	_dispatch_object_debug(channel, "%s", __func__);
	if (channel->fd_entry &&
			!(channel->atomic_flags & (DIO_CLOSED|DIO_STOPPED))) {
		if (channel->fd_entry->path_data) {
			// This modification is safe since path_data->channel is checked
			// only on close_queue (which is still suspended at this point)
			channel->fd_entry->path_data->channel = NULL;
		}
		// Cleanup handlers will only run when all channels related to this
		// fd are complete
		_dispatch_fd_entry_release(channel->fd_entry);
	}
	if (channel->queue) {
		dispatch_release(channel->queue);
	}
	if (channel->barrier_queue) {
		dispatch_release(channel->barrier_queue);
	}
	if (channel->barrier_group) {
		dispatch_release(channel->barrier_group);
	}
}

static int
_dispatch_io_validate_type(dispatch_io_t channel, mode_t mode)
{
	int err = 0;
	if (S_ISDIR(mode)) {
		err = EISDIR;
	} else if (channel->params.type == DISPATCH_IO_RANDOM &&
			(S_ISFIFO(mode) || S_ISSOCK(mode))) {
		err = ESPIPE;
	}
	return err;
}

static int
_dispatch_io_get_error(dispatch_operation_t op, dispatch_io_t channel,
		bool ignore_closed)
{
	// On _any_ queue
	int err;
	if (op) {
		channel = op->channel;
	}
	if (channel->atomic_flags & (DIO_CLOSED|DIO_STOPPED)) {
		if (!ignore_closed || channel->atomic_flags & DIO_STOPPED) {
			err = ECANCELED;
		} else {
			err = 0;
		}
	} else {
		err = op ? op->fd_entry->err : channel->err;
	}
	return err;
}

#pragma mark -
#pragma mark dispatch_io_channels

dispatch_io_t
dispatch_io_create(dispatch_io_type_t type, dispatch_fd_t fd,
		dispatch_queue_t queue, void (^cleanup_handler)(int))
{
	if (type != DISPATCH_IO_STREAM && type != DISPATCH_IO_RANDOM) {
		return DISPATCH_BAD_INPUT;
	}
	dispatch_io_t channel = _dispatch_io_create(type);
	channel->fd = fd;
	_dispatch_channel_debug("create", channel);
	channel->fd_actual = fd;
	dispatch_suspend(channel->queue);
	_dispatch_retain(queue);
	_dispatch_retain(channel);
	_dispatch_fd_entry_init_async(fd, ^(dispatch_fd_entry_t fd_entry) {
		// On barrier queue
		int err = fd_entry->err;
		if (!err) {
			err = _dispatch_io_validate_type(channel, fd_entry->stat.mode);
		}
		if (!err && type == DISPATCH_IO_RANDOM) {
#if defined(_WIN32)
			LARGE_INTEGER liPosition;
			LARGE_INTEGER liDistance = {};
			if (!SetFilePointerEx((HANDLE)fd_entry->fd, liDistance, &liPosition, FILE_CURRENT)) {
				err = (int)GetLastError();
			} else {
				err = 0;
				channel->f_ptr = liPosition.QuadPart;
			}
#else
			off_t f_ptr;
			_dispatch_io_syscall_switch_noerr(err,
				f_ptr = lseek(fd_entry->fd, 0, SEEK_CUR),
				case 0: channel->f_ptr = f_ptr; break;
				default: (void)dispatch_assume_zero(err); break;
			);
#endif
		}
		channel->err = err;
		_dispatch_fd_entry_retain(fd_entry);
		_dispatch_io_init(channel, fd_entry, queue, err, cleanup_handler);
		dispatch_resume(channel->queue);
		_dispatch_object_debug(channel, "%s", __func__);
		_dispatch_release(channel);
		_dispatch_release(queue);
	});
	_dispatch_object_debug(channel, "%s", __func__);
	return channel;
}

dispatch_io_t
dispatch_io_create_f(dispatch_io_type_t type, dispatch_fd_t fd,
		dispatch_queue_t queue, void *context,
		void (*cleanup_handler)(void *context, int error))
{
	return dispatch_io_create(type, fd, queue, !cleanup_handler ? NULL :
			^(int error){ cleanup_handler(context, error); });
}

#if defined(_WIN32)
#define _is_separator(ch) ((ch) == '/' || (ch) == '\\')
#else
#define _is_separator(ch) ((ch) == '/')
#endif

dispatch_io_t
dispatch_io_create_with_path(dispatch_io_type_t type, const char *path,
		int oflag, mode_t mode, dispatch_queue_t queue,
		void (^cleanup_handler)(int error))
{
	if (type != DISPATCH_IO_STREAM && type != DISPATCH_IO_RANDOM) {
		return DISPATCH_BAD_INPUT;
	}
#if defined(_WIN32)
	if (PathIsRelativeA(path)) {
		return DISPATCH_BAD_INPUT;
	}
#else
	if (!_is_separator(*path)) {
		return DISPATCH_BAD_INPUT;
	}
#endif
	size_t pathlen = strlen(path);
	dispatch_io_path_data_t path_data = malloc(sizeof(*path_data) + pathlen+1);
	if (!path_data) {
		return DISPATCH_OUT_OF_MEMORY;
	}
	dispatch_io_t channel = _dispatch_io_create(type);
	channel->fd = -1;
	_dispatch_channel_debug("create with path %s", channel, path);
	channel->fd_actual = -1;
	path_data->channel = channel;
	path_data->oflag = oflag;
	path_data->mode = mode;
	path_data->pathlen = pathlen;
	memcpy(path_data->path, path, pathlen + 1);
	_dispatch_retain(queue);
	_dispatch_retain(channel);
	dispatch_async(channel->queue, ^{
		int err = 0;
		struct stat st;
		_dispatch_io_syscall_switch_noerr(err,
#if defined(_WIN32)
			stat(path_data->path, &st),
#else
			(path_data->oflag & O_NOFOLLOW) == O_NOFOLLOW
#if __APPLE__
					|| (path_data->oflag & O_SYMLINK) == O_SYMLINK
#endif
					? lstat(path_data->path, &st) : stat(path_data->path, &st),
#endif
			case 0:
				err = _dispatch_io_validate_type(channel, st.st_mode);
				break;
			default:
				if ((path_data->oflag & O_CREAT) &&
						!_is_separator(*(path_data->path + path_data->pathlen - 1))) {
					// Check parent directory
					char *c = NULL;
					for (ssize_t i = (ssize_t)path_data->pathlen - 1; i >= 0; i--) {
						if (_is_separator(path_data->path[i])) {
							c = &path_data->path[i];
							break;
						}
					}
					dispatch_assert(c);
					*c = 0;
					int perr;
					_dispatch_io_syscall_switch_noerr(perr,
						stat(path_data->path, &st),
						case 0:
							// Since the parent directory exists, open() will
							// create a regular file after the fd_entry has
							// been filled in
							st.st_mode = S_IFREG;
							err = 0;
							break;
					);
#if defined(_WIN32)
					*c = '\\';
#else
					*c = '/';
#endif
				}
				break;
		);
		channel->err = err;
		if (err) {
			free(path_data);
			_dispatch_io_init(channel, NULL, queue, err, cleanup_handler);
			_dispatch_release(channel);
			_dispatch_release(queue);
			return;
		}
		dispatch_suspend(channel->queue);
		dispatch_once_f(&_dispatch_io_init_pred, NULL,
				_dispatch_io_queues_init);
		dispatch_async(_dispatch_io_devs_lockq, ^{
			dispatch_fd_entry_t fd_entry = _dispatch_fd_entry_create_with_path(
					path_data, st.st_dev, st.st_mode);
			_dispatch_io_init(channel, fd_entry, queue, 0, cleanup_handler);
			dispatch_resume(channel->queue);
			_dispatch_object_debug(channel, "%s", __func__);
			_dispatch_release(channel);
			_dispatch_release(queue);
		});
	});
	_dispatch_object_debug(channel, "%s", __func__);
	return channel;
}

dispatch_io_t
dispatch_io_create_with_path_f(dispatch_io_type_t type, const char *path,
		int oflag, mode_t mode, dispatch_queue_t queue, void *context,
		void (*cleanup_handler)(void *context, int error))
{
	return dispatch_io_create_with_path(type, path, oflag, mode, queue,
			!cleanup_handler ? NULL :
			^(int error){ cleanup_handler(context, error); });
}

dispatch_io_t
dispatch_io_create_with_io(dispatch_io_type_t type, dispatch_io_t in_channel,
		dispatch_queue_t queue, void (^cleanup_handler)(int error))
{
	if (type != DISPATCH_IO_STREAM && type != DISPATCH_IO_RANDOM) {
		return DISPATCH_BAD_INPUT;
	}
	dispatch_io_t channel = _dispatch_io_create(type);
	_dispatch_channel_debug("create with channel %p", channel, in_channel);
	dispatch_suspend(channel->queue);
	_dispatch_retain(queue);
	_dispatch_retain(channel);
	_dispatch_retain(in_channel);
	dispatch_async(in_channel->queue, ^{
		int err0 = _dispatch_io_get_error(NULL, in_channel, false);
		if (err0) {
			channel->err = err0;
			_dispatch_io_init(channel, NULL, queue, err0, cleanup_handler);
			dispatch_resume(channel->queue);
			_dispatch_release(channel);
			_dispatch_release(in_channel);
			_dispatch_release(queue);
			return;
		}
		dispatch_async(in_channel->barrier_queue, ^{
			int err = _dispatch_io_get_error(NULL, in_channel, false);
			// If there is no error, the fd_entry for the in_channel is valid.
			// Since we are running on in_channel's queue, the fd_entry has been
			// fully resolved and will stay valid for the duration of this block
			if (!err) {
				err = in_channel->err;
				if (!err) {
					err = in_channel->fd_entry->err;
				}
			}
			if (!err) {
				err = _dispatch_io_validate_type(channel,
						in_channel->fd_entry->stat.mode);
			}
			if (!err && type == DISPATCH_IO_RANDOM && in_channel->fd != -1) {
#if defined(_WIN32)
				LARGE_INTEGER liPosition;
				LARGE_INTEGER liDistance = {};
				if (!SetFilePointerEx((HANDLE)in_channel->fd_entry->fd, liDistance, &liPosition, FILE_CURRENT)) {
					err = (int)GetLastError();
				} else {
					err = 0;
					channel->f_ptr = liPosition.QuadPart;
				}
#else
				off_t f_ptr;
				_dispatch_io_syscall_switch_noerr(err,
					f_ptr = lseek(in_channel->fd_entry->fd, 0, SEEK_CUR),
					case 0: channel->f_ptr = f_ptr; break;
					default: (void)dispatch_assume_zero(err); break;
				);
#endif
			}
			channel->err = err;
			if (err) {
				_dispatch_io_init(channel, NULL, queue, err, cleanup_handler);
				dispatch_resume(channel->queue);
				_dispatch_release(channel);
				_dispatch_release(in_channel);
				_dispatch_release(queue);
				return;
			}
			if (in_channel->fd == -1) {
				// in_channel was created from path
				channel->fd = -1;
				channel->fd_actual = -1;
				mode_t mode = in_channel->fd_entry->stat.mode;
				dev_t dev = in_channel->fd_entry->stat.dev;
				size_t path_data_len = sizeof(struct dispatch_io_path_data_s) +
						in_channel->fd_entry->path_data->pathlen + 1;
				dispatch_io_path_data_t path_data = malloc(path_data_len);
				memcpy(path_data, in_channel->fd_entry->path_data,
						path_data_len);
				path_data->channel = channel;
				// lockq_io_devs is known to already exist
				dispatch_async(_dispatch_io_devs_lockq, ^{
					dispatch_fd_entry_t fd_entry;
					fd_entry = _dispatch_fd_entry_create_with_path(path_data,
							dev, mode);
					_dispatch_io_init(channel, fd_entry, queue, 0,
							cleanup_handler);
					dispatch_resume(channel->queue);
					_dispatch_release(channel);
					_dispatch_release(queue);
				});
			} else {
				dispatch_fd_entry_t fd_entry = in_channel->fd_entry;
				channel->fd = in_channel->fd;
				channel->fd_actual = in_channel->fd_actual;
				_dispatch_fd_entry_retain(fd_entry);
				_dispatch_io_init(channel, fd_entry, queue, 0, cleanup_handler);
				dispatch_resume(channel->queue);
				_dispatch_release(channel);
				_dispatch_release(queue);
			}
			_dispatch_release(in_channel);
			_dispatch_object_debug(channel, "%s", __func__);
		});
	});
	_dispatch_object_debug(channel, "%s", __func__);
	return channel;
}

dispatch_io_t
dispatch_io_create_with_io_f(dispatch_io_type_t type, dispatch_io_t in_channel,
		dispatch_queue_t queue, void *context,
		void (*cleanup_handler)(void *context, int error))
{
	return dispatch_io_create_with_io(type, in_channel, queue,
			!cleanup_handler ? NULL :
			^(int error){ cleanup_handler(context, error); });
}

#pragma mark -
#pragma mark dispatch_io_accessors

void
dispatch_io_set_high_water(dispatch_io_t channel, size_t high_water)
{
	_dispatch_retain(channel);
	dispatch_async(channel->queue, ^{
		_dispatch_channel_debug("set high water: %zu", channel, high_water);
		if (channel->params.low > high_water) {
			channel->params.low = high_water;
		}
		channel->params.high = high_water ? high_water : 1;
		_dispatch_release(channel);
	});
}

void
dispatch_io_set_low_water(dispatch_io_t channel, size_t low_water)
{
	_dispatch_retain(channel);
	dispatch_async(channel->queue, ^{
		_dispatch_channel_debug("set low water: %zu", channel, low_water);
		if (channel->params.high < low_water) {
			channel->params.high = low_water ? low_water : 1;
		}
		channel->params.low = low_water;
		_dispatch_release(channel);
	});
}

void
dispatch_io_set_interval(dispatch_io_t channel, uint64_t interval,
		unsigned long flags)
{
	_dispatch_retain(channel);
	dispatch_async(channel->queue, ^{
		_dispatch_channel_debug("set interval: %llu", channel,
		  (unsigned long long)interval);
		channel->params.interval = interval < INT64_MAX ? interval : INT64_MAX;
		channel->params.interval_flags = flags;
		_dispatch_release(channel);
	});
}

void
_dispatch_io_set_target_queue(dispatch_io_t channel, dispatch_queue_t dq)
{
	_dispatch_retain(dq);
	_dispatch_retain(channel);
	dispatch_async(channel->queue, ^{
		dispatch_queue_t prev_dq = channel->do_targetq;
		channel->do_targetq = dq;
		_dispatch_release(prev_dq);
		_dispatch_object_debug(channel, "%s", __func__);
		_dispatch_release(channel);
	});
}

dispatch_fd_t
dispatch_io_get_descriptor(dispatch_io_t channel)
{
	if (channel->atomic_flags & (DIO_CLOSED|DIO_STOPPED)) {
		return -1;
	}
	dispatch_fd_t fd = channel->fd_actual;
	if (fd == -1 && !_dispatch_io_get_error(NULL, channel, false)) {
		dispatch_thread_context_t ctxt =
				_dispatch_thread_context_find(_dispatch_io_key);
		if (ctxt && ctxt->dtc_io_in_barrier == channel) {
			(void)_dispatch_fd_entry_open(channel->fd_entry, channel);
		}
	}
	return channel->fd_actual;
}

#pragma mark -
#pragma mark dispatch_io_operations

static void
_dispatch_io_stop(dispatch_io_t channel)
{
	_dispatch_channel_debug("stop", channel);
	(void)os_atomic_or2o(channel, atomic_flags, DIO_STOPPED, relaxed);
	_dispatch_retain(channel);
	dispatch_async(channel->queue, ^{
		dispatch_async(channel->barrier_queue, ^{
			_dispatch_object_debug(channel, "%s", __func__);
			dispatch_fd_entry_t fd_entry = channel->fd_entry;
			if (fd_entry) {
				_dispatch_channel_debug("stop cleanup", channel);
				_dispatch_fd_entry_cleanup_operations(fd_entry, channel);
				if (!(channel->atomic_flags & DIO_CLOSED)) {
					if (fd_entry->path_data) {
						fd_entry->path_data->channel = NULL;
					}
					channel->fd_entry = NULL;
					_dispatch_fd_entry_release(fd_entry);
				}
			} else if (channel->fd != -1) {
				// Stop after close, need to check if fd_entry still exists
				_dispatch_retain(channel);
				dispatch_async(_dispatch_io_fds_lockq, ^{
					_dispatch_object_debug(channel, "%s", __func__);
					_dispatch_channel_debug("stop cleanup after close",
							channel);
					dispatch_fd_entry_t fdi;
					uintptr_t hash = DIO_HASH(channel->fd);
					LIST_FOREACH(fdi, &_dispatch_io_fds[hash], fd_list) {
						if (fdi->fd == channel->fd) {
							_dispatch_fd_entry_cleanup_operations(fdi, channel);
							break;
						}
					}
					_dispatch_release(channel);
				});
			}
			_dispatch_release(channel);
		});
	});
}

void
dispatch_io_close(dispatch_io_t channel, unsigned long flags)
{
	if (flags & DISPATCH_IO_STOP) {
		// Don't stop an already stopped channel
		if (channel->atomic_flags & DIO_STOPPED) {
			return;
		}
		return _dispatch_io_stop(channel);
	}
	// Don't close an already closed or stopped channel
	if (channel->atomic_flags & (DIO_CLOSED|DIO_STOPPED)) {
		return;
	}
	_dispatch_retain(channel);
	dispatch_async(channel->queue, ^{
		dispatch_async(channel->barrier_queue, ^{
			_dispatch_object_debug(channel, "%s", __func__);
			_dispatch_channel_debug("close", channel);
			if (!(channel->atomic_flags & (DIO_CLOSED|DIO_STOPPED))) {
				(void)os_atomic_or2o(channel, atomic_flags, DIO_CLOSED,
						relaxed);
				dispatch_fd_entry_t fd_entry = channel->fd_entry;
				if (fd_entry) {
					if (fd_entry->path_data) {
						fd_entry->path_data->channel = NULL;
					}
					channel->fd_entry = NULL;
					_dispatch_fd_entry_release(fd_entry);
				}
			}
			_dispatch_release(channel);
		});
	});
}

void
dispatch_io_barrier(dispatch_io_t channel, dispatch_block_t barrier)
{
	_dispatch_retain(channel);
	dispatch_async(channel->queue, ^{
		dispatch_queue_t io_q = channel->do_targetq;
		dispatch_queue_t barrier_queue = channel->barrier_queue;
		dispatch_group_t barrier_group = channel->barrier_group;
		dispatch_async(barrier_queue, ^{
			dispatch_suspend(barrier_queue);
			dispatch_group_notify(barrier_group, io_q, ^{
				dispatch_thread_context_s io_ctxt = {
					.dtc_key = _dispatch_io_key,
					.dtc_io_in_barrier = channel,
				};

				_dispatch_object_debug(channel, "%s", __func__);
				_dispatch_thread_context_push(&io_ctxt);
				barrier();
				_dispatch_thread_context_pop(&io_ctxt);
				dispatch_resume(barrier_queue);
				_dispatch_release(channel);
			});
		});
	});
}

void
dispatch_io_barrier_f(dispatch_io_t channel, void *context,
		dispatch_function_t barrier)
{
	return dispatch_io_barrier(channel, ^{ barrier(context); });
}

void
dispatch_io_read(dispatch_io_t channel, off_t offset, size_t length,
		dispatch_queue_t queue, dispatch_io_handler_t handler)
{
	_dispatch_retain(channel);
	_dispatch_retain(queue);
	dispatch_async(channel->queue, ^{
		dispatch_operation_t op;
		op = _dispatch_operation_create(DOP_DIR_READ, channel, offset,
				length, dispatch_data_empty, queue, handler);
		if (op) {
			dispatch_queue_t barrier_q = channel->barrier_queue;
			dispatch_async(barrier_q, ^{
				_dispatch_operation_enqueue(op, DOP_DIR_READ,
						dispatch_data_empty);
			});
		}
		_dispatch_release(channel);
		_dispatch_release(queue);
	});
}

void
dispatch_io_read_f(dispatch_io_t channel, off_t offset, size_t length,
		dispatch_queue_t queue, void *context,
		dispatch_io_handler_function_t handler)
{
	return dispatch_io_read(channel, offset, length, queue,
			^(bool done, dispatch_data_t d, int error){
		handler(context, done, d, error);
	});
}

void
dispatch_io_write(dispatch_io_t channel, off_t offset, dispatch_data_t data,
		dispatch_queue_t queue, dispatch_io_handler_t handler)
{
	_dispatch_io_data_retain(data);
	_dispatch_retain(channel);
	_dispatch_retain(queue);
	dispatch_async(channel->queue, ^{
		dispatch_operation_t op;
		op = _dispatch_operation_create(DOP_DIR_WRITE, channel, offset,
				dispatch_data_get_size(data), data, queue, handler);
		if (op) {
			dispatch_queue_t barrier_q = channel->barrier_queue;
			dispatch_async(barrier_q, ^{
				_dispatch_operation_enqueue(op, DOP_DIR_WRITE, data);
				_dispatch_io_data_release(data);
			});
		} else {
			_dispatch_io_data_release(data);
		}
		_dispatch_release(channel);
		_dispatch_release(queue);
	});
}

void
dispatch_io_write_f(dispatch_io_t channel, off_t offset, dispatch_data_t data,
		dispatch_queue_t queue, void *context,
		dispatch_io_handler_function_t handler)
{
	return dispatch_io_write(channel, offset, data, queue,
			^(bool done, dispatch_data_t d, int error){
		handler(context, done, d, error);
	});
}

void
dispatch_read(dispatch_fd_t fd, size_t length, dispatch_queue_t queue,
		void (^handler)(dispatch_data_t, int))
{
	_dispatch_retain(queue);
	_dispatch_fd_entry_init_async(fd, ^(dispatch_fd_entry_t fd_entry) {
		// On barrier queue
		if (fd_entry->err) {
			int err = fd_entry->err;
			dispatch_async(queue, ^{
				_dispatch_fd_debug("convenience handler invoke", fd);
				handler(dispatch_data_empty, err);
			});
			_dispatch_release(queue);
			return;
		}
		// Safe to access fd_entry on barrier queue
		dispatch_io_t channel = fd_entry->convenience_channel;
		if (!channel) {
			channel = _dispatch_io_create(DISPATCH_IO_STREAM);
			channel->fd = fd;
			channel->fd_actual = fd;
			channel->fd_entry = fd_entry;
			dispatch_retain(fd_entry->barrier_queue);
			dispatch_retain(fd_entry->barrier_group);
			channel->barrier_queue = fd_entry->barrier_queue;
			channel->barrier_group = fd_entry->barrier_group;
			fd_entry->convenience_channel = channel;
		}
		__block dispatch_data_t deliver_data = dispatch_data_empty;
		__block int err = 0;
		dispatch_async(fd_entry->close_queue, ^{
			dispatch_async(queue, ^{
				_dispatch_fd_debug("convenience handler invoke", fd);
				handler(deliver_data, err);
				_dispatch_io_data_release(deliver_data);
			});
			_dispatch_release(queue);
		});
		dispatch_operation_t op =
			_dispatch_operation_create(DOP_DIR_READ, channel, 0,
					length, dispatch_data_empty,
					_dispatch_get_default_queue(false),
					^(bool done, dispatch_data_t data, int error) {
				if (data) {
					data = dispatch_data_create_concat(deliver_data, data);
					_dispatch_io_data_release(deliver_data);
					deliver_data = data;
				}
				if (done) {
					err = error;
				}
			});
		if (op) {
			_dispatch_operation_enqueue(op, DOP_DIR_READ, dispatch_data_empty);
		}
	});
}

void
dispatch_read_f(dispatch_fd_t fd, size_t length, dispatch_queue_t queue,
		void *context, void (*handler)(void *, dispatch_data_t, int))
{
	return dispatch_read(fd, length, queue, ^(dispatch_data_t d, int error){
		handler(context, d, error);
	});
}

void
dispatch_write(dispatch_fd_t fd, dispatch_data_t data, dispatch_queue_t queue,
		void (^handler)(dispatch_data_t, int))
{
	_dispatch_io_data_retain(data);
	_dispatch_retain(queue);
	_dispatch_fd_entry_init_async(fd, ^(dispatch_fd_entry_t fd_entry) {
		// On barrier queue
		if (fd_entry->err) {
			int err = fd_entry->err;
			dispatch_async(queue, ^{
				_dispatch_fd_debug("convenience handler invoke", fd);
				handler(NULL, err);
			});
			_dispatch_release(queue);
			return;
		}
		// Safe to access fd_entry on barrier queue
		dispatch_io_t channel = fd_entry->convenience_channel;
		if (!channel) {
			channel = _dispatch_io_create(DISPATCH_IO_STREAM);
			channel->fd = fd;
			channel->fd_actual = fd;
			channel->fd_entry = fd_entry;
			dispatch_retain(fd_entry->barrier_queue);
			dispatch_retain(fd_entry->barrier_group);
			channel->barrier_queue = fd_entry->barrier_queue;
			channel->barrier_group = fd_entry->barrier_group;
			fd_entry->convenience_channel = channel;
		}
		__block dispatch_data_t deliver_data = NULL;
		__block int err = 0;
		dispatch_async(fd_entry->close_queue, ^{
			dispatch_async(queue, ^{
				_dispatch_fd_debug("convenience handler invoke", fd);
				handler(deliver_data, err);
				if (deliver_data) {
					_dispatch_io_data_release(deliver_data);
				}
			});
			_dispatch_release(queue);
		});
		dispatch_operation_t op =
			_dispatch_operation_create(DOP_DIR_WRITE, channel, 0,
					dispatch_data_get_size(data), data,
					_dispatch_get_default_queue(false),
					^(bool done, dispatch_data_t d, int error) {
				if (done) {
					if (d) {
						_dispatch_io_data_retain(d);
						deliver_data = d;
					}
					err = error;
				}
			});
		if (op) {
			_dispatch_operation_enqueue(op, DOP_DIR_WRITE, data);
		}
		_dispatch_io_data_release(data);
	});
}

void
dispatch_write_f(dispatch_fd_t fd, dispatch_data_t data, dispatch_queue_t queue,
		void *context, void (*handler)(void *, dispatch_data_t, int))
{
	return dispatch_write(fd, data, queue, ^(dispatch_data_t d, int error){
		handler(context, d, error);
	});
}

#pragma mark -
#pragma mark dispatch_operation_t

static dispatch_operation_t
_dispatch_operation_create(dispatch_op_direction_t direction,
		dispatch_io_t channel, off_t offset, size_t length,
		dispatch_data_t data, dispatch_queue_t queue,
		dispatch_io_handler_t handler)
{
	// On channel queue
	dispatch_assert(direction < DOP_DIR_MAX);
	// Safe to call _dispatch_io_get_error() with channel->fd_entry since
	// that can only be NULL if atomic_flags are set rdar://problem/8362514
	int err = _dispatch_io_get_error(NULL, channel, false);
	if (err || !length) {
		_dispatch_io_data_retain(data);
		_dispatch_retain(queue);
		_dispatch_retain(channel);
		dispatch_async(channel->barrier_queue, ^{
			dispatch_async(queue, ^{
				dispatch_data_t d = data;
				if (direction == DOP_DIR_READ && err) {
					d = NULL;
				} else if (direction == DOP_DIR_WRITE && !err) {
					d = NULL;
				}
				_dispatch_channel_debug("IO handler invoke: err %d", channel,
						err);
				handler(true, d, err);
				_dispatch_release(channel);
				_dispatch_io_data_release(data);
			});
			_dispatch_release(queue);
		});
		return NULL;
	}
	dispatch_operation_t op = _dispatch_object_alloc(DISPATCH_VTABLE(operation),
			sizeof(struct dispatch_operation_s));
	_dispatch_channel_debug("operation create: %p", channel, op);
	op->do_next = DISPATCH_OBJECT_LISTLESS;
	op->do_xref_cnt = -1; // operation object is not exposed externally
	op->op_q = dispatch_queue_create_with_target("com.apple.libdispatch-io.opq",
			NULL, queue);
	op->active = false;
	op->direction = direction;
	op->offset = offset + channel->f_ptr;
	op->length = length;
	op->handler = _dispatch_io_Block_copy(handler);
	_dispatch_retain(channel);
	op->channel = channel;
	op->params = channel->params;
	// Take a snapshot of the priority of the channel queue. The actual I/O
	// for this operation will be performed at this priority
	dispatch_queue_t targetq = op->channel->do_targetq;
	while (targetq->do_targetq) {
		targetq = targetq->do_targetq;
	}
	op->do_targetq = targetq;
	_dispatch_object_debug(op, "%s", __func__);
	return op;
}

void
_dispatch_operation_dispose(dispatch_operation_t op,
		DISPATCH_UNUSED bool *allow_free)
{
	_dispatch_object_debug(op, "%s", __func__);
	_dispatch_op_debug("dispose", op);
	// Deliver the data if there's any
	if (op->fd_entry) {
		_dispatch_operation_deliver_data(op, DOP_DONE);
		dispatch_group_leave(op->fd_entry->barrier_group);
		_dispatch_fd_entry_release(op->fd_entry);
	}
	if (op->channel) {
		_dispatch_release(op->channel);
	}
	if (op->timer) {
		dispatch_release(op->timer);
	}
	// For write operations, op->buf is owned by op->buf_data
	if (op->buf && op->direction == DOP_DIR_READ) {
#if defined(_WIN32)
		_aligned_free(op->buf);
#else
		free(op->buf);
#endif
	}
	if (op->buf_data) {
		_dispatch_io_data_release(op->buf_data);
	}
	if (op->data) {
		_dispatch_io_data_release(op->data);
	}
	if (op->op_q) {
		dispatch_release(op->op_q);
	}
	Block_release(op->handler);
	_dispatch_op_debug("disposed", op);
}

static void
_dispatch_operation_enqueue(dispatch_operation_t op,
		dispatch_op_direction_t direction, dispatch_data_t data)
{
	// Called from the barrier queue
	_dispatch_io_data_retain(data);
	// If channel is closed or stopped, then call the handler immediately
	int err = _dispatch_io_get_error(NULL, op->channel, false);
	if (err) {
		dispatch_io_handler_t handler = op->handler;
		dispatch_async(op->op_q, ^{
			dispatch_data_t d = data;
			if (direction == DOP_DIR_READ && err) {
				d = NULL;
			} else if (direction == DOP_DIR_WRITE && !err) {
				d = NULL;
			}
			handler(true, d, err);
			_dispatch_io_data_release(data);
		});
		_dispatch_op_debug("release -> %d, err %d", op, op->do_ref_cnt, err);
		_dispatch_release(op);
		return;
	}
	// Finish operation init
	op->fd_entry = op->channel->fd_entry;
	_dispatch_fd_entry_retain(op->fd_entry);
	dispatch_group_enter(op->fd_entry->barrier_group);
	dispatch_disk_t disk = op->fd_entry->disk;
	if (!disk) {
		dispatch_stream_t stream = op->fd_entry->streams[direction];
		dispatch_async(stream->dq, ^{
			_dispatch_stream_enqueue_operation(stream, op, data);
			_dispatch_io_data_release(data);
		});
	} else {
		dispatch_async(disk->pick_queue, ^{
			_dispatch_disk_enqueue_operation(disk, op, data);
			_dispatch_io_data_release(data);
		});
	}
}

static bool
_dispatch_operation_should_enqueue(dispatch_operation_t op,
		dispatch_queue_t tq, dispatch_data_t data)
{
	// On stream queue or disk queue
	_dispatch_op_debug("enqueue", op);
	_dispatch_io_data_retain(data);
	op->data = data;
	int err = _dispatch_io_get_error(op, NULL, true);
	if (err) {
		op->err = err;
		// Final release
		_dispatch_op_debug("release -> %d, err %d", op, op->do_ref_cnt, err);
		_dispatch_release(op);
		return false;
	}
	if (op->params.interval) {
		dispatch_resume(_dispatch_operation_timer(tq, op));
	}
	return true;
}

static dispatch_source_t
_dispatch_operation_timer(dispatch_queue_t tq, dispatch_operation_t op)
{
	// On stream queue or pick queue
	if (op->timer) {
		return op->timer;
	}
	dispatch_source_t timer = dispatch_source_create(
			DISPATCH_SOURCE_TYPE_TIMER, 0, 0, tq);
	dispatch_source_set_timer(timer,
			dispatch_time(DISPATCH_TIME_NOW, (int64_t)op->params.interval),
			op->params.interval, 0);
	dispatch_source_set_event_handler(timer, ^{
		// On stream queue or pick queue
		if (dispatch_source_testcancel(timer)) {
			// Do nothing. The operation has already completed
			return;
		}
		dispatch_op_flags_t flags = DOP_DEFAULT;
		if (op->params.interval_flags & DISPATCH_IO_STRICT_INTERVAL) {
			// Deliver even if there is less data than the low-water mark
			flags |= DOP_DELIVER;
		}
		// If the operation is active, dont deliver data
		if ((op->active) && (flags & DOP_DELIVER)) {
			op->flags = flags;
		} else {
			_dispatch_operation_deliver_data(op, flags);
		}
	});
	op->timer = timer;
	return op->timer;
}

#pragma mark -
#pragma mark dispatch_fd_entry_t

#if DISPATCH_USE_GUARDED_FD_CHANGE_FDGUARD
static void
_dispatch_fd_entry_guard(dispatch_fd_entry_t fd_entry)
{
	guardid_t guard = fd_entry;
	const unsigned int guard_flags = GUARD_CLOSE;
	int err, fd_flags = 0;
	_dispatch_io_syscall_switch_noerr(err,
		change_fdguard_np(fd_entry->fd, NULL, 0, &guard, guard_flags,
				&fd_flags),
		case 0:
			fd_entry->guard_flags = guard_flags;
			fd_entry->orig_fd_flags = fd_flags;
			break;
		case EPERM: break;
		default: (void)dispatch_assume_zero(err); break;
	);
}

static void
_dispatch_fd_entry_unguard(dispatch_fd_entry_t fd_entry)
{
	if (!fd_entry->guard_flags) {
		return;
	}
	guardid_t guard = fd_entry;
	int err, fd_flags = fd_entry->orig_fd_flags;
	_dispatch_io_syscall_switch(err,
		change_fdguard_np(fd_entry->fd, &guard, fd_entry->guard_flags, NULL, 0,
				&fd_flags),
		default: (void)dispatch_assume_zero(err); break;
	);
}
#else
#if !defined(_WIN32)
static inline void
_dispatch_fd_entry_guard(dispatch_fd_entry_t fd_entry) { (void)fd_entry; }
#endif
static inline void
_dispatch_fd_entry_unguard(dispatch_fd_entry_t fd_entry) { (void)fd_entry; }
#endif // DISPATCH_USE_GUARDED_FD

static inline dispatch_fd_t
_dispatch_fd_entry_guarded_open(dispatch_fd_entry_t fd_entry, const char *path,
		int oflag, mode_t mode) {
#if DISPATCH_USE_GUARDED_FD
	guardid_t guard = (uintptr_t)fd_entry;
	const unsigned int guard_flags = GUARD_CLOSE | GUARD_DUP |
			GUARD_SOCKET_IPC | GUARD_FILEPORT;
	int fd = guarded_open_np(path, &guard, guard_flags, oflag | O_CLOEXEC,
			mode);
	if (fd != -1) {
		fd_entry->guard_flags = guard_flags;
		return fd;
	}
	errno = 0;
#else
	(void)fd_entry;
#endif
#if defined(_WIN32)
	(void)mode;
	DWORD dwDesiredAccess = 0;
	switch (oflag & (_O_RDONLY | _O_WRONLY | _O_RDWR)) {
		case _O_RDONLY:
			dwDesiredAccess = GENERIC_READ;
			break;
		case _O_WRONLY:
			dwDesiredAccess = GENERIC_WRITE;
			break;
		case _O_RDWR:
			dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
			break;
	}
	DWORD dwCreationDisposition = OPEN_EXISTING;
	if (oflag & _O_CREAT) {
		dwCreationDisposition = OPEN_ALWAYS;
		if (oflag & _O_EXCL) {
			dwCreationDisposition = CREATE_NEW;
		} else if (oflag & _O_TRUNC) {
			dwCreationDisposition = CREATE_ALWAYS;
		}
	} else if (oflag & _O_TRUNC) {
		dwCreationDisposition = TRUNCATE_EXISTING;
	}
	return (dispatch_fd_t)CreateFile(path, dwDesiredAccess,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			dwCreationDisposition, 0, NULL);
#else
	return open(path, oflag, mode);
#endif
}

static inline int
_dispatch_fd_entry_guarded_close(dispatch_fd_entry_t fd_entry, dispatch_fd_t fd) {
#if DISPATCH_USE_GUARDED_FD
	if (fd_entry->guard_flags) {
		guardid_t guard = (uintptr_t)fd_entry;
		return guarded_close_np(fd, &guard);
	} else
#else
	(void)fd_entry;
#endif
	{
#if defined(_WIN32)
		return CloseHandle((HANDLE)fd);
#else
		return close(fd);
#endif
	}
}

static inline void
_dispatch_fd_entry_retain(dispatch_fd_entry_t fd_entry) {
	dispatch_suspend(fd_entry->close_queue);
}

static inline void
_dispatch_fd_entry_release(dispatch_fd_entry_t fd_entry) {
	dispatch_resume(fd_entry->close_queue);
}

static void
_dispatch_fd_entry_init_async(dispatch_fd_t fd,
		dispatch_fd_entry_init_callback_t completion_callback)
{
	dispatch_once_f(&_dispatch_io_init_pred, NULL,
			_dispatch_io_queues_init);
	dispatch_async(_dispatch_io_fds_lockq, ^{
		dispatch_fd_entry_t fd_entry = NULL;
		// Check to see if there is an existing entry for the given fd
		uintptr_t hash = DIO_HASH(fd);
		LIST_FOREACH(fd_entry, &_dispatch_io_fds[hash], fd_list) {
			if (fd_entry->fd == fd) {
				// Retain the fd_entry to ensure it cannot go away until the
				// stat() has completed
				_dispatch_fd_entry_retain(fd_entry);
				break;
			}
		}
		if (!fd_entry) {
			// If we did not find an existing entry, create one
			fd_entry = _dispatch_fd_entry_create_with_fd(fd, hash);
		}
		_dispatch_fd_entry_debug("init", fd_entry);
		dispatch_async(fd_entry->barrier_queue, ^{
			_dispatch_fd_entry_debug("init completion", fd_entry);
			completion_callback(fd_entry);
			// stat() is complete, release reference to fd_entry
			_dispatch_fd_entry_release(fd_entry);
		});
	});
}

static dispatch_fd_entry_t
_dispatch_fd_entry_create(dispatch_queue_t q)
{
	dispatch_fd_entry_t fd_entry;
	fd_entry = _dispatch_calloc(1ul, sizeof(struct dispatch_fd_entry_s));
	// Use target queue to ensure that no concurrent lookups are going on when
	// the close queue is running
	fd_entry->close_queue = dispatch_queue_create_with_target(
			"com.apple.libdispatch-io.closeq", NULL, q);
	// Suspend the cleanup queue until closing
	_dispatch_fd_entry_retain(fd_entry);
	return fd_entry;
}

static dispatch_fd_entry_t
_dispatch_fd_entry_create_with_fd(dispatch_fd_t fd, uintptr_t hash)
{
	// On fds lock queue
	dispatch_fd_entry_t fd_entry = _dispatch_fd_entry_create(
			_dispatch_io_fds_lockq);
	_dispatch_fd_entry_debug("create: fd %" PRId64, fd_entry, fd);
	fd_entry->fd = fd;
	LIST_INSERT_HEAD(&_dispatch_io_fds[hash], fd_entry, fd_list);
	fd_entry->barrier_queue = dispatch_queue_create(
			"com.apple.libdispatch-io.barrierq", NULL);
	fd_entry->barrier_group = dispatch_group_create();
	dispatch_async(fd_entry->barrier_queue, ^{
#if defined(_WIN32)
		DWORD dwType = GetFileType((HANDLE)fd);
		if (dwType == FILE_TYPE_PIPE) {
			if (_dispatch_handle_is_socket((HANDLE)fd)) {
				unsigned long value = 1;
				int result = ioctlsocket((SOCKET)fd, (long)FIONBIO, &value);
				(void)dispatch_assume_zero(result);
			} else {
				// Try to make writing nonblocking, although pipes not coming
				// from Foundation.Pipe may not have FILE_WRITE_ATTRIBUTES.
				DWORD dwPipeMode = 0;
				if (GetNamedPipeHandleState((HANDLE)fd, &dwPipeMode, NULL,
						NULL, NULL, NULL, 0) && !(dwPipeMode & PIPE_NOWAIT)) {
					dwPipeMode |= PIPE_NOWAIT;
					if (!SetNamedPipeHandleState((HANDLE)fd, &dwPipeMode,
							NULL, NULL)) {
						// We may end up blocking on subsequent writes, but we
						// don't have a good alternative.
						// The WriteQuotaAvailable from NtQueryInformationFile
						// erroneously returns 0 when there is a blocking read
						// on the other end of the pipe.
						_dispatch_fd_entry_debug("failed to set PIPE_NOWAIT",
								fd_entry);
					}
				}
			}

			_dispatch_stream_init(fd_entry,
				_dispatch_get_default_queue(false));
		} else {
			dispatch_suspend(fd_entry->barrier_queue);
			dispatch_once_f(&_dispatch_io_init_pred, NULL,
					_dispatch_io_queues_init);
			dispatch_async(_dispatch_io_devs_lockq, ^{
				_dispatch_disk_init(fd_entry, 0);
				dispatch_resume(fd_entry->barrier_queue);
			});
		}
#else
		_dispatch_fd_entry_debug("stat", fd_entry);
		int err, orig_flags, orig_nosigpipe = -1;
		struct stat st;
		_dispatch_io_syscall_switch(err,
			fstat(fd, &st),
			default: fd_entry->err = err; return;
		);
		fd_entry->stat.dev = st.st_dev;
		fd_entry->stat.mode = st.st_mode;
		_dispatch_fd_entry_guard(fd_entry);
		_dispatch_io_syscall_switch(err,
			orig_flags = fcntl(fd, F_GETFL),
			default: (void)dispatch_assume_zero(err); break;
		);
#if DISPATCH_USE_SETNOSIGPIPE // rdar://problem/4121123
		if (S_ISFIFO(st.st_mode)) {
			_dispatch_io_syscall_switch(err,
				orig_nosigpipe = fcntl(fd, F_GETNOSIGPIPE),
				default: (void)dispatch_assume_zero(err); break;
			);
			if (orig_nosigpipe != -1) {
				_dispatch_io_syscall_switch(err,
					orig_nosigpipe = fcntl(fd, F_SETNOSIGPIPE, 1),
					default:
						orig_nosigpipe = -1;
						(void)dispatch_assume_zero(err);
						break;
				);
			}
		}
#endif
		if (S_ISREG(st.st_mode)) {
			if (orig_flags != -1) {
				_dispatch_io_syscall_switch(err,
					fcntl(fd, F_SETFL, orig_flags & ~O_NONBLOCK),
					default:
						orig_flags = -1;
						(void)dispatch_assume_zero(err);
						break;
				);
			}
			dev_t dev = (dev_t)major(st.st_dev);
			// We have to get the disk on the global dev queue. The
			// barrier queue cannot continue until that is complete
			dispatch_suspend(fd_entry->barrier_queue);
			dispatch_once_f(&_dispatch_io_init_pred, NULL,
					_dispatch_io_queues_init);
			dispatch_async(_dispatch_io_devs_lockq, ^{
				_dispatch_disk_init(fd_entry, dev);
				dispatch_resume(fd_entry->barrier_queue);
			});
		} else {
			if (orig_flags != -1) {
				_dispatch_io_syscall_switch(err,
					fcntl(fd, F_SETFL, orig_flags | O_NONBLOCK),
					default:
						orig_flags = -1;
						(void)dispatch_assume_zero(err);
						break;
				);
			}

			_dispatch_stream_init(fd_entry,
					_dispatch_get_default_queue(false));
		}
		fd_entry->orig_flags = orig_flags;
		fd_entry->orig_nosigpipe = orig_nosigpipe;
#endif
	});
	// This is the first item run when the close queue is resumed, indicating
	// that all channels associated with this entry have been closed and that
	// all operations associated with this entry have been freed
	dispatch_async(fd_entry->close_queue, ^{
		if (!fd_entry->disk) {
			_dispatch_fd_entry_debug("close queue cleanup", fd_entry);
			dispatch_op_direction_t dir;
			for (dir = 0; dir < DOP_DIR_MAX; dir++) {
				_dispatch_stream_dispose(fd_entry, dir);
			}
		} else {
			dispatch_disk_t disk = fd_entry->disk;
			dispatch_async(_dispatch_io_devs_lockq, ^{
				_dispatch_release(disk);
			});
		}
		// Remove this entry from the global fd list
		LIST_REMOVE(fd_entry, fd_list);
	});
	// If there was a source associated with this stream, disposing of the
	// source cancels it and suspends the close queue. Freeing the fd_entry
	// structure must happen after the source cancel handler has finished
	dispatch_async(fd_entry->close_queue, ^{
		_dispatch_fd_entry_debug("close queue release", fd_entry);
		dispatch_release(fd_entry->close_queue);
		_dispatch_fd_entry_debug("barrier queue release", fd_entry);
		dispatch_release(fd_entry->barrier_queue);
		_dispatch_fd_entry_debug("barrier group release", fd_entry);
		dispatch_release(fd_entry->barrier_group);
#if !defined(_WIN32)
		if (fd_entry->orig_flags != -1) {
			_dispatch_io_syscall(
				fcntl(fd, F_SETFL, fd_entry->orig_flags)
			);
		}
#if DISPATCH_USE_SETNOSIGPIPE // rdar://problem/4121123
		if (fd_entry->orig_nosigpipe != -1) {
			_dispatch_io_syscall(
				fcntl(fd, F_SETNOSIGPIPE, fd_entry->orig_nosigpipe)
			);
		}
#endif
#endif
		_dispatch_fd_entry_unguard(fd_entry);
		if (fd_entry->convenience_channel) {
			fd_entry->convenience_channel->fd_entry = NULL;
			dispatch_release(fd_entry->convenience_channel);
		}
		free(fd_entry);
	});
	return fd_entry;
}

static dispatch_fd_entry_t
_dispatch_fd_entry_create_with_path(dispatch_io_path_data_t path_data,
		dev_t dev, mode_t mode)
{
	// On devs lock queue
	dispatch_fd_entry_t fd_entry = _dispatch_fd_entry_create(
			path_data->channel->queue);
	_dispatch_fd_entry_debug("create: path %s", fd_entry, path_data->path);
	if (S_ISREG(mode)) {
#if defined(_WIN32)
		_dispatch_disk_init(fd_entry, 0);
#else
		_dispatch_disk_init(fd_entry, (dev_t)major(dev));
#endif
	} else {
			_dispatch_stream_init(fd_entry,
					_dispatch_get_default_queue(false));
	}
	fd_entry->fd = -1;
	fd_entry->orig_flags = -1;
	fd_entry->path_data = path_data;
	fd_entry->stat.dev = dev;
	fd_entry->stat.mode = mode;
	fd_entry->barrier_queue = dispatch_queue_create(
			"com.apple.libdispatch-io.barrierq", NULL);
	fd_entry->barrier_group = dispatch_group_create();
	// This is the first item run when the close queue is resumed, indicating
	// that the channel associated with this entry has been closed and that
	// all operations associated with this entry have been freed
	dispatch_async(fd_entry->close_queue, ^{
		_dispatch_fd_entry_debug("close queue cleanup", fd_entry);
		if (!fd_entry->disk) {
			dispatch_op_direction_t dir;
			for (dir = 0; dir < DOP_DIR_MAX; dir++) {
				_dispatch_stream_dispose(fd_entry, dir);
			}
		}
		if (fd_entry->fd != -1) {
			_dispatch_fd_entry_guarded_close(fd_entry, fd_entry->fd);
		}
		if (fd_entry->path_data->channel) {
			// If associated channel has not been released yet, mark it as
			// no longer having an fd_entry (for stop after close).
			// It is safe to modify channel since we are on close_queue with
			// target queue the channel queue
			fd_entry->path_data->channel->fd_entry = NULL;
		}
	});
	dispatch_async(fd_entry->close_queue, ^{
		_dispatch_fd_entry_debug("close queue release", fd_entry);
		dispatch_release(fd_entry->close_queue);
		dispatch_release(fd_entry->barrier_queue);
		dispatch_release(fd_entry->barrier_group);
		free(fd_entry->path_data);
		free(fd_entry);
	});
	return fd_entry;
}

static int
_dispatch_fd_entry_open(dispatch_fd_entry_t fd_entry, dispatch_io_t channel)
{
	if (!(fd_entry->fd == -1 && fd_entry->path_data)) {
		return 0;
	}
	if (fd_entry->err) {
		return fd_entry->err;
	}
	dispatch_fd_t fd = -1;
	int oflag = fd_entry->disk ? fd_entry->path_data->oflag & ~O_NONBLOCK :
			fd_entry->path_data->oflag | O_NONBLOCK;
open:
	fd = _dispatch_fd_entry_guarded_open(fd_entry, fd_entry->path_data->path,
			oflag, fd_entry->path_data->mode);
	if (fd == -1) {
		int err = errno;
		if (err == EINTR) {
			goto open;
		}
		(void)os_atomic_cmpxchg2o(fd_entry, err, 0, err, relaxed);
		return err;
	}
	if (!os_atomic_cmpxchg2o(fd_entry, fd, -1, fd, relaxed)) {
		// Lost the race with another open
		_dispatch_fd_entry_guarded_close(fd_entry, fd);
	} else {
		channel->fd_actual = fd;
	}
	_dispatch_object_debug(channel, "%s", __func__);
	return 0;
}

static void
_dispatch_fd_entry_cleanup_operations(dispatch_fd_entry_t fd_entry,
		dispatch_io_t channel)
{
	if (fd_entry->disk) {
		if (channel) {
			_dispatch_retain(channel);
		}
		_dispatch_fd_entry_retain(fd_entry);
		dispatch_async(fd_entry->disk->pick_queue, ^{
			_dispatch_disk_cleanup_inactive_operations(fd_entry->disk, channel);
			_dispatch_fd_entry_release(fd_entry);
			if (channel) {
				_dispatch_release(channel);
			}
		});
	} else {
		dispatch_op_direction_t direction;
		for (direction = 0; direction < DOP_DIR_MAX; direction++) {
			dispatch_stream_t stream = fd_entry->streams[direction];
			if (!stream) {
				continue;
			}
			if (channel) {
				_dispatch_retain(channel);
			}
			_dispatch_fd_entry_retain(fd_entry);
			dispatch_async(stream->dq, ^{
				_dispatch_stream_cleanup_operations(stream, channel);
				_dispatch_fd_entry_release(fd_entry);
				if (channel) {
					_dispatch_release(channel);
				}
			});
		}
	}
}

#pragma mark -
#pragma mark dispatch_stream_t/dispatch_disk_t

static void
_dispatch_stream_init(dispatch_fd_entry_t fd_entry, dispatch_queue_t tq)
{
	dispatch_op_direction_t direction;
	for (direction = 0; direction < DOP_DIR_MAX; direction++) {
		dispatch_stream_t stream;
		stream = _dispatch_calloc(1ul, sizeof(struct dispatch_stream_s));
		stream->dq = dispatch_queue_create_with_target(
				"com.apple.libdispatch-io.streamq", NULL, tq);
		dispatch_set_context(stream->dq, stream);
		TAILQ_INIT(&stream->operations[DISPATCH_IO_RANDOM]);
		TAILQ_INIT(&stream->operations[DISPATCH_IO_STREAM]);
		fd_entry->streams[direction] = stream;
	}
}

static void
_dispatch_stream_dispose(dispatch_fd_entry_t fd_entry,
		dispatch_op_direction_t direction)
{
	// On close queue
	dispatch_stream_t stream = fd_entry->streams[direction];
	if (!stream) {
		return;
	}
	dispatch_assert(TAILQ_EMPTY(&stream->operations[DISPATCH_IO_STREAM]));
	dispatch_assert(TAILQ_EMPTY(&stream->operations[DISPATCH_IO_RANDOM]));
	if (stream->source) {
		// Balanced by source cancel handler:
		_dispatch_fd_entry_retain(fd_entry);
		dispatch_source_cancel(stream->source);
		dispatch_resume(stream->source);
		dispatch_release(stream->source);
	}
	dispatch_set_context(stream->dq, NULL);
	dispatch_release(stream->dq);
	free(stream);
}

static void
_dispatch_disk_init(dispatch_fd_entry_t fd_entry, dev_t dev)
{
	// On devs lock queue
	dispatch_disk_t disk;
	// Check to see if there is an existing entry for the given device
	uintptr_t hash = DIO_HASH(dev);
	LIST_FOREACH(disk, &_dispatch_io_devs[hash], disk_list) {
		if (disk->dev == dev) {
			_dispatch_retain(disk);
			goto out;
		}
	}
	// Otherwise create a new entry
	size_t pending_reqs_depth = dispatch_io_defaults.max_pending_io_reqs;
	disk = _dispatch_object_alloc(DISPATCH_VTABLE(disk),
			sizeof(struct dispatch_disk_s) +
			(pending_reqs_depth * sizeof(dispatch_operation_t)));
	disk->do_next = DISPATCH_OBJECT_LISTLESS;
	disk->do_xref_cnt = -1;
	disk->advise_list_depth = pending_reqs_depth;
	disk->do_targetq = _dispatch_get_default_queue(false);
	disk->dev = dev;
	TAILQ_INIT(&disk->operations);
	disk->cur_rq = TAILQ_FIRST(&disk->operations);
	char label[45];
	snprintf(label, sizeof(label), "com.apple.libdispatch-io.deviceq.%d",
			(int)dev);
	disk->pick_queue = dispatch_queue_create(label, NULL);
	LIST_INSERT_HEAD(&_dispatch_io_devs[hash], disk, disk_list);
out:
	fd_entry->disk = disk;
	TAILQ_INIT(&fd_entry->stream_ops);
}

void
_dispatch_disk_dispose(dispatch_disk_t disk, DISPATCH_UNUSED bool *allow_free)
{
	LIST_REMOVE(disk, disk_list);
	dispatch_assert(TAILQ_EMPTY(&disk->operations));
	size_t i;
	for (i = 0; i < disk->advise_list_depth; ++i) {
		dispatch_assert(!disk->advise_list[i]);
	}
	dispatch_release(disk->pick_queue);
}

#pragma mark -
#pragma mark dispatch_stream_operations/dispatch_disk_operations

static inline bool
_dispatch_stream_operation_avail(dispatch_stream_t stream)
{
	return  !(TAILQ_EMPTY(&stream->operations[DISPATCH_IO_RANDOM])) ||
			!(TAILQ_EMPTY(&stream->operations[DISPATCH_IO_STREAM]));
}

static void
_dispatch_stream_enqueue_operation(dispatch_stream_t stream,
		dispatch_operation_t op, dispatch_data_t data)
{
	if (!_dispatch_operation_should_enqueue(op, stream->dq, data)) {
		return;
	}
	_dispatch_object_debug(op, "%s", __func__);
	bool no_ops = !_dispatch_stream_operation_avail(stream);
	TAILQ_INSERT_TAIL(&stream->operations[op->params.type], op, operation_list);
	if (no_ops) {
		dispatch_async_f(stream->dq, stream->dq,
				_dispatch_stream_queue_handler);
	}
}

static void
_dispatch_disk_enqueue_operation(dispatch_disk_t disk, dispatch_operation_t op,
		dispatch_data_t data)
{
	if (!_dispatch_operation_should_enqueue(op, disk->pick_queue, data)) {
		return;
	}
	_dispatch_object_debug(op, "%s", __func__);
	if (op->params.type == DISPATCH_IO_STREAM) {
		if (TAILQ_EMPTY(&op->fd_entry->stream_ops)) {
			TAILQ_INSERT_TAIL(&disk->operations, op, operation_list);
		}
		TAILQ_INSERT_TAIL(&op->fd_entry->stream_ops, op, stream_list);
	} else {
		TAILQ_INSERT_TAIL(&disk->operations, op, operation_list);
	}
	_dispatch_disk_handler(disk);
}

static void
_dispatch_stream_complete_operation(dispatch_stream_t stream,
		dispatch_operation_t op)
{
	// On stream queue
	_dispatch_object_debug(op, "%s", __func__);
	_dispatch_op_debug("complete: stream %p", op, stream);
	TAILQ_REMOVE(&stream->operations[op->params.type], op, operation_list);
	if (op == stream->op) {
		stream->op = NULL;
	}
	if (op->timer) {
		dispatch_source_cancel(op->timer);
	}
	// Final release will deliver any pending data
	_dispatch_op_debug("release -> %d (stream complete)", op, op->do_ref_cnt);
	_dispatch_release(op);
}

static void
_dispatch_disk_complete_operation(dispatch_disk_t disk, dispatch_operation_t op)
{
	// On pick queue
	_dispatch_object_debug(op, "%s", __func__);
	_dispatch_op_debug("complete: disk %p", op, disk);
	// Current request is always the last op returned
	if (disk->cur_rq == op) {
		disk->cur_rq = TAILQ_PREV(op, dispatch_disk_operations_s,
				operation_list);
	}
	if (op->params.type == DISPATCH_IO_STREAM) {
		// Check if there are other pending stream operations behind it
		dispatch_operation_t op_next = TAILQ_NEXT(op, stream_list);
		TAILQ_REMOVE(&op->fd_entry->stream_ops, op, stream_list);
		if (op_next) {
			TAILQ_INSERT_TAIL(&disk->operations, op_next, operation_list);
		}
	}
	TAILQ_REMOVE(&disk->operations, op, operation_list);
	if (op->timer) {
		dispatch_source_cancel(op->timer);
	}
	// Final release will deliver any pending data
	_dispatch_op_debug("release -> %d (disk complete)", op, op->do_ref_cnt);
	_dispatch_release(op);
}

static dispatch_operation_t
_dispatch_stream_pick_next_operation(dispatch_stream_t stream,
		dispatch_operation_t op)
{
	// On stream queue
	if (!op) {
		// On the first run through, pick the first operation
		if (!_dispatch_stream_operation_avail(stream)) {
			return op;
		}
		if (!TAILQ_EMPTY(&stream->operations[DISPATCH_IO_STREAM])) {
			op = TAILQ_FIRST(&stream->operations[DISPATCH_IO_STREAM]);
		} else if (!TAILQ_EMPTY(&stream->operations[DISPATCH_IO_RANDOM])) {
			op = TAILQ_FIRST(&stream->operations[DISPATCH_IO_RANDOM]);
		}
		return op;
	}
	if (op->params.type == DISPATCH_IO_STREAM) {
		// Stream operations need to be serialized so continue the current
		// operation until it is finished
		return op;
	}
	// Get the next random operation (round-robin)
	if (op->params.type == DISPATCH_IO_RANDOM) {
		op = TAILQ_NEXT(op, operation_list);
		if (!op) {
			op = TAILQ_FIRST(&stream->operations[DISPATCH_IO_RANDOM]);
		}
		return op;
	}
	return NULL;
}

static dispatch_operation_t
_dispatch_disk_pick_next_operation(dispatch_disk_t disk)
{
	// On pick queue
	dispatch_operation_t op;
	if (!TAILQ_EMPTY(&disk->operations)) {
		if (disk->cur_rq == NULL) {
			op = TAILQ_FIRST(&disk->operations);
		} else {
			op = disk->cur_rq;
			do {
				op = TAILQ_NEXT(op, operation_list);
				if (!op) {
					op = TAILQ_FIRST(&disk->operations);
				}
				// TODO: more involved picking algorithm rdar://problem/8780312
			} while (op->active && op != disk->cur_rq);
		}
		if (!op->active) {
			disk->cur_rq = op;
			return op;
		}
	}
	return NULL;
}

static void
_dispatch_stream_cleanup_operations(dispatch_stream_t stream,
		dispatch_io_t channel)
{
	// On stream queue
	dispatch_operation_t op, tmp;
	__typeof__(*stream->operations) *operations;
	operations = &stream->operations[DISPATCH_IO_RANDOM];
	TAILQ_FOREACH_SAFE(op, operations, operation_list, tmp) {
		if (!channel || op->channel == channel) {
			_dispatch_stream_complete_operation(stream, op);
		}
	}
	operations = &stream->operations[DISPATCH_IO_STREAM];
	TAILQ_FOREACH_SAFE(op, operations, operation_list, tmp) {
		if (!channel || op->channel == channel) {
			_dispatch_stream_complete_operation(stream, op);
		}
	}
	if (stream->source_running && !_dispatch_stream_operation_avail(stream)) {
		dispatch_suspend(stream->source);
		stream->source_running = false;
	}
}

static inline void
_dispatch_disk_cleanup_specified_operations(dispatch_disk_t disk,
		dispatch_io_t channel, bool inactive_only)
{
	// On pick queue
	dispatch_operation_t op, tmp;
	TAILQ_FOREACH_SAFE(op, &disk->operations, operation_list, tmp) {
		if (inactive_only && op->active) continue;
		if (!channel || op->channel == channel) {
			_dispatch_op_debug("cleanup: disk %p", op, disk);
			_dispatch_disk_complete_operation(disk, op);
		}
	}
}

static void
_dispatch_disk_cleanup_operations(dispatch_disk_t disk, dispatch_io_t channel)
{
	_dispatch_disk_cleanup_specified_operations(disk, channel, false);
}

static void
_dispatch_disk_cleanup_inactive_operations(dispatch_disk_t disk,
		dispatch_io_t channel)
{
	_dispatch_disk_cleanup_specified_operations(disk, channel, true);
}

#pragma mark -
#pragma mark dispatch_stream_handler/dispatch_disk_handler

static dispatch_source_t
_dispatch_stream_source(dispatch_stream_t stream, dispatch_operation_t op)
{
	// On stream queue
	if (stream->source) {
		return stream->source;
	}
	dispatch_fd_t fd = op->fd_entry->fd;
	_dispatch_op_debug("stream source create", op);
	dispatch_source_t source = NULL;
	if (op->direction == DOP_DIR_READ) {
		source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
				(uintptr_t)fd, 0, stream->dq);
	} else if (op->direction == DOP_DIR_WRITE) {
		source = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE,
				(uintptr_t)fd, 0, stream->dq);
	} else {
		dispatch_assert(op->direction < DOP_DIR_MAX);
		return NULL;
	}
	dispatch_set_context(source, stream);
	dispatch_source_set_event_handler_f(source,
			_dispatch_stream_source_handler);
	// Close queue must not run user cleanup handlers until sources are fully
	// unregistered
	dispatch_queue_t close_queue = op->fd_entry->close_queue;
	dispatch_source_set_mandatory_cancel_handler(source, ^{
		_dispatch_op_debug("stream source cancel", op);
		dispatch_resume(close_queue);
	});
	stream->source = source;
	return stream->source;
}

static void
_dispatch_stream_source_handler(void *ctx)
{
	// On stream queue
	dispatch_stream_t stream = (dispatch_stream_t)ctx;
	dispatch_suspend(stream->source);
	stream->source_running = false;
	return _dispatch_stream_handler(stream);
}

static void
_dispatch_stream_queue_handler(void *ctx)
{
	// On stream queue
	dispatch_stream_t stream = (dispatch_stream_t)dispatch_get_context(ctx);
	if (!stream) {
		// _dispatch_stream_dispose has been called
		return;
	}
	return _dispatch_stream_handler(stream);
}

static void
_dispatch_stream_handler(void *ctx)
{
	// On stream queue
	dispatch_stream_t stream = (dispatch_stream_t)ctx;
	dispatch_operation_t op;
pick:
	op = _dispatch_stream_pick_next_operation(stream, stream->op);
	if (!op) {
		_dispatch_debug("no operation found: stream %p", stream);
		return;
	}
	int err = _dispatch_io_get_error(op, NULL, true);
	if (err) {
		op->err = err;
		_dispatch_stream_complete_operation(stream, op);
		goto pick;
	}
	stream->op = op;
	_dispatch_op_debug("stream handler", op);
	dispatch_fd_entry_t fd_entry = op->fd_entry;
	_dispatch_fd_entry_retain(fd_entry);
	// For performance analysis
	if (!op->total && dispatch_io_defaults.initial_delivery) {
		// Empty delivery to signal the start of the operation
		_dispatch_op_debug("initial delivery", op);
		_dispatch_operation_deliver_data(op, DOP_DELIVER);
	}
	// TODO: perform on the operation target queue to get correct priority
	int result = _dispatch_operation_perform(op);
	dispatch_op_flags_t flags = ~0u;
	switch (result) {
	case DISPATCH_OP_DELIVER:
		flags = DOP_DEFAULT;
		DISPATCH_FALLTHROUGH;
	case DISPATCH_OP_DELIVER_AND_COMPLETE:
		flags = (flags != DOP_DEFAULT) ? DOP_DELIVER | DOP_NO_EMPTY :
				DOP_DEFAULT;
		_dispatch_operation_deliver_data(op, flags);
		DISPATCH_FALLTHROUGH;
	case DISPATCH_OP_COMPLETE:
		if (flags != DOP_DEFAULT) {
			_dispatch_stream_complete_operation(stream, op);
		}
		if (_dispatch_stream_operation_avail(stream)) {
			dispatch_async_f(stream->dq, stream->dq,
					_dispatch_stream_queue_handler);
		}
		break;
	case DISPATCH_OP_COMPLETE_RESUME:
		_dispatch_stream_complete_operation(stream, op);
		DISPATCH_FALLTHROUGH;
	case DISPATCH_OP_RESUME:
		if (_dispatch_stream_operation_avail(stream)) {
			stream->source_running = true;
			dispatch_resume(_dispatch_stream_source(stream, op));
		}
		break;
	case DISPATCH_OP_ERR:
		_dispatch_stream_cleanup_operations(stream, op->channel);
		break;
	case DISPATCH_OP_FD_ERR:
		_dispatch_fd_entry_retain(fd_entry);
		dispatch_async(fd_entry->barrier_queue, ^{
			_dispatch_fd_entry_cleanup_operations(fd_entry, NULL);
			_dispatch_fd_entry_release(fd_entry);
		});
		break;
	default:
		break;
	}
	_dispatch_fd_entry_release(fd_entry);
	return;
}

static void
_dispatch_disk_handler(void *ctx)
{
	// On pick queue
	dispatch_disk_t disk = (dispatch_disk_t)ctx;
	if (disk->io_active) {
		return;
	}
	_dispatch_disk_debug("disk handler", disk);
	dispatch_operation_t op;
	size_t i = disk->free_idx, j = disk->req_idx;
	if (j <= i) {
		j += disk->advise_list_depth;
	}
	while (i <= j) {
		if ((!disk->advise_list[i%disk->advise_list_depth]) &&
				(op = _dispatch_disk_pick_next_operation(disk))) {
			int err = _dispatch_io_get_error(op, NULL, true);
			if (err) {
				op->err = err;
				_dispatch_disk_complete_operation(disk, op);
				continue;
			}
			_dispatch_retain(op);
			_dispatch_op_debug("retain -> %d", op, op->do_ref_cnt + 1);
			disk->advise_list[i%disk->advise_list_depth] = op;
			op->active = true;
			_dispatch_op_debug("activate: disk %p", op, disk);
			_dispatch_object_debug(op, "%s", __func__);
		} else {
			// No more operations to get
			break;
		}
		i++;
	}
	disk->free_idx = (i%disk->advise_list_depth);
	op = disk->advise_list[disk->req_idx];
	if (op) {
		disk->io_active = true;
		_dispatch_op_debug("async perform: disk %p", op, disk);
		dispatch_async_f(op->do_targetq, disk, _dispatch_disk_perform);
	}
}

static void
_dispatch_disk_perform(void *ctxt)
{
	dispatch_disk_t disk = ctxt;
	_dispatch_disk_debug("disk perform", disk);
	size_t chunk_size = dispatch_io_defaults.chunk_size;
	dispatch_operation_t op;
	size_t i = disk->advise_idx, j = disk->free_idx;
	if (j <= i) {
		j += disk->advise_list_depth;
	}
	do {
		op = disk->advise_list[i%disk->advise_list_depth];
		if (!op) {
			// Nothing more to advise, must be at free_idx
			dispatch_assert(i%disk->advise_list_depth == disk->free_idx);
			break;
		}
		if (op->direction == DOP_DIR_WRITE) {
			// TODO: preallocate writes ? rdar://problem/9032172
			continue;
		}
		if (op->fd_entry->fd == -1 && _dispatch_fd_entry_open(op->fd_entry,
				op->channel)) {
			continue;
		}
		// For performance analysis
		if (!op->total && dispatch_io_defaults.initial_delivery) {
			// Empty delivery to signal the start of the operation
			_dispatch_op_debug("initial delivery", op);
			_dispatch_operation_deliver_data(op, DOP_DELIVER);
		}
		// Advise two chunks if the list only has one element and this is the
		// first advise on the operation
		if ((j-i) == 1 && !disk->advise_list[disk->free_idx] &&
				!op->advise_offset) {
			chunk_size *= 2;
		}
		_dispatch_operation_advise(op, chunk_size);
	} while (++i < j);
	disk->advise_idx = i%disk->advise_list_depth;
	op = disk->advise_list[disk->req_idx];
	int result = _dispatch_operation_perform(op);
	disk->advise_list[disk->req_idx] = NULL;
	disk->req_idx = (disk->req_idx + 1) % disk->advise_list_depth;
	_dispatch_op_debug("async perform completion: disk %p", op, disk);
	dispatch_async(disk->pick_queue, ^{
		_dispatch_op_debug("perform completion", op);
		switch (result) {
		case DISPATCH_OP_DELIVER:
			_dispatch_operation_deliver_data(op, DOP_DEFAULT);
			break;
		case DISPATCH_OP_COMPLETE:
			_dispatch_disk_complete_operation(disk, op);
			break;
		case DISPATCH_OP_DELIVER_AND_COMPLETE:
			_dispatch_operation_deliver_data(op, DOP_DELIVER | DOP_NO_EMPTY);
			_dispatch_disk_complete_operation(disk, op);
			break;
		case DISPATCH_OP_ERR:
			_dispatch_disk_cleanup_operations(disk, op->channel);
			break;
		case DISPATCH_OP_FD_ERR:
			_dispatch_disk_cleanup_operations(disk, NULL);
			break;
		default:
			dispatch_assert(result);
			break;
		}
		_dispatch_op_debug("deactivate: disk %p", op, disk);
		op->active = false;
		disk->io_active = false;
		_dispatch_disk_handler(disk);
		// Balancing the retain in _dispatch_disk_handler. Note that op must be
		// released at the very end, since it might hold the last reference to
		// the disk
		_dispatch_op_debug("release -> %d (disk perform complete)", op,
				op->do_ref_cnt);
		_dispatch_release(op);
	});
}

#pragma mark -
#pragma mark dispatch_operation_perform

static void
_dispatch_operation_advise(dispatch_operation_t op, size_t chunk_size)
{
	_dispatch_op_debug("advise", op);
#if defined(_WIN32)
	(void)op;
	(void)chunk_size;
#else
	if (_dispatch_io_get_error(op, NULL, true)) return;
#if !defined(F_RDADVISE)
	// Compatibility struct whose values may be passed to posix_fadvise()
	struct radvisory {
		off_t ra_offset;
		int   ra_count;
	};
#endif
	int err;
	struct radvisory advise;
	// No point in issuing a read advise for the next chunk if we are already
	// a chunk ahead from reading the bytes
	if (op->advise_offset > (off_t)(((size_t)op->offset + op->total) +
			chunk_size + PAGE_SIZE)) {
		return;
	}
	_dispatch_object_debug(op, "%s", __func__);
	advise.ra_count = (int)chunk_size;
	if (!op->advise_offset) {
		op->advise_offset = op->offset;
		// If this is the first time through, align the advised range to a
		// page boundary
		size_t pg_fraction = ((size_t)op->offset + chunk_size) % PAGE_SIZE;
		advise.ra_count += (int)(pg_fraction ? PAGE_SIZE - pg_fraction : 0);
	}
	advise.ra_offset = op->advise_offset;
	op->advise_offset += advise.ra_count;
#if defined(F_RDADVISE)
	_dispatch_io_syscall_switch(err,
		fcntl(op->fd_entry->fd, F_RDADVISE, &advise),
		case EFBIG: break; // advised past the end of the file rdar://10415691
		case ENOTSUP: break; // not all FS support radvise rdar://13484629
		// TODO: set disk status on error
		default: (void)dispatch_assume_zero(err); break;
	);
#elif defined(HAVE_POSIX_FADVISE)
	err = posix_fadvise(op->fd_entry->fd, advise.ra_offset,
			(off_t)advise.ra_count, POSIX_FADV_WILLNEED);
	switch (err) {
		case 0: break;
		case EINVAL: break; // unsupported advice or file type
		case ESPIPE: break; // fd refers to a pipe or FIFO
		default: (void)dispatch_assume_zero(err); break;
	}
#else
	(void)err;
#endif // defined(F_RDADVISE)
#endif // defined(_WIN32)
}

static int
_dispatch_operation_perform(dispatch_operation_t op)
{
	_dispatch_op_debug("perform", op);
	int err = _dispatch_io_get_error(op, NULL, true);
	if (err) {
		goto error;
	}
	_dispatch_object_debug(op, "%s", __func__);
	if (!op->buf) {
		size_t max_buf_siz = op->params.high;
		size_t chunk_siz = dispatch_io_defaults.chunk_size;
		if (op->direction == DOP_DIR_READ) {
			// If necessary, create a buffer for the ongoing operation, large
			// enough to fit chunk_size but at most high-water
			size_t data_siz = dispatch_data_get_size(op->data);
			if (data_siz) {
				dispatch_assert(data_siz < max_buf_siz);
				max_buf_siz -= data_siz;
			}
			if (max_buf_siz > chunk_siz) {
				max_buf_siz = chunk_siz;
			}
			if (op->length < SIZE_MAX) {
				op->buf_siz = op->length - op->total;
				if (op->buf_siz > max_buf_siz) {
					op->buf_siz = max_buf_siz;
				}
			} else {
				op->buf_siz = max_buf_siz;
			}
#if defined(_WIN32)
			static bool bQueried = false;
			static SYSTEM_INFO siInfo;
			if (!bQueried) {
				GetNativeSystemInfo(&siInfo);
				bQueried = true;
			}
			op->buf = _aligned_malloc(op->buf_siz, siInfo.dwPageSize);
#else
			err = posix_memalign(&op->buf, (size_t)PAGE_SIZE, op->buf_siz);
			if (err != 0) {
				goto error;
			}
#endif
			_dispatch_op_debug("buffer allocated", op);
		} else if (op->direction == DOP_DIR_WRITE) {
			// Always write the first data piece, if that is smaller than a
			// chunk, accumulate further data pieces until chunk size is reached
			if (chunk_siz > max_buf_siz) {
				chunk_siz = max_buf_siz;
			}
			op->buf_siz = 0;
			dispatch_data_apply(op->data,
					^(dispatch_data_t region DISPATCH_UNUSED,
					size_t offset DISPATCH_UNUSED,
					const void* buf DISPATCH_UNUSED, size_t len) {
				size_t siz = op->buf_siz + len;
				if (!op->buf_siz || siz <= chunk_siz) {
					op->buf_siz = siz;
				}
				return (bool)(siz < chunk_siz);
			});
			if (op->buf_siz > max_buf_siz) {
				op->buf_siz = max_buf_siz;
			}
			dispatch_data_t d;
			d = dispatch_data_create_subrange(op->data, 0, op->buf_siz);
			op->buf_data = dispatch_data_create_map(d, (const void**)&op->buf,
					NULL);
			_dispatch_io_data_release(d);
			_dispatch_op_debug("buffer mapped", op);
		}
	}
	if (op->fd_entry->fd == -1) {
		err = _dispatch_fd_entry_open(op->fd_entry, op->channel);
		if (err) {
			goto error;
		}
	}
	void *buf = op->buf + op->buf_len;
	size_t len = op->buf_siz - op->buf_len;
#if defined(_WIN32)
	assert(len <= UINT_MAX && "overflow for read/write");
	LONGLONG off = (LONGLONG)((size_t)op->offset + op->total);
#else
	off_t off = (off_t)((size_t)op->offset + op->total);
#endif
#if defined(_WIN32)
	long processed = -1;
#else
	ssize_t processed = -1;
#endif
syscall:
	if (op->direction == DOP_DIR_READ) {
		if (op->params.type == DISPATCH_IO_STREAM) {
#if defined(_WIN32)
			HANDLE hFile = (HANDLE)op->fd_entry->fd;
			BOOL bSuccess;
			if (_dispatch_handle_is_socket(hFile)) {
				processed = recv((SOCKET)hFile, buf, len, 0);
				if (processed < 0) {
					bSuccess = FALSE;
					err = WSAGetLastError();
					if (err == WSAEWOULDBLOCK) {
						err = EAGAIN;
					}
					goto error;
				}
				bSuccess = TRUE;
			} else if (GetFileType(hFile) == FILE_TYPE_PIPE) {
				OVERLAPPED ovlOverlapped = {};
				DWORD dwTotalBytesAvail;
				bSuccess = PeekNamedPipe(hFile, NULL, 0, NULL,
						&dwTotalBytesAvail, NULL);
				if (bSuccess) {
					if (dwTotalBytesAvail == 0) {
						err = EAGAIN;
						goto error;
					}
					len = MIN(len, dwTotalBytesAvail);
					bSuccess = ReadFile(hFile, buf, (DWORD)len,
							(LPDWORD)&processed, &ovlOverlapped);
				}
				if (!bSuccess) {
					DWORD dwError = GetLastError();
					if (dwError == ERROR_IO_PENDING) {
						bSuccess = GetOverlappedResult(hFile, &ovlOverlapped,
								(LPDWORD)&processed, /* bWait */ TRUE);
						dwError = GetLastError();
					}
					if (dwError == ERROR_BROKEN_PIPE ||
							dwError == ERROR_NO_DATA) {
						bSuccess = TRUE;
						processed = 0;
					}
				}
			} else {
				bSuccess = ReadFile(hFile, buf, (DWORD)len,
						(LPDWORD)&processed, NULL);
			}
			if (!bSuccess) {
				err = EIO;
				goto error;
			}
#else
			processed = read(op->fd_entry->fd, buf, len);
#endif
		} else if (op->params.type == DISPATCH_IO_RANDOM) {
#if defined(_WIN32)
			OVERLAPPED ovlOverlapped = {};
			ovlOverlapped.Offset = off & 0xffffffff;
			ovlOverlapped.OffsetHigh = (off >> 32) & 0xffffffff;
			ReadFile((HANDLE)op->fd_entry->fd, buf, (DWORD)len,
					(LPDWORD)&processed, &ovlOverlapped);
#else
			processed = pread(op->fd_entry->fd, buf, len, off);
#endif
		}
	} else if (op->direction == DOP_DIR_WRITE) {
		if (op->params.type == DISPATCH_IO_STREAM) {
#if defined(_WIN32)
			HANDLE hFile = (HANDLE)op->fd_entry->fd;
			BOOL bSuccess;
			if (_dispatch_handle_is_socket(hFile)) {
				processed = send((SOCKET)hFile, buf, len, 0);
				if (processed < 0) {
					bSuccess = FALSE;
					err = WSAGetLastError();
					if (err == WSAEWOULDBLOCK) {
						err = EAGAIN;
					}
					goto error;
				}
				bSuccess = TRUE;
			} else if (GetFileType(hFile) == FILE_TYPE_PIPE) {
				// WriteFile with more bytes than are available in the
				// buffer of a NOWAIT pipe will immediately return 0,
				// so clamp our requested write length to make progress.
				IO_STATUS_BLOCK iosb;
				FILE_PIPE_LOCAL_INFORMATION fpli;
				NTSTATUS status = _dispatch_NtQueryInformationFile(hFile,
						&iosb, &fpli, sizeof(fpli), FilePipeLocalInformation);
				if (NT_SUCCESS(status)) {
					// WriteQuotaAvailable is unreliable in the presence
					// of a blocking reader, when it can return zero, so only
					// account for it otherwise
					if (fpli.WriteQuotaAvailable > 0) {
						len = MIN(len, fpli.WriteQuotaAvailable);
					}
					len = MIN(len, fpli.OutboundQuota);
				}

				OVERLAPPED ovlOverlapped = {};
				bSuccess = WriteFile(hFile, buf, (DWORD)len,
						(LPDWORD)&processed, &ovlOverlapped);
				if (!bSuccess) {
					DWORD dwError = GetLastError();
					if (dwError == ERROR_IO_PENDING) {
						bSuccess = GetOverlappedResult(hFile, &ovlOverlapped,
								(LPDWORD)&processed, /* bWait */ TRUE);
						dwError = GetLastError();
					}
					if (dwError == ERROR_BROKEN_PIPE ||
							dwError == ERROR_NO_DATA) {
						bSuccess = TRUE;
						processed = 0;
					}
				}
				if (bSuccess && processed == 0) {
					err = EAGAIN;
					goto error;
				}
			} else {
				bSuccess = WriteFile(hFile, buf, (DWORD)len,
						(LPDWORD)&processed, NULL);
			}
			if (!bSuccess) {
				err = EIO;
				goto error;
			}
#else
			processed = write(op->fd_entry->fd, buf, len);
#endif
		} else if (op->params.type == DISPATCH_IO_RANDOM) {
#if defined(_WIN32)
			OVERLAPPED ovlOverlapped = {};
			ovlOverlapped.Offset = off & 0xffffffff;
			ovlOverlapped.OffsetHigh = (off >> 32) & 0xffffffff;
			WriteFile((HANDLE)op->fd_entry->fd, buf, (DWORD)len,
					(LPDWORD)&processed, &ovlOverlapped);
#else
			processed = pwrite(op->fd_entry->fd, buf, len, off);
#endif
		}
	}
	// Encountered an error on the file descriptor
	if (processed == -1) {
		err = errno;
		if (err == EINTR) {
			goto syscall;
		}
		goto error;
	}
	// EOF is indicated by two handler invocations
	if (processed == 0) {
		_dispatch_op_debug("performed: EOF", op);
		return DISPATCH_OP_DELIVER_AND_COMPLETE;
	}
	op->buf_len += (size_t)processed;
	op->total += (size_t)processed;
	if (op->total == op->length) {
		// Finished processing all the bytes requested by the operation
		return DISPATCH_OP_COMPLETE;
	} else {
		// Deliver data only if we satisfy the filters
		return DISPATCH_OP_DELIVER;
	}
error:
	if (err == EAGAIN || err == EWOULDBLOCK) {
		// For disk based files with blocking I/O we should never get EAGAIN
		dispatch_assert(!op->fd_entry->disk);
		_dispatch_op_debug("performed: EAGAIN/EWOULDBLOCK", op);
		if (op->direction == DOP_DIR_READ && op->total &&
				op->channel == op->fd_entry->convenience_channel) {
			// Convenience read with available data completes on EAGAIN
			return DISPATCH_OP_COMPLETE_RESUME;
		}
		return DISPATCH_OP_RESUME;
	}
	_dispatch_op_debug("performed: err %d", op, err);
	op->err = err;
	switch (err) {
	case ECANCELED:
		return DISPATCH_OP_ERR;
	case EBADF:
		(void)os_atomic_cmpxchg2o(op->fd_entry, err, 0, err, relaxed);
		return DISPATCH_OP_FD_ERR;
	default:
		return DISPATCH_OP_COMPLETE;
	}
}

static void
_dispatch_operation_deliver_data(dispatch_operation_t op,
		dispatch_op_flags_t flags)
{
	// Either called from stream resp. pick queue or when op is finalized
	dispatch_data_t data = NULL;
	int err = 0;
	size_t undelivered = op->undelivered + op->buf_len;
	bool deliver = (flags & (DOP_DELIVER|DOP_DONE)) ||
			(op->flags & DOP_DELIVER);
	op->flags = DOP_DEFAULT;
	if (!deliver) {
		// Don't deliver data until low water mark has been reached
		if (undelivered >= op->params.low) {
			deliver = true;
		} else if (op->buf_len < op->buf_siz) {
			// Request buffer is not yet used up
			_dispatch_op_debug("buffer data: undelivered %zu", op, undelivered);
			return;
		}
	} else {
		err = op->err;
		if (!err && (op->channel->atomic_flags & DIO_STOPPED)) {
			err = ECANCELED;
			op->err = err;
		}
	}
	// Deliver data or buffer used up
	if (op->direction == DOP_DIR_READ) {
		if (op->buf_len) {
			void *buf = op->buf;
#if defined(_WIN32)
			// buf is allocated with _aligned_malloc()
			data = dispatch_data_create(buf, op->buf_len, NULL,
					^{ _aligned_free(buf); });
#else
			data = dispatch_data_create(buf, op->buf_len, NULL,
					DISPATCH_DATA_DESTRUCTOR_FREE);
#endif
			op->buf = NULL;
			op->buf_len = 0;
			dispatch_data_t d = dispatch_data_create_concat(op->data, data);
			_dispatch_io_data_release(op->data);
			_dispatch_io_data_release(data);
			data = d;
		} else {
			data = op->data;
		}
		op->data = deliver ? dispatch_data_empty : data;
	} else if (op->direction == DOP_DIR_WRITE) {
		if (deliver) {
			data = dispatch_data_create_subrange(op->data, op->buf_len,
					op->length);
		}
		if (op->buf_data && op->buf_len == op->buf_siz) {
			_dispatch_io_data_release(op->buf_data);
			op->buf_data = NULL;
			op->buf = NULL;
			op->buf_len = 0;
			// Trim newly written buffer from head of unwritten data
			dispatch_data_t d;
			if (deliver) {
				_dispatch_io_data_retain(data);
				d = data;
			} else {
				d = dispatch_data_create_subrange(op->data, op->buf_siz,
						op->length);
			}
			_dispatch_io_data_release(op->data);
			op->data = d;
		}
	} else {
		dispatch_assert(op->direction < DOP_DIR_MAX);
		return;
	}
	if (!deliver || ((flags & DOP_NO_EMPTY) && !dispatch_data_get_size(data))) {
		op->undelivered = undelivered;
		_dispatch_op_debug("buffer data: undelivered %zu", op, undelivered);
		return;
	}
	op->undelivered = 0;
	_dispatch_object_debug(op, "%s", __func__);
	_dispatch_op_debug("deliver data", op);
	dispatch_op_direction_t direction = op->direction;
	dispatch_io_handler_t handler = op->handler;
	dispatch_fd_entry_t fd_entry = op->fd_entry;
	_dispatch_fd_entry_retain(fd_entry);
	dispatch_io_t channel = op->channel;
	_dispatch_retain(channel);
	// Note that data delivery may occur after the operation is freed
	dispatch_async(op->op_q, ^{
		bool done = (flags & DOP_DONE);
		dispatch_data_t d = data;
		if (done) {
			if (direction == DOP_DIR_READ && err) {
				if (dispatch_data_get_size(d)) {
					_dispatch_op_debug("IO handler invoke", op);
					handler(false, d, 0);
				}
				d = NULL;
			} else if (direction == DOP_DIR_WRITE && !err) {
				d = NULL;
			}
		}
		_dispatch_op_debug("IO handler invoke: err %d", op, err);
		handler(done, d, err);
		_dispatch_release(channel);
		_dispatch_fd_entry_release(fd_entry);
		_dispatch_io_data_release(data);
	});
}

#pragma mark -
#pragma mark dispatch_io_debug

DISPATCH_COLD
static size_t
_dispatch_io_debug_attr(dispatch_io_t channel, char* buf, size_t bufsiz)
{
	dispatch_queue_t target = channel->do_targetq;
	return dsnprintf(buf, bufsiz, "type = %s, fd = 0x%" PRIxPTR ", %sfd_entry = %p, "
			"queue = %p, target = %s[%p], barrier_queue = %p, barrier_group = "
			"%p, err = 0x%x, low = 0x%zx, high = 0x%zx, interval%s = %llu ",
			channel->params.type == DISPATCH_IO_STREAM ? "stream" : "random",
			(intptr_t)channel->fd_actual, channel->atomic_flags & DIO_STOPPED ?
			"stopped, " : channel->atomic_flags & DIO_CLOSED ? "closed, " : "",
			channel->fd_entry, channel->queue, target && target->dq_label ?
			target->dq_label : "", target, channel->barrier_queue,
			channel->barrier_group, channel->err, channel->params.low,
			channel->params.high, channel->params.interval_flags &
			DISPATCH_IO_STRICT_INTERVAL ? "(strict)" : "",
			(unsigned long long) channel->params.interval);
}

size_t
_dispatch_io_debug(dispatch_io_t channel, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			_dispatch_object_class_name(channel), channel);
	offset += _dispatch_object_debug_attr(channel, &buf[offset],
			bufsiz - offset);
	offset += _dispatch_io_debug_attr(channel, &buf[offset], bufsiz - offset);
	offset += dsnprintf(&buf[offset], bufsiz - offset, "}");
	return offset;
}

DISPATCH_COLD
static size_t
_dispatch_operation_debug_attr(dispatch_operation_t op, char* buf,
		size_t bufsiz)
{
	dispatch_queue_t target = op->do_targetq;
	dispatch_queue_t oqtarget = op->op_q ? op->op_q->do_targetq : NULL;
	return dsnprintf(buf, bufsiz, "type = %s %s, fd = 0x%" PRIxPTR ", fd_entry = %p, "
			"channel = %p, queue = %p -> %s[%p], target = %s[%p], "
			"offset = %lld, length = %zu, done = %zu, undelivered = %zu, "
			"flags = %u, err = 0x%x, low = 0x%zx, high = 0x%zx, "
			"interval%s = %llu ", op->params.type == DISPATCH_IO_STREAM ?
			"stream" : "random", op->direction == DOP_DIR_READ ? "read" :
			"write", (intptr_t)(op->fd_entry ? op->fd_entry->fd : -1), op->fd_entry,
			op->channel, op->op_q, oqtarget && oqtarget->dq_label ?
			oqtarget->dq_label : "", oqtarget, target && target->dq_label ?
			target->dq_label : "", target, (long long)op->offset, op->length,
			op->total, op->undelivered + op->buf_len, op->flags, op->err,
			op->params.low, op->params.high, op->params.interval_flags &
			DISPATCH_IO_STRICT_INTERVAL ? "(strict)" : "",
			(unsigned long long)op->params.interval);
}

size_t
_dispatch_operation_debug(dispatch_operation_t op, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			_dispatch_object_class_name(op), op);
	offset += _dispatch_object_debug_attr(op, &buf[offset], bufsiz - offset);
	offset += _dispatch_operation_debug_attr(op, &buf[offset], bufsiz - offset);
	offset += dsnprintf(&buf[offset], bufsiz - offset, "}");
	return offset;
}
