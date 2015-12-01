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
#include <assert.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <mach/clock_types.h>
#include <mach-o/arch.h>
#include <sys/resource.h>
#include <sys/time.h>

#include <bsdtests.h>

extern char **environ;

int
main(int argc, char *argv[])
{
	dispatch_source_t tmp_ds;
	int res;
	pid_t pid = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: %s [...]\n", argv[0]);
		exit(1);
	}

	short spawnflags = POSIX_SPAWN_START_SUSPENDED;
#if TARGET_OS_EMBEDDED
	spawnflags |= POSIX_SPAWN_SETEXEC;
#endif

	posix_spawnattr_t attr;
	res = posix_spawnattr_init(&attr);
	assert(res == 0);
	res = posix_spawnattr_setflags(&attr, spawnflags);
	assert(res == 0);

	uint64_t to = 0;
	char *tos = getenv("BSDTEST_TIMEOUT");
	if (tos) {
		to = strtoul(tos, NULL, 0);
		to *= NSEC_PER_SEC;
	}

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

	struct timeval tv_start;
	gettimeofday(&tv_start, NULL);

	if (spawnflags & POSIX_SPAWN_SETEXEC) {
		pid = fork();
	}
	if (!pid) {
		res = posix_spawnp(&pid, newargv[0], NULL, &attr, newargv, environ);
		if (res) {
			errno = res;
			perror(newargv[0]);
			exit(EXIT_FAILURE);
		}
	}

	//fprintf(stderr, "pid = %d\n", pid);
	assert(pid > 0);

	dispatch_queue_t main_q = dispatch_get_main_queue();

	tmp_ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, pid, DISPATCH_PROC_EXIT, main_q);
	assert(tmp_ds);
	dispatch_source_set_event_handler(tmp_ds, ^{
		int status;
		struct rusage usage;
		struct timeval tv_stop, tv_wall;

		gettimeofday(&tv_stop, NULL);
		tv_wall.tv_sec = tv_stop.tv_sec - tv_start.tv_sec;
		tv_wall.tv_sec -= (tv_stop.tv_usec < tv_start.tv_usec);
		tv_wall.tv_usec = abs(tv_stop.tv_usec - tv_start.tv_usec);

		int res2 = wait4(pid, &status, 0, &usage);
		assert(res2 != -1);
		test_long("Process exited", (WIFEXITED(status) && WEXITSTATUS(status) && WEXITSTATUS(status) != 0xff) || WIFSIGNALED(status), 0);
		printf("[PERF]\twall time: %ld.%06d\n", tv_wall.tv_sec, tv_wall.tv_usec);
		printf("[PERF]\tuser time: %ld.%06d\n", usage.ru_utime.tv_sec, usage.ru_utime.tv_usec);
		printf("[PERF]\tsystem time: %ld.%06d\n", usage.ru_stime.tv_sec, usage.ru_stime.tv_usec);
		printf("[PERF]\tmax resident set size: %ld\n", usage.ru_maxrss);
		printf("[PERF]\tpage faults: %ld\n", usage.ru_majflt);
		printf("[PERF]\tswaps: %ld\n", usage.ru_nswap);
		printf("[PERF]\tvoluntary context switches: %ld\n", usage.ru_nvcsw);
		printf("[PERF]\tinvoluntary context switches: %ld\n", usage.ru_nivcsw);
		exit((WIFEXITED(status) && WEXITSTATUS(status)) || WIFSIGNALED(status));
	});
	dispatch_resume(tmp_ds);

	if (!to) {
#if TARGET_OS_EMBEDDED
		to = 180LL * NSEC_PER_SEC;
#else
		to = 90LL * NSEC_PER_SEC;
#endif
	}

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, to), main_q, ^{
		kill(pid, SIGKILL);
		fprintf(stderr, "Terminating unresponsive process (%0.1lfs)\n", (double)to / NSEC_PER_SEC);
	});

	signal(SIGINT, SIG_IGN);
	tmp_ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, main_q);
	assert(tmp_ds);
	dispatch_source_set_event_handler(tmp_ds, ^{
		fprintf(stderr, "Terminating process due to signal\n");
		kill(pid, SIGKILL);
	});
	dispatch_resume(tmp_ds);

	if (spawnflags & POSIX_SPAWN_SETEXEC) {
		usleep(USEC_PER_SEC/10);
	}
	kill(pid, SIGCONT);

	dispatch_main();

	return 0;
}
