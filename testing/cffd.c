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

#include <dispatch/dispatch.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include <CoreFoundation/CoreFoundation.h>

#include <bsdtests.h>
#include "dispatch_test.h"

static int long kevent_data = 0;

#if 0
int debug = 0;

#define DEBUG(...) do { \
		if (debug) fprintf(stderr, __VA_ARGS__); \
	} while(0);
#endif

#define assert_errno(str, expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s: %s\n", (str), strerror(errno)); \
		exit(1); \
	} } while(0);

int
init_kqueue(void)
{
	int kq;
	int res;
	struct kevent ke;
	static struct timespec t0;

	kq = kqueue();
	assert_errno("kqueue", kq >= 0);

	EV_SET(&ke, 1, EVFILT_TIMER, EV_ADD, NOTE_USECONDS, USEC_PER_SEC/10, 0);

	res = kevent(kq, &ke, 1, NULL, 0, &t0);
	assert_errno("kevent", res == 0);

	return kq;
}

int
read_kevent(int kq)
{
	int res;
	struct kevent ke;
	//static struct timespec t0;

	res = kevent(kq, NULL, 0, &ke, 1, NULL);
	assert_errno("kevent", res >= 0);

	kevent_data += ke.data;
	fprintf(stdout, "kevent.data = %ld\n", ke.data);

	return (res < 0);
}


static void
cffd_callback(CFFileDescriptorRef cffd,
	CFOptionFlags callBackTypes __attribute__((unused)),
	void *info __attribute__((unused)))
{
	int kq;

	kq = CFFileDescriptorGetNativeDescriptor(cffd);
	if (read_kevent(kq) == 0) {
		// ...
	}

	CFFileDescriptorEnableCallBacks(cffd, kCFFileDescriptorReadCallBack);
}

#if 0
void
timer()
{
	dispatch_source_t ds;
	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	assert(ds);
	dispatch_source_set_timer(ds, dispatch_time(0, 1*NSEC_PER_SEC), NSEC_PER_SEC, 0);
	dispatch_source_set_event_handler(ds, ^{
		printf("ping\n");
	});
	dispatch_resume(ds);
}

void
hangup()
{
	dispatch_source_t ds;
	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGHUP, 0, dispatch_get_main_queue());
	assert(ds);
	dispatch_source_set_event_handler(ds, ^{
		printf("hangup\n");
	});
	dispatch_resume(ds);
}
#endif

int
main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
	int kq;
	CFFileDescriptorRef cffd;
	CFRunLoopSourceRef  rls;
	CFFileDescriptorContext ctx;

	dispatch_test_start("CFFileDescriptor");

#if 0
	signal(SIGHUP, SIG_IGN);
#endif

	kq = init_kqueue();

	memset(&ctx, 0, sizeof(CFFileDescriptorContext));
	cffd = CFFileDescriptorCreate(NULL, kq, 1, cffd_callback, &ctx);
	assert(cffd);

	rls = CFFileDescriptorCreateRunLoopSource(NULL, cffd, 0);
	assert(rls);
	CFRunLoopAddSource(CFRunLoopGetCurrent(), rls, kCFRunLoopDefaultMode);
	CFFileDescriptorEnableCallBacks(cffd, kCFFileDescriptorReadCallBack);

#if 0
	timer();
	hangup();
#endif

	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.05, false);
	// Should fire at least 10 times ...
	test_long_greater_than_or_equal("kevent data", kevent_data, 10);

	test_stop();

	return 0;
}

