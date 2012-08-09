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
#include <unistd.h>
#include <sys/event.h>
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
	int kq = kqueue();
	assert(kq != -1);
	struct kevent ke = {
		.ident = fd,
		.filter = EVFILT_READ,
		.flags = EV_ADD|EV_ENABLE,
	};
	struct timespec t = {
		.tv_sec = 1,
	};
	int r = kevent(kq, &ke, 1, &ke, 1, &t);
	close(kq);
	return r > 0;
}

void
_dispatch_test_current(const char* file, long line, const char* desc, dispatch_queue_t expected)
{
	dispatch_queue_t actual = dispatch_get_current_queue();
	_test_ptr(file, line, desc, actual, expected);
}
