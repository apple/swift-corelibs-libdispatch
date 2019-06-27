/*
 * Copyright (c) 2019 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	 http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <unistd.h>
#endif

#include <dispatch/dispatch.h>

#include <bsdtests.h>
#include "dispatch_test.h"

enum {
	DISPATCH_PIPE_KIND_ANONYMOUS,
#if defined(_WIN32)
	DISPATCH_PIPE_KIND_NAMED_INBOUND,
	DISPATCH_PIPE_KIND_NAMED_OUTBOUND,
	DISPATCH_PIPE_KIND_NAMED_INBOUND_OVERLAPPED,
	DISPATCH_PIPE_KIND_NAMED_OUTBOUND_OVERLAPPED,
#endif
	DISPATCH_PIPE_KIND_COUNT,
};

enum {
	DISPATCH_TEST_IMMEDIATE,
	DISPATCH_TEST_DELAYED,
};

static const char *const pipe_names[] = {
	[DISPATCH_PIPE_KIND_ANONYMOUS] = "anonymous",
#if defined(_WIN32)
	[DISPATCH_PIPE_KIND_NAMED_INBOUND] = "named, inbound",
	[DISPATCH_PIPE_KIND_NAMED_OUTBOUND] = "named, outbound",
	[DISPATCH_PIPE_KIND_NAMED_INBOUND_OVERLAPPED] = "named, inbound, overlapped",
	[DISPATCH_PIPE_KIND_NAMED_OUTBOUND_OVERLAPPED] = "named, outbound, overlapped",
#endif
};

static const char *const delay_names[] = {
	[DISPATCH_TEST_IMMEDIATE] = "Immediate",
	[DISPATCH_TEST_DELAYED] = "Delayed",
};

#if defined(_WIN32)
enum {
	NAMED_PIPE_BUFFER_SIZE = 0x1000,
};
#endif

static size_t
test_get_pipe_buffer_size(int kind)
{
#if defined(_WIN32)
	if (kind != DISPATCH_PIPE_KIND_ANONYMOUS) {
		return NAMED_PIPE_BUFFER_SIZE;
	}
	static dispatch_once_t once;
	static DWORD size;
	dispatch_once(&once, ^{
		HANDLE read_handle, write_handle;
		if (!CreatePipe(&read_handle, &write_handle, NULL, 0)) {
			test_long("CreatePipe", GetLastError(), ERROR_SUCCESS);
			test_stop();
		}
		GetNamedPipeInfo(write_handle, NULL, &size, NULL, NULL);
		CloseHandle(read_handle);
		CloseHandle(write_handle);
	});
	return size;
#else
	(void)kind;
	static dispatch_once_t once;
	static size_t size;
	dispatch_once(&once, ^{
		int fds[2];
		if (pipe(fds) < 0) {
			test_errno("pipe", errno, 0);
			test_stop();
		}
		fcntl(fds[1], F_SETFL, O_NONBLOCK);
		for (size = 0; write(fds[1], "", 1) > 0; size++) {}
		close(fds[0]);
		close(fds[1]);
	});
	return size;
#endif
}

#if defined(_WIN32)
static void
test_make_named_pipe(DWORD flags, dispatch_fd_t *readfd, dispatch_fd_t *writefd)
{
	wchar_t name[64];
	static int counter = 0;
	swprintf(name, sizeof(name), L"\\\\.\\pipe\\dispatch_io_pipe_%lu_%d",
			GetCurrentProcessId(), counter++);
	HANDLE server = CreateNamedPipeW(name,
			flags | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE,
			/* nMaxInstances */ 1, NAMED_PIPE_BUFFER_SIZE,
			NAMED_PIPE_BUFFER_SIZE, /* nDefaultTimeOut */ 0,
			/* lpSecurityAttributes */ NULL);
	if (server == INVALID_HANDLE_VALUE) {
		test_ptr_not("CreateNamedPipe", server, INVALID_HANDLE_VALUE);
		test_stop();
	}
	HANDLE client = CreateFileW(name,
			(flags & PIPE_ACCESS_INBOUND) ? GENERIC_WRITE : GENERIC_READ,
			/* dwShareMode */ 0, /* lpSecurityAttributes */ NULL, OPEN_EXISTING,
			flags & FILE_FLAG_OVERLAPPED, /* hTemplateFile */ NULL);
	if (client == INVALID_HANDLE_VALUE) {
		test_ptr_not("CreateFile", client, INVALID_HANDLE_VALUE);
		test_stop();
	}
	if (flags & PIPE_ACCESS_INBOUND) {
		*readfd = (dispatch_fd_t)server;
		*writefd = (dispatch_fd_t)client;
	} else {
		*readfd = (dispatch_fd_t)client;
		*writefd = (dispatch_fd_t)server;
	}
}
#endif

