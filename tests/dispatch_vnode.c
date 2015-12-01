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

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <dispatch/dispatch.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#define ITERATIONS 1000
long iterations, notifications;

int
main(void)
{
	dispatch_test_start("Dispatch VNODE RENAME");

	char path1[] = "/tmp/dispatchtest_vnode.XXXXXX";
	char path2[] = "/tmp/dispatchtest_vnode.XXXXXX";
	int fd1 = mkstemp(path1);
	if (fd1 == -1) {
		test_errno("mkstemp", errno, 0);
		test_stop();
	}
	close(fd1);
	if (!mktemp(path2)) {
		test_errno("mktemp", errno, 0);
		test_stop();
	}
	char *currentName = path1;
	char *renameDest = path2;
	dispatch_semaphore_t renamedSemaphore = dispatch_semaphore_create(0);
	dispatch_group_t g = dispatch_group_create();

	while (iterations++ < ITERATIONS) {
		int fd = open(currentName, O_EVTONLY);
		if (fd == -1) {
			test_errno("open", errno, 0);
			test_stop();
		}

		dispatch_source_t monitorSource = dispatch_source_create(
				DISPATCH_SOURCE_TYPE_VNODE, fd, DISPATCH_VNODE_RENAME,
				dispatch_get_global_queue(0, 0));
		dispatch_source_set_event_handler(monitorSource, ^{
			dispatch_semaphore_signal(renamedSemaphore);
		});
		dispatch_source_set_cancel_handler(monitorSource, ^{
			close(fd);
		});
#if DISPATCH_API_VERSION >= 20100818 // <rdar://problem/7731284>
		dispatch_group_enter(g);
		dispatch_source_set_registration_handler(monitorSource, ^{
			dispatch_group_leave(g);
		});
#endif
		dispatch_resume(monitorSource);
		dispatch_group_wait(g, DISPATCH_TIME_FOREVER);

		if(rename(currentName, renameDest)) {
			test_errno("rename", errno, 0);
			test_stop();
		}
		char *tmp = currentName;
		currentName = renameDest;
		renameDest = tmp;

		if(!dispatch_semaphore_wait(renamedSemaphore,
				dispatch_time(DISPATCH_TIME_NOW, 10 * 1000 * NSEC_PER_USEC))) {
			fprintf(stderr, ".");
			notifications++;
			dispatch_source_cancel(monitorSource);
		} else {
			fprintf(stderr, "!");
			dispatch_source_cancel(monitorSource);
			dispatch_semaphore_signal(renamedSemaphore);
		}
		if (!(iterations % 80)) {
			fprintf(stderr, "\n");
		}
		dispatch_release(monitorSource);
	}
	fprintf(stderr, "\n");
	unlink(currentName);
	dispatch_release(g);
	dispatch_release(renamedSemaphore);
	test_long("VNODE RENAME notifications", notifications, ITERATIONS);
	test_stop();
	return 0;
}
