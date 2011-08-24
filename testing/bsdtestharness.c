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

#include <mach/clock_types.h>
#include <dispatch/dispatch.h>
#include <assert.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <mach-o/arch.h>

#include <bsdtests.h>

extern char **environ;

int
main(int argc, char *argv[])
{
	dispatch_source_t tmp_ds;
	int res;
	pid_t pid;

	if (argc < 2) {
		fprintf(stderr, "usage: %s [...]\n", argv[0]);
		exit(1);
	}

	posix_spawnattr_t attr;
	res = posix_spawnattr_init(&attr);
	assert(res == 0);
	res = posix_spawnattr_setflags(&attr, POSIX_SPAWN_START_SUSPENDED);
	assert(res == 0);

	char *arch = getenv("BSDTEST_ARCH");
	if (arch) {
		const NXArchInfo *ai = NXGetArchInfoFromName(arch);
		if (ai) {
			res = posix_spawnattr_setbinpref_np(&attr, 1, (cpu_type_t*)&ai->cputype, NULL);
			assert(res == 0);
		}
	}

	int i;
	char** newargv = calloc(argc, sizeof(void*));
	for (i = 1; i < argc; ++i) {
		newargv[i-1] = argv[i];
	}
	newargv[i-1] = NULL;

	res = posix_spawnp(&pid, newargv[0], NULL, &attr, newargv, environ);
	if (res) {
		errno = res;
		perror(newargv[0]);
		exit(EXIT_FAILURE);
	}
	//fprintf(stderr, "pid = %d\n", pid);
	assert(pid > 0);

	dispatch_queue_t main_q = dispatch_get_main_queue();

	tmp_ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, pid, DISPATCH_PROC_EXIT, main_q);
	assert(tmp_ds);
	dispatch_source_set_event_handler(tmp_ds, ^{
		int status;
		int res2 = waitpid(pid, &status, 0);
		assert(res2 != -1);
		test_long("Process exited", (WIFEXITED(status) && WEXITSTATUS(status) && WEXITSTATUS(status) != 0xff) || WIFSIGNALED(status), 0);
		exit((WIFEXITED(status) && WEXITSTATUS(status)) || WIFSIGNALED(status));
	});
	dispatch_resume(tmp_ds);

#if TARGET_OS_EMBEDDED
	// Give embedded platforms a little more time.
	uint64_t timeout = 300LL * NSEC_PER_SEC;
#else
	uint64_t timeout = 150LL * NSEC_PER_SEC;
#endif

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, timeout), main_q, ^{
		kill(pid, SIGKILL);
		fprintf(stderr, "Terminating unresponsive process (%0.1lfs)\n", (double)timeout/NSEC_PER_SEC);
	});

	signal(SIGINT, SIG_IGN);
	tmp_ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, main_q);
	assert(tmp_ds);
	dispatch_source_set_event_handler(tmp_ds, ^{
		fprintf(stderr, "Terminating process due to signal\n");
		kill(pid, SIGKILL);
	});
	dispatch_resume(tmp_ds);

	kill(pid, SIGCONT);

	dispatch_main();

	return 0;
}