static void
test_make_pipe(int kind, dispatch_fd_t *readfd, dispatch_fd_t *writefd)
{
#if defined(_WIN32)
	switch (kind) {
	case DISPATCH_PIPE_KIND_ANONYMOUS:
		if (!CreatePipe((PHANDLE)readfd, (PHANDLE)writefd, NULL, 0)) {
			test_long("CreatePipe", GetLastError(), ERROR_SUCCESS);
			test_stop();
		}
		break;
	case DISPATCH_PIPE_KIND_NAMED_INBOUND:
		test_make_named_pipe(PIPE_ACCESS_INBOUND, readfd, writefd);
		break;
	case DISPATCH_PIPE_KIND_NAMED_OUTBOUND:
		test_make_named_pipe(PIPE_ACCESS_OUTBOUND, readfd, writefd);
		break;
	case DISPATCH_PIPE_KIND_NAMED_INBOUND_OVERLAPPED:
		test_make_named_pipe(PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, readfd,
				writefd);
		break;
	case DISPATCH_PIPE_KIND_NAMED_OUTBOUND_OVERLAPPED:
		test_make_named_pipe(PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
				readfd, writefd);
		break;
	}
#else
	(void)kind;
	int fds[2];
	if (pipe(fds) < 0) {
		test_errno("pipe", errno, 0);
		test_stop();
	}
	*readfd = fds[0];
	*writefd = fds[1];
#endif
}

static void
test_source_read(int kind, int delay)
{
	printf("\nSource Read %s: %s\n", delay_names[delay], pipe_names[kind]);

	dispatch_fd_t readfd, writefd;
	test_make_pipe(kind, &readfd, &writefd);

	dispatch_group_t g = dispatch_group_create();
	dispatch_group_enter(g);

	void (^write_block)(void) = ^{
		dispatch_group_enter(g);
		char buf[512] = {0};
		ssize_t n = dispatch_test_fd_write(writefd, buf, sizeof(buf));
		if (n < 0) {
			test_errno("write error", errno, 0);
			test_stop();
		}
		test_sizet("num written", (size_t)n, sizeof(buf));
		dispatch_group_leave(g);
	};
	if (delay == DISPATCH_TEST_IMMEDIATE) {
		write_block();
	}

	dispatch_source_t reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
			(uintptr_t)readfd, 0, dispatch_get_global_queue(0, 0));
	test_ptr_notnull("dispatch_source_create", reader);
	assert(reader);
	dispatch_source_set_event_handler(reader, ^{
		dispatch_group_enter(g);
		char buf[512];
		size_t available = dispatch_source_get_data(reader);
		test_sizet("num available", available, sizeof(buf));
		ssize_t n = dispatch_test_fd_read(readfd, buf, sizeof(buf));
		if (n >= 0) {
			test_sizet("num read", (size_t)n, sizeof(buf));
		} else {
			test_errno("read error", errno, 0);
		}
		dispatch_source_cancel(reader);
		dispatch_group_leave(g);
	});
	dispatch_source_set_cancel_handler(reader, ^{
		dispatch_release(reader);
		dispatch_group_leave(g);
	});
	dispatch_resume(reader);

	dispatch_source_t t = NULL;
	if (delay == DISPATCH_TEST_DELAYED) {
		t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
				dispatch_get_global_queue(0, 0));
		dispatch_source_set_event_handler(t, write_block);
		dispatch_source_set_timer(t,
				dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
				DISPATCH_TIME_FOREVER, 0);
		dispatch_resume(t);
	}

	test_group_wait(g);
	dispatch_release(g);
	if (t) {
		dispatch_source_cancel(t);
		dispatch_release(t);
	}
	dispatch_test_fd_close(readfd);
	dispatch_test_fd_close(writefd);
}

