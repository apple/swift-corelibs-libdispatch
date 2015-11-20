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
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <spawn.h>
#include <signal.h>
#include <libkern/OSAtomic.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#define PID_CNT 5

static long event_cnt;

void
test_proc(pid_t bad_pid)
{
	dispatch_source_t proc_s[PID_CNT], proc;
	int res;
	pid_t pid, monitor_pid;

	event_cnt = 0;
	// Creates a process and register multiple observers.  Send a signal,
	// exit the process, etc., and verify all observers were notified.

	posix_spawnattr_t attr;
	res = posix_spawnattr_init(&attr);
	assert(res == 0);
	res = posix_spawnattr_setflags(&attr, POSIX_SPAWN_START_SUSPENDED);
	assert(res == 0);

	char* args[] = {
		"/bin/sleep", "2", NULL
	};

	res = posix_spawnp(&pid, args[0], NULL, &attr, args, NULL);
	if (res < 0) {
		perror(args[0]);
		exit(127);
	}

	res = posix_spawnattr_destroy(&attr);
	assert(res == 0);

	dispatch_group_t group = dispatch_group_create();

	assert(pid > 0);
	monitor_pid = bad_pid ? bad_pid : pid; // rdar://problem/8090801

	int i;
	for (i = 0; i < PID_CNT; ++i) {
		dispatch_group_enter(group);
		proc = proc_s[i] = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC,
				monitor_pid, DISPATCH_PROC_EXIT, dispatch_get_global_queue(0, 0));
		test_ptr_notnull("dispatch_source_proc_create", proc);
		dispatch_source_set_event_handler(proc, ^{
			long flags = dispatch_source_get_data(proc);
			test_long("DISPATCH_PROC_EXIT", flags, DISPATCH_PROC_EXIT);
			event_cnt++;
			dispatch_source_cancel(proc);
		});
		dispatch_source_set_cancel_handler(proc, ^{
			dispatch_group_leave(group);
		});
		dispatch_resume(proc);
	}
	kill(pid, SIGCONT);
	if (dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 10*NSEC_PER_SEC))) {
		for (i = 0; i < PID_CNT; ++i) {
			dispatch_source_cancel(proc_s[i]);
		}
		dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
	}
	for (i = 0; i < PID_CNT; ++i) {
		dispatch_release(proc_s[i]);
	}
	dispatch_release(group);
	// delay 5 seconds to give a chance for any bugs that
	// result in too many events to be noticed
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5*NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		int status;
		int res2 = waitpid(pid, &status, 0);
		assert(res2 != -1);
		//int passed = (WIFEXITED(status) && WEXITSTATUS(status) == 0);
		test_long("Sub-process exited", WEXITSTATUS(status) | WTERMSIG(status), 0);
		test_long("Event count", event_cnt, PID_CNT);
		if (bad_pid) {
			test_stop();
		} else {
			dispatch_async(dispatch_get_main_queue(), ^{
				test_proc(pid);
			});
		}
	});
}

int
main(void)
{
	dispatch_test_start("Dispatch Proc");
	test_proc(0);
	dispatch_main();

	return 0;
}
