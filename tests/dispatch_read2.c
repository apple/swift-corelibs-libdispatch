/*
 * Copyright (c) 2010-2011 Apple Inc. All rights reserved.
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
#include <Block.h>

#include <dispatch/dispatch.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#ifndef DISPATCHTEST_IO
#if DISPATCH_API_VERSION >= 20100226 && DISPATCH_API_VERSION != 20101110
#define DISPATCHTEST_IO 1
#endif
#endif

void
test_fin(void *cxt)
{
	test_ptr("test_fin run", cxt, cxt);
	test_stop();
}

#if DISPATCHTEST_IO

/*
 Basic way of implementing dispatch_io's dispatch_read without
 using dispatch channel api's
 */
static void
dispatch_read2(dispatch_fd_t fd,
			  size_t length,
			  dispatch_queue_t queue,
			  void (^handler)(dispatch_data_t d, int error))
{
	if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
		test_errno("fcntl O_NONBLOCK", errno, 0);
		test_stop();
	}
	dispatch_source_t reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
			fd, 0, queue);
	test_ptr_notnull("reader", reader);

	__block size_t bytes_read = 0;
	__block dispatch_data_t data = dispatch_data_empty;
	__block int err = 0;
	dispatch_source_set_event_handler(reader, ^{
		const ssize_t bufsiz = 1024*512; // 512KB buffer
		char *buffer = valloc(bufsiz);
		ssize_t actual = read(fd, buffer, bufsiz);
		if (actual == -1) {
			err = errno;
		}
		if (actual > 0) {
			bytes_read += actual;
			dispatch_data_t tmp_data = dispatch_data_create(buffer, actual,
					NULL, DISPATCH_DATA_DESTRUCTOR_FREE);
			dispatch_data_t concat = dispatch_data_create_concat(data,tmp_data);
			dispatch_release(tmp_data);
			dispatch_release(data);
			data = concat;
		}
		// If we reached EOF or we read as much we were asked to.
		if (actual < bufsiz || bytes_read >= length) {
			char foo[2];
			actual = read(fd, foo, 2);
			bytes_read += actual;
			// confirm EOF condition
			test_long("EOF", actual, 0);
			dispatch_source_cancel(reader);
		}
	});

	dispatch_source_set_cancel_handler(reader, ^{
		dispatch_data_t d = dispatch_data_create_subrange(data, 0, length);
		dispatch_release(data);
		handler(d, err);
		dispatch_release(d);
		dispatch_release(reader);
	});

	dispatch_resume(reader);
}

static void
test_read(void)
{
	const char *path = "/usr/share/dict/words";
	int fd = open(path, O_RDONLY);
	if (fd == -1) {
		test_errno("open", errno, 0);
		test_stop();
	}
	if (fcntl(fd, F_NOCACHE, 1)) {
		test_errno("fcntl F_NOCACHE", errno, 0);
		test_stop();
	}
	struct stat sb;
	if (fstat(fd, &sb)) {
		test_errno("fstat", errno, 0);
		test_stop();
	}
	size_t size = sb.st_size;
	dispatch_group_t g = dispatch_group_create();
	void (^b)(dispatch_data_t, int) = ^(dispatch_data_t d, int error) {
		test_errno("read error", error, 0);
		test_long("dispatch data size", d ? dispatch_data_get_size(d) : 0, size);
		if (d) {
			const void *contig_buf;
			size_t contig_size;
			dispatch_data_t tmp = dispatch_data_create_map(d, &contig_buf,
					&contig_size);
			test_long("dispatch data contig size", contig_size, size);
			if (contig_size) {
				// Validate the copied buffer is similar to what we expect
				char *buf = (char*)malloc(size);
				pread(fd, buf, size, 0);
				test_long("dispatch data contents", memcmp(buf, contig_buf,
						size), 0);
				free(buf);
			}
			dispatch_release(tmp);
		}
		dispatch_group_leave(g);
	};
	dispatch_group_enter(g);
	dispatch_read(fd, SIZE_MAX, dispatch_get_global_queue(0, 0), b); // rdar://problem/7795794
	test_group_wait(g);
	lseek(fd, 0, SEEK_SET);
	if (dispatch_test_check_evfilt_read_for_fd(fd)) {
		dispatch_group_enter(g);
		dispatch_read2(fd, size, dispatch_get_global_queue(0,0), b);
		test_group_wait(g);
	} else {
		test_skip("EVFILT_READ kevent not firing for test file");
	}
	dispatch_release(g);
	close(fd);
}

static void
test_read_write(void)
{
	const char *path_in = "/dev/random";
	char path_out[] = "/tmp/dispatchtest_io.XXXXXX";
	const size_t siz_in = 10240;

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
	__block dispatch_data_t data;
	dispatch_read(in, siz_in, q, ^(dispatch_data_t data_in, int err_in) {
		if (err_in) {
			test_errno("dispatch_read", err_in, 0);
			test_stop();
		}
		close(in);
		size_t siz_out = dispatch_data_get_size(data_in);
		test_long("read size", siz_out, siz_in);
		dispatch_retain(data_in);
		data = data_in;
		dispatch_write(out, data, q, ^(dispatch_data_t data_out, int err_out) {
			if (err_out || data_out) {
				test_errno("dispatch_write", err_out, 0);
				test_stop();
			}
			lseek(out, 0, SEEK_SET);
			dispatch_read(out, siz_out, q,
					^(dispatch_data_t cmp, int err_cmp) {
				if (err_cmp) {
					test_errno("dispatch_read", err_cmp, 0);
					test_stop();
				}
				close(out);
				size_t siz_cmp = dispatch_data_get_size(cmp);
				test_long("readback size", siz_cmp, siz_out);
				const void *data_buf, *cmp_buf;
				dispatch_data_t data_map, cmp_map;
				data_map = dispatch_data_create_map(data, &data_buf, NULL);
				cmp_map = dispatch_data_create_map(cmp, &cmp_buf, NULL);
				test_long("readback memcmp",
						memcmp(data_buf, cmp_buf, MIN(siz_out, siz_cmp)), 0);
				dispatch_release(cmp_map);
				dispatch_release(data_map);
				dispatch_release(data);
				dispatch_group_leave(g);
			});
		});
	});
	test_group_wait(g);
	dispatch_release(g);
}

