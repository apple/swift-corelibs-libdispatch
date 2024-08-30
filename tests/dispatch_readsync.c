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
#include <dispatch/private.h>
#include <stdlib.h>
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <unistd.h>
#ifdef __ANDROID__
#include <linux/sysctl.h>
#else
#if !defined(__linux__)
#include <sys/sysctl.h>
#endif
#endif /* __ANDROID__ */
#endif
#include <assert.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#define LAPS 10000
#define INTERVAL 100

#if TARGET_OS_EMBEDDED
#define BUSY 10000
#define NTHREADS 16
#else
#define BUSY 1000000
#define NTHREADS 64
#endif

static dispatch_group_t g;
static _Atomic(size_t) r_count, w_count, workers, readers, writers, crw, count, drain;

static void
writer(void *ctxt)
{
	size_t w = atomic_fetch_add(&writers, 1, memory_order_relaxed) + 1, *m = (size_t *)ctxt;
	if (w > *m) *m = w;

	usleep(10000);
	size_t busy = BUSY;
	while (busy--) if (readers) atomic_fetch_add(&crw, 1, memory_order_relaxed);

	if (atomic_fetch_sub(&w_count, 1, memory_order_relaxed) == 0) {
		if (r_count == 0) {
			dispatch_async(dispatch_get_main_queue(), ^{test_stop();});
		}
	}
	atomic_fetch_sub(&writers, 1, memory_order_relaxed)
	dispatch_group_leave(g);
}

static void
reader(void *ctxt)
{
	size_t r = atomic_fetch_add(&readers, 1, memory_order_relaxed) + 1, *m = (size_t *)ctxt;
	if (r > *m) *m = r;

	usleep(10000);
	size_t busy = BUSY;
	while (busy--) if (writers) atomic_fetch_add(&crw, 1, memory_order_relaxed);

	if (atomic_fetch_sub(&r_count, 1, memory_order_relaxed) - 1 == 0) {
		if (r_count == 0) {
			dispatch_async(dispatch_get_main_queue(), ^{test_stop();});
		}
	}
	atomic_fetch_sub(&readers, 1, memory_order_relaxed);
}

static void
test_readsync(dispatch_queue_t rq, dispatch_queue_t wq, size_t n)
{
	size_t i, max_readers = 0, max_writers = 0;
	size_t *mrs = calloc(n, sizeof(size_t)), *mr, *mw = &max_writers;

	r_count = LAPS * 2;
	w_count = LAPS / INTERVAL;
	workers = readers = writers = crw = count = 0;

	for (i = 0, mr = mrs; i < n; i++, mr++) {
		dispatch_group_async(g,
				dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH,
				DISPATCH_QUEUE_OVERCOMMIT), ^{
			atomic_fetch_add(&workers, 1, memory_order_relaxed);
			do {
				usleep(100000);
			} while (workers < n);
			for (;;) {
				size_t idx = atomic_fetch_add(&count, 1, memory_order_relaxed) + 1;
				if (idx > LAPS) break;
				dispatch_sync_f(rq, mr, reader);
				if (!(idx % INTERVAL)) {
					dispatch_group_enter(g);
					dispatch_barrier_async_f(wq, mw, writer);
				}
				dispatch_sync_f(rq, mr, reader);
				if (!(idx % (INTERVAL*10))) {
					// Let the queue drain
					atomic_fetch_sub(&drain, 1, memory_order_relaxed);
					usleep(10000);
					dispatch_barrier_sync(wq, ^{});
					atomic_fetch_sub(&drain, 1, memory_order_relaxed);
				} else while (drain) usleep(1000);
			}
		});
	}
	dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
	for (i = 0, mr = mrs; i < n; i++, mr++) {
		if (*mr > max_readers) max_readers = *mr;
	}
	free(mrs);

	test_sizet("max readers", max_readers, n);
	test_sizet("max writers", max_writers, 1);
	test_sizet("concurrent readers & writers", crw, 0);
}

int
main(void)
{
	dispatch_test_start("Dispatch Reader/Writer Queues");

	uint32_t activecpu, wq_max_threads;
#ifdef __linux__
	activecpu = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
	// don't want to parse /proc/sys/kernel/threads-max
	wq_max_threads = activecpu * NTHREADS + 2;
#elif defined(_WIN32)
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	activecpu = si.dwNumberOfProcessors;
	wq_max_threads = activecpu * NTHREADS + 2;
#else
	size_t s = sizeof(uint32_t);
	sysctlbyname("hw.activecpu", &activecpu, &s, NULL, 0);
	s = sizeof(uint32_t);
	sysctlbyname("kern.wq_max_threads", &wq_max_threads, &s, NULL, 0);
#endif

	// cap at wq_max_threads - one wq thread for dq - one wq thread for manager
	size_t n = MIN(activecpu * NTHREADS, wq_max_threads - 2);

	g = dispatch_group_create();
	dispatch_queue_attr_t qattr = NULL;
#if DISPATCH_API_VERSION >= 20100518 // rdar://problem/7790099
	qattr = DISPATCH_QUEUE_CONCURRENT;
#endif
	dispatch_queue_t dq = dispatch_queue_create("readsync", qattr);
	assert(dq);
	if (!qattr) {
		dispatch_queue_set_width(dq, LONG_MAX); // rdar://problem/7919264
		dispatch_barrier_sync(dq, ^{}); // wait for changes to take effect
	}
	test_readsync(dq, dq, n);

	dispatch_queue_t tq = dispatch_queue_create("writebarrierasync", qattr);
	assert(tq);
	if (!qattr) {
		dispatch_queue_set_width(tq, LONG_MAX);
	}
	dispatch_set_target_queue(dq, tq);
	dispatch_barrier_sync(tq, ^{}); // wait for changes to take effect
	test_readsync(dq, tq, n); // rdar://problem/8186485
	dispatch_release(tq);

	dispatch_release(dq);
	dispatch_release(g);
	dispatch_main();
}
