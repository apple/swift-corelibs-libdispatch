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

#include <fcntl.h>
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
#include <Windows.h>
#include <bcrypt.h>
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
dispatch_test_check_evfilt_read_for_fd(dispatch_fd_t fd)
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
	HANDLE handle = (HANDLE)fd;
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
#elif defined(__unix__) || defined(_WIN32)
	// Depending on /usr/bin/vi being present is unreliable (especially on
	// Android), so fill up a large-enough temp file with random bytes

#if defined(_WIN32)
	char temp_dir_buf[MAX_PATH];
	const char *temp_dir = getenv("TEMP") ?: getenv("TMP");
	if (!temp_dir) {
		DWORD len = GetTempPathA(sizeof(temp_dir_buf), temp_dir_buf);
		if (len > 0 && len < sizeof(temp_dir_buf)) {
			temp_dir = temp_dir_buf;
		} else {
			temp_dir = ".";
		}
	}
#else
	const char *temp_dir = getenv("TMPDIR");
	if (temp_dir == NULL || temp_dir[0] == '\0') {
		temp_dir = "/tmp";
	}
#endif

	const char *const suffix = "/dispatch_test.XXXXXX";
	size_t temp_dir_len = strlen(temp_dir);
	size_t suffix_len = strlen(suffix);
	char *path = malloc(temp_dir_len + suffix_len + 1);
	assert(path != NULL);
	memcpy(path, temp_dir, temp_dir_len);
	memcpy(&path[temp_dir_len], suffix, suffix_len + 1);
	dispatch_fd_t temp_fd = mkstemp(path);
	if (temp_fd == -1) {
		perror("mkstemp");
		exit(EXIT_FAILURE);
	}

	const size_t file_size = 2 * 1024 * 1024;
	char *file_buf = malloc(file_size);
	assert(file_buf != NULL);

	ssize_t num;