static void
test_read_writes(void) // <rdar://problem/7785143>
{
	const char *path_in = "/dev/random";
	char path_out[] = "/tmp/dispatchtest_io.XXXXXX";
	const size_t chunks_out = 320;
	const size_t siz_chunk = 32, siz_in = siz_chunk * chunks_out;

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
	__block dispatch_data_t data;
	__block size_t siz_out;
	dispatch_read(in, siz_in, q, ^(dispatch_data_t data_in, int err_in) {
		if (err_in) {
			test_errno("dispatch_read", err_in, 0);
			test_stop();
		}
		close(in);
		siz_out = dispatch_data_get_size(data_in);
		test_long("read size", siz_out, siz_in);
		dispatch_retain(data_in);
		data = data_in;
		dispatch_data_t data_chunks[chunks_out];
		size_t i;
		for (i = 0; i < chunks_out; i++) {
			data_chunks[i] = dispatch_data_create_subrange(data_in,
					i * siz_chunk, siz_chunk);
		}
		for (i = 0; i < chunks_out; i++) {
			dispatch_data_t d = data_chunks[i];
			dispatch_group_enter(g);
			dispatch_write(out, d, q, ^(dispatch_data_t data_out,
					int err_out) {
				if (err_out || data_out) {
					test_errno("dispatch_write", err_out, 0);
					test_stop();
				}
				dispatch_group_leave(g);
			});
		}
		for (i = 0; i < chunks_out; i++) {
			dispatch_release(data_chunks[i]);
		}
		dispatch_group_leave(g);
	});
	test_group_wait(g);
	dispatch_group_enter(g);
	lseek(out, 0, SEEK_SET);
	dispatch_read(out, siz_in, q,
			^(dispatch_data_t cmp, int err_cmp) {
		if (err_cmp) {
			test_errno("dispatch_read", err_cmp, 0);
			test_stop();
		}
		close(out);
		size_t siz_cmp = dispatch_data_get_size(cmp);
		test_long("readback size", siz_cmp, siz_out);
		const void *data_buf, *cmp_buf;
		dispatch_data_t data_map, cmp_map;
		data_map = dispatch_data_create_map(data, &data_buf, NULL);
		cmp_map = dispatch_data_create_map(cmp, &cmp_buf, NULL);
		test_long("readback memcmp",
				memcmp(data_buf, cmp_buf, MIN(siz_out, siz_cmp)), 0);
		dispatch_release(cmp_map);
		dispatch_release(data_map);
		dispatch_release(data);
		dispatch_group_leave(g);
	});
	test_group_wait(g);
	dispatch_release(g);
}

static void
test_writes_reads_eagain(void) // rdar://problem/8333366
{
	int in = open("/dev/random", O_RDONLY);
	if (in == -1) {
		test_errno("open", errno, 0);
		test_stop();
	}
	int fds[2], *fd = fds;
	if(pipe(fd) == -1) {
		test_errno("pipe", errno, 0);
		test_stop();
	}
	const size_t chunks = 320;
	const size_t siz_chunk = 32, siz = siz_chunk * chunks;

	dispatch_queue_t q = dispatch_get_global_queue(0,0);
	dispatch_group_t g = dispatch_group_create();
	__block size_t siz_acc = 0, deliveries = 0;
	__block void (^b)(dispatch_data_t, int);
	b = Block_copy(^(dispatch_data_t data, int err) {
		if (err) {
			test_errno("dispatch_read", err, 0);
			test_stop();
		}
		deliveries++;
		siz_acc += dispatch_data_get_size(data);
		if (siz_acc < siz) {
			dispatch_group_enter(g);
			dispatch_read(*fd, siz, q, b);
		}
		dispatch_group_leave(g);
	});
	dispatch_group_enter(g);
	dispatch_read(*fd, siz, q, b);
	char *buf[siz_chunk];
	size_t i;
	for (i = 0; i < chunks; i++) {
		ssize_t s = read(in, buf, siz_chunk);
		if (s < (ssize_t)siz_chunk) {
			test_errno("read", errno, 0);
			test_stop();
		}
		s = write(*(fd+1), buf, siz_chunk);
		if (s < (ssize_t)siz_chunk) {
			test_errno("write", errno, 0);
			test_stop();
		}
		usleep(10000);
	}
	close(in);
	close(*(fd+1));
	test_group_wait(g);
	test_long("dispatch_read deliveries", deliveries, chunks);
	test_long("dispatch_read data size", siz_acc, siz);
	close(*fd);
	Block_release(b);
	dispatch_release(g);
}

#endif // DISPATCHTEST_IO

int
main(void)
{
	dispatch_test_start("Dispatch IO Convenience Read/Write");

	dispatch_async(dispatch_get_main_queue(), ^{
#if DISPATCHTEST_IO
		test_read();
		test_read_write();
		test_read_writes();
		test_writes_reads_eagain();
#endif
		test_fin(NULL);
	});
	dispatch_main();
}
