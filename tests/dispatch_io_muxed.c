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

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#endif

#include <dispatch/dispatch.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#if !defined(_WIN32)
#define closesocket(x) close(x)
#endif

static void
test_file_muxed(void)
{
	printf("\nFile Muxed\n");

#if defined(_WIN32)
	const char *temp_dir = getenv("TMP");
	if (!temp_dir) {
		temp_dir = getenv("TEMP");
	}
	if (!temp_dir) {
		test_ptr_notnull("temporary directory", temp_dir);
		test_stop();
	}
	const char *path_separator = "\\";
#else
	const char *temp_dir = getenv("TMPDIR");
	if (!temp_dir) {
		temp_dir = "/tmp";
	}
	const char *path_separator = "/";
#endif
	char *path = NULL;
	(void)asprintf(&path, "%s%sdispatchtest_io.XXXXXX", temp_dir, path_separator);
	dispatch_fd_t fd = mkstemp(path);
	if (fd == -1) {
		test_errno("mkstemp", errno, 0);
		test_stop();
	}
	if (unlink(path) == -1) {
		test_errno("unlink", errno, 0);
		test_stop();
	}
#if defined(_WIN32)
	free(path);
#endif
	dispatch_test_fd_write(fd, "test", 4);
	dispatch_test_fd_lseek(fd, 0, SEEK_SET);

	dispatch_group_t g = dispatch_group_create();
	dispatch_group_enter(g);
	dispatch_group_enter(g);

	dispatch_source_t reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
			(uintptr_t)fd, 0, dispatch_get_global_queue(0, 0));
	test_ptr_notnull("dispatch_source_create", reader);
	assert(reader);
	dispatch_source_set_event_handler(reader, ^{
		dispatch_source_cancel(reader);
	});
	dispatch_source_set_cancel_handler(reader, ^{
		dispatch_release(reader);
		dispatch_group_leave(g);
	});

	dispatch_source_t writer = dispatch_source_create(
			DISPATCH_SOURCE_TYPE_WRITE, (uintptr_t)fd, 0,
			dispatch_get_global_queue(0, 0));
	test_ptr_notnull("dispatch_source_create", writer);
	assert(writer);
	dispatch_source_set_event_handler(writer, ^{
		dispatch_source_cancel(writer);
	});
	dispatch_source_set_cancel_handler(writer, ^{
		dispatch_release(writer);
		dispatch_group_leave(g);
	});

	dispatch_resume(reader);
	dispatch_resume(writer);

	test_group_wait(g);
	dispatch_release(g);
	dispatch_test_fd_close(fd);
}

static void
test_stream_muxed(dispatch_fd_t serverfd, dispatch_fd_t clientfd)
{
	dispatch_group_t g = dispatch_group_create();
	dispatch_group_enter(g);
	dispatch_group_enter(g);

	dispatch_source_t reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
			(uintptr_t)serverfd, 0, dispatch_get_global_queue(0, 0));
	test_ptr_notnull("dispatch_source_create", reader);
	assert(reader);
	dispatch_source_set_event_handler(reader, ^{
		dispatch_source_cancel(reader);
	});
	dispatch_source_set_cancel_handler(reader, ^{
		dispatch_release(reader);
		dispatch_group_leave(g);
	});

	dispatch_source_t writer = dispatch_source_create(
			DISPATCH_SOURCE_TYPE_WRITE, (uintptr_t)serverfd, 0,
			dispatch_get_global_queue(0, 0));
	test_ptr_notnull("dispatch_source_create", writer);
	assert(writer);
	dispatch_source_set_event_handler(writer, ^{
		dispatch_source_cancel(writer);
	});
	dispatch_source_set_cancel_handler(writer, ^{
		dispatch_release(writer);
		dispatch_group_leave(g);
	});

	dispatch_resume(reader);
	dispatch_resume(writer);

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
			dispatch_get_global_queue(0, 0), ^{
		dispatch_group_enter(g);
		char buf[512] = {0};
		ssize_t n = dispatch_test_fd_write(clientfd, buf, sizeof(buf));
		if (n < 0) {
			test_errno("write error", errno, 0);
			test_stop();
		}
		test_sizet("num written", (size_t)n, sizeof(buf));
		dispatch_group_leave(g);
	});

	test_group_wait(g);
	dispatch_release(g);
}

static void
test_socket_muxed(void)
{
	printf("\nSocket Muxed\n");

	int listenfd = -1, serverfd = -1, clientfd = -1;
	struct sockaddr_in addr;
	socklen_t addrlen;

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenfd == -1) {
		test_errno("socket()", errno, 0);
		test_stop();
	}
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	addrlen = sizeof(addr);
	if (bind(listenfd, (struct sockaddr *)&addr, addrlen) == -1) {
		test_errno("bind()", errno, 0);
		test_stop();
	}
	if (listen(listenfd, 3) == -1) {
		test_errno("listen()", errno, 0);
		test_stop();
	}
	if (getsockname(listenfd, (struct sockaddr *)&addr, &addrlen) == -1) {
		test_errno("getsockname()", errno, 0);
		test_stop();
	}

	clientfd = socket(AF_INET, SOCK_STREAM, 0);
	if (clientfd == -1) {
		test_errno("socket()", errno, 0);
		test_stop();
	}
	if (connect(clientfd, (struct sockaddr *)&addr, addrlen)) {
		test_errno("connect()", errno, 0);
		test_stop();
	}

	serverfd = accept(listenfd, (struct sockaddr *)&addr, &addrlen);
	if (serverfd == -1) {
		test_errno("accept()", errno, 0);
		test_stop();
	}

	test_stream_muxed((dispatch_fd_t)serverfd, (dispatch_fd_t)clientfd);

	closesocket(clientfd);
	closesocket(serverfd);
	closesocket(listenfd);
}

#if defined(_WIN32)
static void
test_pipe_muxed(void)
{
	printf("\nDuplex Pipe Muxed\n");

	wchar_t name[64];
	swprintf(name, sizeof(name), L"\\\\.\\pipe\\dispatch_io_muxed_%lu",
			GetCurrentProcessId());
	HANDLE server = CreateNamedPipeW(name,
			PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE,
			/* nMaxInstances */ 1, /* nOutBufferSize */ 0x1000,
			/* nInBufferSize */ 0x1000, /* nDefaultTimeOut */ 0,
			/* lpSecurityAttributes */ NULL);
	if (server == INVALID_HANDLE_VALUE) {
		test_ptr_not("CreateNamedPipe", server, INVALID_HANDLE_VALUE);
		test_stop();
	}
	HANDLE client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE,
			/* dwShareMode */ 0, /* lpSecurityAttributes */ NULL, OPEN_EXISTING,
			/* dwFlagsAndAttributes */ 0, /* hTemplateFile */ NULL);
	if (client == INVALID_HANDLE_VALUE) {
		test_ptr_not("CreateFile", client, INVALID_HANDLE_VALUE);
		test_stop();
	}

	test_stream_muxed((dispatch_fd_t)server, (dispatch_fd_t)client);

	CloseHandle(client);
	CloseHandle(server);
}
#endif

int
main(void)
{
	dispatch_test_start("Dispatch IO Muxed");
#if defined(_WIN32)
	WSADATA wsa;
	int err = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (err != 0) {
		fprintf(stderr, "WSAStartup failed with %d\n", err);
		test_stop();
	}
#endif
	dispatch_async(dispatch_get_main_queue(), ^{
		test_file_muxed();
		test_socket_muxed();
#if defined(_WIN32)
		test_pipe_muxed();
#endif
		test_stop();
	});
	dispatch_main();
}
