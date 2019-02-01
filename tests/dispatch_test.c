/*
 * Copyright (c) 2008-2011 Apple Inc. All rights reserved.
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

#include "dispatch_test.h"
#include "bsdtests.h"

#ifdef __OBJC_GC__
#include <objc/objc-auto.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <unistd.h>
#if __has_include(<sys/event.h>)
#define HAS_SYS_EVENT_H 1
#include <sys/event.h>
#else
#include <sys/poll.h>
#endif
#elif defined(_WIN32)
#include <io.h>
#include <Windows.h>
#endif
#include <assert.h>

#include <dispatch/dispatch.h>

void test_start(const char* desc);

void
dispatch_test_start(const char* desc)
{
#if defined(__OBJC_GC__) && MAC_OS_X_VERSION_MIN_REQUIRED < 1070
	objc_startCollectorThread();
#endif
	test_start(desc);
}

bool
dispatch_test_check_evfilt_read_for_fd(int fd)
{
#if HAS_SYS_EVENT_H
	int kq = kqueue();
	assert(kq != -1);
	struct kevent ke = {
		.ident = (uintptr_t)fd,
		.filter = EVFILT_READ,
		.flags = EV_ADD|EV_ENABLE,
	};
	struct timespec t = {
		.tv_sec = 1,
	};
	int r = kevent(kq, &ke, 1, &ke, 1, &t);
	close(kq);
	return r > 0;
#elif defined(_WIN32)
	HANDLE handle = (HANDLE)_get_osfhandle(fd);
	// A zero-distance move retrieves the file pointer
	LARGE_INTEGER currentPosition;
	LARGE_INTEGER distance = {.QuadPart = 0};
	if (!SetFilePointerEx(handle, distance, &currentPosition, FILE_CURRENT)) {
		return false;
	}
	// If we are not at the end, assume the file is readable
	LARGE_INTEGER fileSize;
	if (GetFileSizeEx(handle, &fileSize) == 0) {
		return false;
	}
	return currentPosition.QuadPart < fileSize.QuadPart;
#else
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN,
	};
	int rc;
	do {
		rc = poll(&pfd, 1, 0);
	} while (rc == -1 && errno == EINTR);
	assert(rc != -1);
	return rc == 1;
#endif
}

char *
dispatch_test_get_large_file(void)
{
#if defined(__APPLE__)
	return strdup("/usr/bin/vi");
#elif defined(__unix__)
	// Depending on /usr/bin/vi being present is unreliable (especially on
	// Android), so fill up a large-enough temp file with random bytes

	const char *temp_dir = getenv("TMPDIR");
	if (temp_dir == NULL || temp_dir[0] == '\0') {
		temp_dir = "/tmp";
	}
	const char *const suffix = "/dispatch_test.XXXXXX";
	size_t temp_dir_len = strlen(temp_dir);
	size_t suffix_len = strlen(suffix);
	char *path = malloc(temp_dir_len + suffix_len + 1);
	assert(path != NULL);
	memcpy(path, temp_dir, temp_dir_len);
	memcpy(&path[temp_dir_len], suffix, suffix_len + 1);
	int temp_fd = mkstemp(path);
	if (temp_fd == -1) {
		perror("mkstemp");
		exit(EXIT_FAILURE);
	}

	const size_t file_size = 2 * 1024 * 1024;
	char *file_buf = malloc(file_size);
	assert(file_buf != NULL);

	int urandom_fd = open("/dev/urandom", O_RDONLY);
	if (urandom_fd == -1) {
		perror("/dev/urandom");
		exit(EXIT_FAILURE);
	}
	ssize_t num;
	size_t pos = 0;
	while (pos < file_size) {
		num = read(urandom_fd, &file_buf[pos], file_size - pos);
		if (num > 0) {
			pos += (size_t)num;
		} else if (num == -1 && errno != EINTR) {
			perror("read");
			exit(EXIT_FAILURE);
		}
	}
	close(urandom_fd);

	do {
		num = write(temp_fd, file_buf, file_size);
	} while (num == -1 && errno == EINTR);
	if (num == -1) {
		perror("write");
		exit(EXIT_FAILURE);
	}
	assert(num == file_size);
	close(temp_fd);
	free(file_buf);
	return path;
#elif defined(_WIN32)
	// TODO
	fprintf(stderr, "dispatch_test_get_large_file() not implemented on Windows\n");
	abort();
#else
#error "dispatch_test_get_large_file not implemented on this platform"
#endif
}

void
dispatch_test_release_large_file(const char *path)
{
#if defined(__APPLE__)
	// The path is fixed to a system file - do nothing
	(void)path;
#elif defined(__unix__)
	unlink(path);
#elif defined(_WIN32)
	// TODO
#else
#error "dispatch_test_release_large_file not implemented on this platform"
#endif
}

void
_dispatch_test_current(const char* file, long line, const char* desc, dispatch_queue_t expected)
{
	dispatch_queue_t actual = dispatch_get_current_queue();
	_test_ptr(file, line, desc, actual, expected);
}
