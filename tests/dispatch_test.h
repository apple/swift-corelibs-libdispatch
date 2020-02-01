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

#include <stdbool.h>
#include <dispatch/dispatch.h>

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <generic_unix_port.h>
#elif defined(_WIN32)
#include <generic_win_port.h>
#endif

#define test_group_wait(g) do { \
	if (dispatch_group_wait(g, dispatch_time(DISPATCH_TIME_NOW, \
			25ull * NSEC_PER_SEC))) { \
		test_long("group wait timed out", 1, 0); \
		test_stop(); \
	} } while (0)

#if defined(__cplusplus)
extern "C" {
#endif

void dispatch_test_start(const char* desc);

bool dispatch_test_check_evfilt_read_for_fd(dispatch_fd_t fd);

char *dispatch_test_get_large_file(void);
void dispatch_test_release_large_file(const char *path);

void _dispatch_test_current(const char* file, long line, const char* desc, dispatch_queue_t expected);
#define dispatch_test_current(a,b) _dispatch_test_current(__SOURCE_FILE__, __LINE__, a, b)

#if __APPLE__
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp,
		size_t *newpl);
#endif

dispatch_fd_t dispatch_test_fd_open(const char *path, int flags);
int dispatch_test_fd_close(dispatch_fd_t fd);
off_t dispatch_test_fd_lseek(dispatch_fd_t fd, off_t offset, int whence);
ssize_t dispatch_test_fd_pread(dispatch_fd_t fd, void *buf, size_t count, off_t offset);
ssize_t dispatch_test_fd_read(dispatch_fd_t fd, void *buf, size_t count);
ssize_t dispatch_test_fd_write(dispatch_fd_t fd, const void *buf, size_t count);

#if defined(__cplusplus)
} /* extern "C" */
#endif