#if defined(_WIN32)
	NTSTATUS status = BCryptGenRandom(NULL, (PUCHAR)file_buf, file_size,
			BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (status < 0) {
		fprintf(stderr, "BCryptGenRandom failed with %ld\n", status);
		dispatch_test_release_large_file(path);
		exit(EXIT_FAILURE);
	}
#else
	int urandom_fd = open("/dev/urandom", O_RDONLY);
	if (urandom_fd == -1) {
		perror("/dev/urandom");
		dispatch_test_release_large_file(path);
		exit(EXIT_FAILURE);
	}
	size_t pos = 0;
	while (pos < file_size) {
		num = read(urandom_fd, &file_buf[pos], file_size - pos);
		if (num > 0) {
			pos += (size_t)num;
		} else if (num == -1 && errno != EINTR) {
			perror("read");
			dispatch_test_release_large_file(path);
			exit(EXIT_FAILURE);
		}
	}
	close(urandom_fd);
#endif

	do {
		num = dispatch_test_fd_write(temp_fd, file_buf, file_size);
	} while (num == -1 && errno == EINTR);
	if (num == -1) {
		perror("write");
		dispatch_test_release_large_file(path);
		exit(EXIT_FAILURE);
	}
	assert(num == file_size);
	dispatch_test_fd_close(temp_fd);
	free(file_buf);
	return path;
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
#elif defined(__unix__) || defined(_WIN32)
	if (unlink(path) < 0) {
		perror("unlink");
	}
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

dispatch_fd_t
dispatch_test_fd_open(const char *path, int flags)
{
#if defined(_WIN32)
	DWORD desired_access = 0;
	DWORD creation_disposition = OPEN_EXISTING;
	switch (flags & (O_RDONLY | O_WRONLY | O_RDWR)) {
		case O_RDONLY:
			desired_access = GENERIC_READ;
			break;
		case O_WRONLY:
			desired_access = GENERIC_WRITE;
			break;
		case O_RDWR:
			desired_access = GENERIC_READ | GENERIC_WRITE;
			break;
	}
	if (flags & O_CREAT) {
		creation_disposition = OPEN_ALWAYS;
		if (flags & O_EXCL) {
			creation_disposition = CREATE_NEW;
		}
	}
	// FILE_SHARE_DELETE is important here because tests must be able to delete
	// temporary files after opening them
	HANDLE handle = CreateFileA(path, desired_access,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			/* lpSecurityAttributes */ NULL, creation_disposition,
			/* dwFlagsAndAttributes */ 0, /* hTemplateFile */ NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		switch (error) {
			case ERROR_ACCESS_DENIED:
				errno = EACCES;
				break;
			case ERROR_FILE_EXISTS:
				errno = EEXIST;
				break;
			case ERROR_FILE_NOT_FOUND:
			case ERROR_PATH_NOT_FOUND:
				errno = ENOENT;
				break;
			default:
				print_winapi_error("CreateFileA", GetLastError());
				errno = EIO;
				break;
		}
		return -1;
	}
	return (dispatch_fd_t)handle;
#else
	return open(path, flags);
#endif
}

int
dispatch_test_fd_close(dispatch_fd_t fd)
{
#if defined(_WIN32)
	if (!CloseHandle((HANDLE)fd)) {
		errno = EBADF;
		return -1;
	}
	return 0;
#else
	return close(fd);
#endif
}

off_t
dispatch_test_fd_lseek(dispatch_fd_t fd, off_t offset, int whence)
{
#if defined(_WIN32)
	DWORD method;
	switch (whence) {
		case SEEK_CUR:
			method = FILE_CURRENT;
			break;
		case SEEK_END:
			method = FILE_END;
			break;
        case SEEK_SET:
        default:
            method = FILE_BEGIN;
            break;
	}
	LARGE_INTEGER distance = {.QuadPart = offset};
	LARGE_INTEGER new_pos;
	if (!SetFilePointerEx((HANDLE)fd, distance, &new_pos, method)) {
		print_winapi_error("SetFilePointerEx", GetLastError());
		errno = EINVAL;
		return -1;
	}
	return (off_t)new_pos.QuadPart;
#else
	return lseek(fd, offset, whence);
#endif
}

ssize_t
dispatch_test_fd_pread(dispatch_fd_t fd, void *buf, size_t count, off_t offset)
{
#if defined(_WIN32)
	OVERLAPPED overlapped;
	memset(&overlapped, 0, sizeof(overlapped));
	LARGE_INTEGER lioffset = {.QuadPart = offset};
	overlapped.Offset = lioffset.LowPart;
	overlapped.OffsetHigh = lioffset.HighPart;
	DWORD num_read;
	if (!ReadFile((HANDLE)fd, buf, count, &num_read, &overlapped)) {
		print_winapi_error("ReadFile", GetLastError());
		errno = EIO;
		return -1;
	}
	return (ssize_t)num_read;
#else
	return pread(fd, buf, count, offset);
#endif
}

ssize_t
dispatch_test_fd_read(dispatch_fd_t fd, void *buf, size_t count)
{
#if defined(_WIN32)
	if (GetFileType((HANDLE)fd) == FILE_TYPE_PIPE) {
		OVERLAPPED ov = {0};
		DWORD num_read;
		BOOL success = ReadFile((HANDLE)fd, buf, count, &num_read, &ov);
		if (!success && GetLastError() == ERROR_IO_PENDING) {
			success = GetOverlappedResult((HANDLE)fd, &ov, &num_read,
					/* bWait */ TRUE);
		}
		if (!success) {
			print_winapi_error("ReadFile", GetLastError());
			errno = EIO;
			return -1;
		}
		return (ssize_t)num_read;
	}
	DWORD num_read;
	if (!ReadFile((HANDLE)fd, buf, count, &num_read, NULL)) {
		print_winapi_error("ReadFile", GetLastError());
		errno = EIO;
		return -1;
	}
	return (ssize_t)num_read;
#else
	return read(fd, buf, count);
#endif
}

ssize_t
dispatch_test_fd_write(dispatch_fd_t fd, const void *buf, size_t count)
{
#if defined(_WIN32)
	if (GetFileType((HANDLE)fd) == FILE_TYPE_PIPE) {
		OVERLAPPED ov = {0};
		DWORD num_written;
		BOOL success = WriteFile((HANDLE)fd, buf, count, &num_written, &ov);
		if (!success && GetLastError() == ERROR_IO_PENDING) {
			success = GetOverlappedResult((HANDLE)fd, &ov, &num_written,
					/* bWait */ TRUE);
		}
		if (!success) {
			print_winapi_error("WriteFile", GetLastError());
			errno = EIO;
			return -1;
		}
		return (ssize_t)num_written;
	}
	DWORD num_written;
	if (!WriteFile((HANDLE)fd, buf, count, &num_written, NULL)) {
		print_winapi_error("WriteFile", GetLastError());
		errno = EIO;
		return -1;
	}
	return (ssize_t)num_written;
#else
	return write(fd, buf, count);
#endif
}