static void
test_source_write(int kind, int delay)
{
	printf("\nSource Write %s: %s\n", delay_names[delay], pipe_names[kind]);

	dispatch_fd_t readfd, writefd;
	test_make_pipe(kind, &readfd, &writefd);

	dispatch_group_t g = dispatch_group_create();
	dispatch_group_enter(g);

	const size_t bufsize = test_get_pipe_buffer_size(kind);

	void (^write_block)(void) = ^{
		char *buf = calloc(bufsize, 1);
		assert(buf);
		ssize_t nw = dispatch_test_fd_write(writefd, buf, bufsize);
		free(buf);
		if (nw < 0) {
			test_errno("write error", errno, 0);
			test_stop();
		}
		test_sizet("num written", (size_t)nw, bufsize);
	};
	write_block();

	void (^read_block)(void) = ^{
		dispatch_group_enter(g);
		char *buf = calloc(bufsize, 1);
		assert(buf);
		ssize_t nr = dispatch_test_fd_read(readfd, buf, bufsize);
		free(buf);
		if (nr < 0) {
			test_errno("read error", errno, 0);
			test_stop();
		}
		test_sizet("num read", (size_t)nr, bufsize);
		dispatch_group_leave(g);
	};
	if (delay == DISPATCH_TEST_IMMEDIATE) {
		read_block();
	}

	dispatch_source_t writer = dispatch_source_create(
			DISPATCH_SOURCE_TYPE_WRITE, (uintptr_t)writefd, 0,
			dispatch_get_global_queue(0, 0));
	test_ptr_notnull("dispatch_source_create", writer);
	assert(writer);
	dispatch_source_set_event_handler(writer, ^{
		dispatch_group_enter(g);
		size_t available = dispatch_source_get_data(writer);
		test_sizet_less_than("num available", 0, available);
		write_block();
		read_block();
		dispatch_source_cancel(writer);
		dispatch_group_leave(g);
	});
	dispatch_source_set_cancel_handler(writer, ^{
		dispatch_release(writer);
		dispatch_group_leave(g);
	});
	dispatch_resume(writer);

	dispatch_source_t t = NULL;
	if (delay == DISPATCH_TEST_DELAYED) {
		t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
				dispatch_get_global_queue(0, 0));
		dispatch_source_set_event_handler(t, read_block);
		dispatch_source_set_timer(t,
				dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
				DISPATCH_TIME_FOREVER, 0);
		dispatch_resume(t);
	}

	test_group_wait(g);
	dispatch_release(g);
	if (t) {
		dispatch_source_cancel(t);
		dispatch_release(t);
	}
	dispatch_test_fd_close(readfd);
	dispatch_test_fd_close(writefd);
}

static void
test_dispatch_read(int kind, int delay)
{
	printf("\nDispatch Read %s: %s\n", delay_names[delay], pipe_names[kind]);

	dispatch_fd_t readfd, writefd;
	test_make_pipe(kind, &readfd, &writefd);

	dispatch_group_t g = dispatch_group_create();
	dispatch_group_enter(g);

	char writebuf[512] = {0};
	char *writebufp = writebuf;
	void (^write_block)(void) = ^{
		dispatch_group_enter(g);
		ssize_t n =
			dispatch_test_fd_write(writefd, writebufp, sizeof(writebuf));
		if (n < 0) {
			test_errno("write error", errno, 0);
			test_stop();
		}
		test_sizet("num written", (size_t)n, sizeof(writebuf));
		dispatch_group_leave(g);
	};
	if (delay == DISPATCH_TEST_IMMEDIATE) {
		write_block();
	}

	dispatch_read(readfd, sizeof(writebuf), dispatch_get_global_queue(0, 0),
			^(dispatch_data_t data, int err) {
		test_errno("read error", err, 0);
		test_sizet("num read", dispatch_data_get_size(data), sizeof(writebuf));
		dispatch_group_leave(g);
	});

	dispatch_source_t t = NULL;
	if (delay == DISPATCH_TEST_DELAYED) {
		t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
				dispatch_get_global_queue(0, 0));
		dispatch_source_set_event_handler(t, write_block);
		dispatch_source_set_timer(t,
				dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
				DISPATCH_TIME_FOREVER, 0);
		dispatch_resume(t);
	}

	test_group_wait(g);
	dispatch_release(g);
	if (t) {
		dispatch_source_cancel(t);
		dispatch_release(t);
	}
	dispatch_test_fd_close(readfd);
	dispatch_test_fd_close(writefd);
}

