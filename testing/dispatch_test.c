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

#ifdef __OBJC_GC__
#include <objc/objc-auto.h>
#endif

#include <unistd.h>
#include <sys/event.h>
#include <assert.h>

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
