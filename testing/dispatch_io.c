/*
 * Copyright (c) 2009-2011 Apple Inc. All rights reserved.
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

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fts.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libkern/OSAtomic.h>
#include <TargetConditionals.h>

#include <dispatch/dispatch.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#ifndef DISPATCHTEST_IO
#if DISPATCH_API_VERSION >= 20100226 && DISPATCH_API_VERSION != 20101110
#define DISPATCHTEST_IO 1
#if DISPATCH_API_VERSION >= 20100723
#define DISPATCHTEST_IO_PATH 1 // rdar://problem/7738093
#endif
#endif
#endif

void
test_fin(void *cxt)
{
	test_ptr("test_fin run", cxt, cxt);
	test_stop();
}

#if DISPATCHTEST_IO

#if TARGET_OS_EMBEDDED
#define LARGE_FILE "/System/Library/Fonts/Cache/STHeiti-Light.ttc" // 29MB file
#define maxopenfiles 768
#else
#define LARGE_FILE "/System/Library/Speech/Voices/Alex.SpeechVoice/Contents/Resources/PCMWave" // 417MB file
#define maxopenfiles 4096
#endif

static void
test_io_close(int with_timer, bool from_path)
{
	#define chunks 4
	#define READSIZE (512*1024)
	unsigned int i;
	const char *path = LARGE_FILE;
	int fd = open(path, O_RDONLY);
	if (fd == -1) {
		if (errno == ENOENT) {
			test_skip("Large file not found");
			return;
		}
		test_errno("open", errno, 0);
		test_stop();
	}
	if (fcntl(fd, F_GLOBAL_NOCACHE, 1) == -1) {
		test_errno("fcntl F_GLOBAL_NOCACHE", errno, 0);
		test_stop();
	}
	struct stat sb;
	if (fstat(fd, &sb)) {
		test_errno("fstat", errno, 0);
		test_stop();
	}
	const size_t size = sb.st_size / chunks;
	const int expected_error = with_timer? ECANCELED : 0;
	dispatch_source_t t = NULL;
	dispatch_group_t g = dispatch_group_create();
	dispatch_group_enter(g);
	void (^cleanup_handler)(int error) = ^(int error) {
		test_errno("create error", error, 0);
		dispatch_group_leave(g);
		close(fd);
	};
	dispatch_io_t io;
	if (!from_path) {
		io = dispatch_io_create(DISPATCH_IO_RANDOM, fd,
				dispatch_get_global_queue(0, 0), cleanup_handler);
	} else {
#if DISPATCHTEST_IO_PATH
		io = dispatch_io_create_with_path(DISPATCH_IO_RANDOM, path, O_RDONLY, 0,
				dispatch_get_global_queue(0, 0), cleanup_handler);
#endif
	}
	dispatch_io_set_high_water(io, READSIZE);
	if (with_timer == 1) {
		dispatch_io_set_low_water(io, READSIZE);
		dispatch_io_set_interval(io,  2 * NSEC_PER_SEC,
				DISPATCH_IO_STRICT_INTERVAL);
	} else if (with_timer == 2) {
		t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
				dispatch_get_global_queue(0,0));
		dispatch_retain(io);
		dispatch_source_set_event_handler(t, ^{
			dispatch_io_close(io, DISPATCH_IO_STOP);
			dispatch_source_cancel(t);
		});
		dispatch_source_set_cancel_handler(t, ^{
			dispatch_release(io);
		});
		dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW,
				2 * NSEC_PER_SEC), DISPATCH_TIME_FOREVER, 0);
		dispatch_resume(t);
	}

	size_t chunk_sizes[chunks] = {}, *chunk_size = chunk_sizes, total = 0;
	dispatch_data_t data_chunks[chunks], *data = data_chunks;
	for (i = 0; i < chunks; i++) {
		data[i] = dispatch_data_empty;
		dispatch_group_enter(g);
		dispatch_io_read(io, i * size, size, dispatch_get_global_queue(0,0),
				^(bool done, dispatch_data_t d, int error) {
			if (d) {
				chunk_size[i] += dispatch_data_get_size(d);
				dispatch_data_t concat = dispatch_data_create_concat(data[i], d);
				dispatch_release(data[i]);
				data[i] = concat;
				if ((dispatch_data_get_size(d) < READSIZE && !error && !done)) {
					// The timer must have fired
					dispatch_io_close(io, DISPATCH_IO_STOP);
					return;
				}
			}
			if (done) {
				test_errno("read error", error,
						error == expected_error ? expected_error : 0);
				dispatch_group_leave(g);
				dispatch_release(data[i]);
			}
		});
	}
	dispatch_io_close(io, 0);
	dispatch_io_close(io, 0);
	dispatch_io_read(io, 0, 1, dispatch_get_global_queue(0,0),
			^(bool done, dispatch_data_t d, int error) {
		test_long("closed done", done, true);
		test_errno("closed error", error, ECANCELED);
		test_ptr_null("closed data", d);
	});
	dispatch_release(io);
	test_group_wait(g);
	dispatch_release(g);
	if (t) {
		dispatch_source_cancel(t);
		dispatch_release(t);
	}
	for (i = 0; i < chunks; i++) {
		if (with_timer) {
			test_long_less_than("chunk size", chunk_size[i], size + 1);
		} else {
			test_long("chunk size", chunk_size[i], size);
		}
		total += chunk_size[i];
	}
	if (with_timer) {
		test_long_less_than("total size", total, chunks * size + 1);
	} else {
		test_long("total size", total, chunks * size);
	}
}

static void
test_io_stop(void) // rdar://problem/8250057
{
	int fds[2], *fd = fds;
	if(pipe(fd) == -1) {
		test_errno("pipe", errno, 0);
		test_stop();
	}
	dispatch_group_t g = dispatch_group_create();
	dispatch_group_enter(g);
	dispatch_io_t io = dispatch_io_create(DISPATCH_IO_STREAM, *fd,
			dispatch_get_global_queue(0, 0), ^(int error) {
		test_errno("create error", error, 0);
		close(*fd);
		close(*(fd+1));
		dispatch_group_leave(g);
	});
	dispatch_group_enter(g);
	dispatch_io_read(io, 0, 1, dispatch_get_global_queue(0, 0),
			^(bool done, dispatch_data_t d __attribute__((unused)), int error) {
		if (done) {
			test_errno("read error", error, ECANCELED);
			dispatch_group_leave(g);
		}
	});
	dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0,
			0, dispatch_get_global_queue(0,0));
	dispatch_retain(io);
	dispatch_source_set_event_handler(t, ^{
		dispatch_io_close(io, DISPATCH_IO_STOP);
		dispatch_release(io);
	});
	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW,
			2 * NSEC_PER_SEC), DISPATCH_TIME_FOREVER, 0);
	dispatch_resume(t);
	dispatch_release(io);
	test_group_wait(g);
	dispatch_release(g);
	dispatch_source_cancel(t);
	dispatch_release(t);
}

static void
test_io_read_write(void)
{
	const char *path_in = "/usr/share/dict/words";
	char path_out[] = "/tmp/dispatchtest_io.XXXXXX";
	const size_t siz_in = 1024 * 1024;

	int in = open(path_in, O_RDONLY);
	if (in == -1) {
		test_errno("open", errno, 0);
		test_stop();
	}
	int out = mkstemp(path_out);
	if (out == -1) {
		test_errno("mkstemp", errno, 0);
		test_stop();
	}
	if (unlink(path_out) == -1) {
		test_errno("unlink", errno, 0);
		test_stop();
	}
	dispatch_queue_t q = dispatch_get_global_queue(0,0);
	dispatch_group_t g = dispatch_group_create();
	dispatch_group_enter(g);
	dispatch_io_t io_in = dispatch_io_create(DISPATCH_IO_STREAM, in,
			q, ^(int error) {
		test_errno("dispatch_io_create", error, 0);
		close(in);
		dispatch_group_leave(g);
	});
	dispatch_io_set_high_water(io_in, siz_in/4);
	dispatch_group_enter(g);
	dispatch_io_t io_out = dispatch_io_create(DISPATCH_IO_STREAM, out,
			q, ^(int error) {
		test_errno("dispatch_io_create", error, 0);
		dispatch_group_leave(g);
	});
	dispatch_io_set_high_water(io_out, siz_in/16);
	__block dispatch_data_t data = dispatch_data_empty;
	dispatch_group_enter(g);
	dispatch_io_read(io_in, 0, siz_in, q,
			^(bool done_in, dispatch_data_t data_in, int err_in) {
		test_long_less_than("read size", dispatch_data_get_size(data_in),
				siz_in);
		if (data_in) {
			dispatch_group_enter(g);
			dispatch_io_write(io_out, 0, data_in, q,
					^(bool done_out, dispatch_data_t data_out, int err_out) {
				if (done_out) {
					test_errno("dispatch_io_write", err_out, 0);
					test_long("remaining write size",
							data_out ? dispatch_data_get_size(data_out) : 0, 0);
					dispatch_group_leave(g);
				} else {
					test_long_less_than("remaining write size",
							dispatch_data_get_size(data_out), siz_in);
				}
			});
			dispatch_data_t concat = dispatch_data_create_concat(data, data_in);
			dispatch_release(data);
			data = concat;
		}
		if (done_in) {
			test_errno("dispatch_io_read", err_in, 0);
			dispatch_release(io_out);
			dispatch_group_leave(g);
		}
	});
	dispatch_release(io_in);
	test_group_wait(g);
	lseek(out, 0, SEEK_SET);
	dispatch_group_enter(g);
	dispatch_read(out, siz_in, q,
			^(dispatch_data_t cmp, int err_cmp) {
		if (err_cmp) {
			test_errno("dispatch_read", err_cmp, 0);
			test_stop();
		}
		close(out);
		size_t siz_cmp = dispatch_data_get_size(cmp);
		test_long("readback size", siz_cmp, siz_in);
		const void *data_buf, *cmp_buf;
		dispatch_data_t data_map, cmp_map;
		data_map = dispatch_data_create_map(data, &data_buf, NULL);
		cmp_map = dispatch_data_create_map(cmp, &cmp_buf, NULL);
		test_long("readback memcmp",
				memcmp(data_buf, cmp_buf, MIN(siz_in, siz_cmp)), 0);
		dispatch_release(cmp_map);
		dispatch_release(data_map);
		dispatch_release(data);
		dispatch_group_leave(g);
	});
	test_group_wait(g);
	dispatch_release(g);
}

enum {
	DISPATCH_ASYNC_READ_ON_CONCURRENT_QUEUE = 0,
	DISPATCH_ASYNC_READ_ON_SERIAL_QUEUE,
	DISPATCH_READ_ON_CONCURRENT_QUEUE,
	DISPATCH_IO_READ_ON_CONCURRENT_QUEUE,
	DISPATCH_IO_READ_FROM_PATH_ON_CONCURRENT_QUEUE,
};

static void
test_async_read(char *path, size_t size, int option, dispatch_queue_t queue,
		void (^process_data)(size_t))
{
	int fd = open(path, O_RDONLY);
	if (fd == -1) {
		test_errno("Failed to open file", errno, 0);
		test_stop();
	}
	// disable caching also for extra fd opened by dispatch_io_create_with_path
	if (fcntl(fd, F_GLOBAL_NOCACHE, 1) == -1) {
		test_errno("fcntl F_GLOBAL_NOCACHE", errno, 0);
		test_stop();
	}
	switch (option) {
		case DISPATCH_ASYNC_READ_ON_CONCURRENT_QUEUE:
		case DISPATCH_ASYNC_READ_ON_SERIAL_QUEUE:
			dispatch_async(queue, ^{
				char* buffer = valloc(size);
				ssize_t r = read(fd, buffer, size);
				if (r == -1) {
					test_errno("async read error", errno, 0);
					test_stop();
				}
				free(buffer);
				close(fd);
				process_data(r);
			});
			break;
		case DISPATCH_READ_ON_CONCURRENT_QUEUE:
			dispatch_read(fd, size, queue, ^(dispatch_data_t d, int error) {
				if (error) {
					test_errno("dispatch_read error", error, 0);
					test_stop();
				}
				close(fd);
				process_data(dispatch_data_get_size(d));
			});
			break;
		case DISPATCH_IO_READ_ON_CONCURRENT_QUEUE:
		case DISPATCH_IO_READ_FROM_PATH_ON_CONCURRENT_QUEUE: {
			__block bool is_done = false;
			__block dispatch_data_t d = dispatch_data_empty;
			void (^cleanup_handler)(int error) = ^(int error) {
				if (error) {
					test_errno("dispatch_io_create error", error, 0);
					test_stop();
				}
				if (!is_done) {
					test_long("dispatch_io_read done", is_done, true);
				}
				close(fd);
				process_data(dispatch_data_get_size(d));
				dispatch_release(d);
			};
			dispatch_io_t io = NULL;
			if (option == DISPATCH_IO_READ_FROM_PATH_ON_CONCURRENT_QUEUE) {
#if DISPATCHTEST_IO_PATH
				io = dispatch_io_create_with_path(DISPATCH_IO_RANDOM, path,
						O_RDONLY, 0, queue, cleanup_handler);
#endif
			} else {
				io = dispatch_io_create(DISPATCH_IO_RANDOM, fd, queue,
						cleanup_handler);
			}
			if (!io) {
				test_ptr_notnull("dispatch_io_create", io);
				test_stop();
			}

			// Timeout after 20 secs
			dispatch_io_set_interval(io, 20 * NSEC_PER_SEC,
					DISPATCH_IO_STRICT_INTERVAL);

			dispatch_io_read(io, 0, size, queue,
					^(bool done, dispatch_data_t data, int error) {
				if (!done && !error && !dispatch_data_get_size(data)) {
					// Timer fired, and no progress from last delivery
					dispatch_io_close(io, DISPATCH_IO_STOP);
				}
				if (data) {
					dispatch_data_t c = dispatch_data_create_concat(d, data);
					dispatch_release(d);
					d = c;
				}
				if (error) {
					test_errno("dispatch_io_read error", error, 0);
					if (error != ECANCELED) {
						test_stop();
					}
				}
				is_done = done;
			});
			dispatch_release(io);
			break;
		}
	}
}

static int
test_read_dirs(char **paths, dispatch_queue_t queue, dispatch_group_t g,
		dispatch_semaphore_t s, volatile int32_t *bytes, int option)
{
	FTS *tree = fts_open(paths, FTS_PHYSICAL|FTS_XDEV, NULL);
	if (!tree) {
		test_ptr_notnull("fts_open failed", tree);
		test_stop();
	}
	unsigned int files_opened = 0;
	size_t size, total_size = 0;
	FTSENT *node;
	while ((node = fts_read(tree)) &&
			!(node->fts_info == FTS_ERR || node->fts_info == FTS_NS)) {
		if (node->fts_level > 0 && node->fts_name[0] == '.') {
			fts_set(tree, node, FTS_SKIP);
		} else if (node->fts_info == FTS_F) {
			dispatch_group_enter(g);
			dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
			size = node->fts_statp->st_size;
			total_size += size;
			files_opened++;
			test_async_read(node->fts_path, size, option, queue, ^(size_t len){
				OSAtomicAdd32(len, bytes);
				dispatch_semaphore_signal(s);
				dispatch_group_leave(g);
			});
		}
	}
	if ((!node && errno) || (node && (node->fts_info == FTS_ERR ||
			node->fts_info == FTS_NS))) {
		test_errno("fts_read failed", !node ? errno : node->fts_errno, 0);
		test_stop();
	}
	if (fts_close(tree)) {
		test_errno("fts_close failed", errno, 0);
		test_stop();
	}
	test_group_wait(g);
	test_long("total size", *bytes, total_size);
	return files_opened;
}

extern __attribute__((weak_import)) void
_dispatch_iocntl(uint32_t param, uint64_t value);

enum {
	DISPATCH_IOCNTL_CHUNK_PAGES = 1,
	DISPATCH_IOCNTL_LOW_WATER_CHUNKS,
	DISPATCH_IOCNTL_INITIAL_DELIVERY,
};

static void
test_read_many_files(void)
{
	char *paths[] = {"/usr/lib", NULL};
	dispatch_group_t g = dispatch_group_create();
	dispatch_semaphore_t s = dispatch_semaphore_create(maxopenfiles);
	uint64_t start;
	volatile int32_t bytes;
	unsigned int files_read, i;

	const dispatch_queue_t queues[] = {
		[DISPATCH_ASYNC_READ_ON_CONCURRENT_QUEUE] =
				dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0),
		[DISPATCH_ASYNC_READ_ON_SERIAL_QUEUE] =
				dispatch_queue_create("read", NULL),
		[DISPATCH_READ_ON_CONCURRENT_QUEUE] =
				dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0),
		[DISPATCH_IO_READ_ON_CONCURRENT_QUEUE] =
				dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0),
#if DISPATCHTEST_IO_PATH
		[DISPATCH_IO_READ_FROM_PATH_ON_CONCURRENT_QUEUE] =
				dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0),
#endif
	};
	dispatch_set_target_queue(queues[DISPATCH_ASYNC_READ_ON_SERIAL_QUEUE],
			dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0));
	static const char *names[] = {
		[DISPATCH_ASYNC_READ_ON_CONCURRENT_QUEUE] =
			"dispatch_async(^{read();}) on concurrent queue",
		[DISPATCH_ASYNC_READ_ON_SERIAL_QUEUE] =
			"dispatch_async(^{read();}) on serial queue",
		[DISPATCH_READ_ON_CONCURRENT_QUEUE] =
			"dispatch_read() on concurrent queue",
		[DISPATCH_IO_READ_ON_CONCURRENT_QUEUE] =
			"dispatch_io_read() on concurrent queue",
		[DISPATCH_IO_READ_FROM_PATH_ON_CONCURRENT_QUEUE] =
			"dispatch_io_read() from path on concurrent queue",
	};

	if (_dispatch_iocntl) {
		const size_t chunk_pages = 3072;
		_dispatch_iocntl(DISPATCH_IOCNTL_CHUNK_PAGES, (uint64_t)chunk_pages);
	}
	struct rlimit l;
	if (!getrlimit(RLIMIT_NOFILE, &l) && l.rlim_cur < 2 * maxopenfiles + 256) {
		l.rlim_cur = 2 * maxopenfiles + 256;
		setrlimit(RLIMIT_NOFILE, &l);
	}
	for (i = 0; i < sizeof(queues)/sizeof(dispatch_queue_t); ++i) {
		fprintf(stdout, "%s:\n", names[i]);
		bytes = 0;
		start = mach_absolute_time();
		files_read = test_read_dirs(paths, queues[i], g, s, &bytes, i);
		double elapsed = (double)(mach_absolute_time() - start) / NSEC_PER_SEC;
		double throughput = ((double)bytes / elapsed)/(1024 * 1024);
		fprintf(stdout, "Total Files read: %u, Total MBytes %g, "
				"Total time: %g s, Throughput: %g MB/s\n", files_read,
				(double)bytes / (1024 * 1024), elapsed, throughput);
	}
	dispatch_release(queues[DISPATCH_ASYNC_READ_ON_SERIAL_QUEUE]);
	dispatch_release(s);
	dispatch_release(g);
}

static void
test_io_from_io(void) // rdar://problem/8388909
{
#if DISPATCH_API_VERSION >= 20101012
	const size_t siz_in = 10240;
	dispatch_queue_t q = dispatch_get_global_queue(0, 0);
	char path[] = "/tmp/dispatchtest_io.XXXXXX/file.name";
	char *tmp = strrchr(path, '/');
	*tmp = '\0';
	if (!mkdtemp(path)) {
		test_ptr_notnull("mkdtemp failed", path);
		test_stop();
	}
	// Make the directory immutable
	if (chflags(path, UF_IMMUTABLE) == -1) {
		test_errno("chflags", errno, 0);
		test_stop();
	}
	*tmp = '/';
	dispatch_io_t io = dispatch_io_create_with_path(DISPATCH_IO_RANDOM, path,
			O_CREAT|O_RDWR, 0600, q, ^(int error) {
				if (error) {
					test_errno("channel cleanup called with error", error, 0);
					test_stop();
				}
				test_errno("channel cleanup called", error, 0);
			});

	dispatch_group_t g = dispatch_group_create();
	char *foo = malloc(256);
	dispatch_data_t tdata;
	tdata = dispatch_data_create(foo, 256, NULL, DISPATCH_DATA_DESTRUCTOR_FREE);
	dispatch_group_enter(g);
	dispatch_io_write(io, 0, tdata, q, ^(bool done, dispatch_data_t data_out,
			int err_out) {
		test_errno("error from write to immutable directory", err_out, EPERM);
		test_long("unwritten data", dispatch_data_get_size(data_out), 256);
		if (!err_out && done) {
			test_stop();
		}
		if (done) {
			dispatch_group_leave(g);
		}
	});
	dispatch_release(tdata);
	dispatch_release(io);
	test_group_wait(g);
	// Change the directory to mutable
	*tmp = '\0';
	if (chflags(path, 0) == -1) {
		test_errno("chflags", errno, 0);
		test_stop();
	}
	const char *path_in = "/dev/random";
	int in = open(path_in, O_RDONLY);
	if (in == -1) {
		test_errno("open", errno, 0);
		test_stop();
	}
	*tmp = '/';
	dispatch_group_enter(g);

	io = dispatch_io_create_with_path(DISPATCH_IO_RANDOM, path,
			O_CREAT|O_RDWR, 0600, q, ^(int error) {
		if (error) {
			test_errno("channel cleanup called with error", error, 0);
			test_stop();
		}
		test_errno("channel cleanup called", error, 0);
	});
	dispatch_read(in, siz_in, q, ^(dispatch_data_t data_in, int err_in ) {
		if (err_in) {
			test_errno("dispatch_read", err_in, 0);
			test_stop();
		}
		dispatch_io_write(io, 0, data_in, q,
				^(bool done, dispatch_data_t data_out, int err_out) {
			if (done) {
				test_errno("dispatch_io_write", err_out, 0);
				test_long("remaining write size",
						data_out ? dispatch_data_get_size(data_out) : 0, 0);
				dispatch_group_leave(g);
			} else {
				test_long_less_than("remaining write size",
						dispatch_data_get_size(data_out), siz_in);
			}
		});
	});
	test_group_wait(g);
	dispatch_io_t io2 = dispatch_io_create_with_io(DISPATCH_IO_STREAM, io, q,
			^(int error) {
		if (error) {
			test_errno("dispatch_io_create_with_io", error, 0);
			test_stop();
		}
	});
	dispatch_release(io);
	dispatch_group_enter(g);
	__block dispatch_data_t data_out = dispatch_data_empty;
	dispatch_io_read(io2, 0, siz_in, q,
			^(bool done, dispatch_data_t d, int error) {
		if (d) {
			dispatch_data_t concat = dispatch_data_create_concat(data_out, d);
			dispatch_release(data_out);
			data_out = concat;
		}
		if (done) {
			test_errno("read error from channel created_with_io", error, 0);
			dispatch_group_leave(g);
		}
	});
	dispatch_release(io2);
	test_group_wait(g);
	dispatch_release(g);
	test_long("readback size", dispatch_data_get_size(data_out), siz_in);
	dispatch_release(data_out);
#endif
}

#endif // DISPATCHTEST_IO

int
main(void)
{
	dispatch_test_start("Dispatch IO");
	dispatch_async(dispatch_get_main_queue(), ^{
#if DISPATCHTEST_IO
		int i; bool from_path = false;
		do {
			for (i = 0; i < 3; i++) {
				test_io_close(i, from_path);
			}
#if DISPATCHTEST_IO_PATH
			from_path = !from_path;
#endif
		} while (from_path);
		test_io_stop();
		test_io_from_io();
		test_io_read_write();
		test_read_many_files();
#endif
		test_fin(NULL);
	});
	dispatch_main();
}