static void
test_dispatch_write(int kind, int delay)
{
	printf("\nDispatch Write %s: %s\n", delay_names[delay], pipe_names[kind]);

	dispatch_fd_t readfd, writefd;
	test_make_pipe(kind, &readfd, &writefd);

	dispatch_group_t g = dispatch_group_create();
	dispatch_group_enter(g);

	const size_t bufsize = test_get_pipe_buffer_size(kind);

	char *buf = calloc(bufsize, 1);
	assert(buf);
	ssize_t nw = dispatch_test_fd_write(writefd, buf, bufsize);
	free(buf);
	if (nw < 0) {
		test_errno("write error", errno, 0);
		test_stop();
	}
	test_sizet("num written", (size_t)nw, bufsize);

	void (^read_block)(void) = ^{
		dispatch_group_enter(g);
		char *readbuf = calloc(bufsize, 1);
		assert(readbuf);
		ssize_t nr = dispatch_test_fd_read(readfd, readbuf, bufsize);
		free(readbuf);
		if (nr < 0) {
			test_errno("read error", errno, 0);
			test_stop();
		}
		test_sizet("num read", (size_t)nr, bufsize);
		dispatch_group_leave(g);
	};
	if (delay == DISPATCH_TEST_IMMEDIATE) {
		read_block();
	}

	buf = calloc(bufsize, 1);
	assert(buf);
	dispatch_data_t wd = dispatch_data_create(buf, bufsize,
			dispatch_get_global_queue(0, 0), DISPATCH_DATA_DESTRUCTOR_FREE);
	dispatch_write(writefd, wd, dispatch_get_global_queue(0, 0),
			^(dispatch_data_t data, int err) {
		test_errno("write error", err, 0);
		test_ptr_null("data written", data);
		read_block();
		dispatch_group_leave(g);
	});

	dispatch_source_t t = NULL;
	if (delay == DISPATCH_TEST_DELAYED) {
		t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
				dispatch_get_global_queue(0, 0));
		dispatch_source_set_event_handler(t, read_block);
		dispatch_source_set_timer(t,
				dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
				DISPATCH_TIME_FOREVER, 0);
		dispatch_resume(t);
	}

	test_group_wait(g);
	dispatch_release(g);
	dispatch_release(wd);
	if (t) {
		dispatch_source_cancel(t);
		dispatch_release(t);
	}
	dispatch_test_fd_close(readfd);
	dispatch_test_fd_close(writefd);
}

int
main(void)
{
	dispatch_test_start("Dispatch IO Pipe");
	dispatch_async(dispatch_get_main_queue(), ^{
		for (int kind = 0; kind < DISPATCH_PIPE_KIND_COUNT; kind++) {
			test_source_read(kind, DISPATCH_TEST_IMMEDIATE);
			test_source_read(kind, DISPATCH_TEST_DELAYED);
			test_source_write(kind, DISPATCH_TEST_IMMEDIATE);
			test_source_write(kind, DISPATCH_TEST_DELAYED);
			test_dispatch_read(kind, DISPATCH_TEST_IMMEDIATE);
			test_dispatch_read(kind, DISPATCH_TEST_DELAYED);
			test_dispatch_write(kind, DISPATCH_TEST_IMMEDIATE);
			test_dispatch_write(kind, DISPATCH_TEST_DELAYED);
		}
		test_stop();
	});
	dispatch_main();
}
