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

#include <sys/cdefs.h>
#include <stdbool.h>
#include <dispatch/dispatch.h>

#define test_group_wait(g) do { \
	if (dispatch_group_wait(g, dispatch_time(DISPATCH_TIME_NOW, \
			25ull * NSEC_PER_SEC))) { \
		test_long("group wait timed out", 1, 0); \
		test_stop(); \
	} } while (0)

__BEGIN_DECLS

void dispatch_test_start(const char* desc);

bool dispatch_test_check_evfilt_read_for_fd(int fd);

void _dispatch_test_current(const char* file, long line, const char* desc, dispatch_queue_t expected);
#define dispatch_test_current(a,b) _dispatch_test_current(__SOURCE_FILE__, __LINE__, a, b)

__END_DECLS
