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

#include <dispatch/dispatch.h>
#include <dispatch/private.h>
#include <stdlib.h>
#include <stdio.h>
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <sys/types.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <linux/sysctl.h>
#else
#if !defined(__linux__)
#include <sys/sysctl.h>
#endif
#endif /* __ANDROID__ */
#endif

#include <bsdtests.h>
#include "dispatch_test.h"

static _Atomic(size_t) done, concur;
static int use_group_async;
static uint32_t activecpu;
static uint32_t min_acceptable_concurrency;

static dispatch_queue_t q;
static dispatch_group_t g, gw;

const size_t workers = 4;

static void
nop(void* ctxt __attribute__((unused)))
{
	return;
}

static void
work(void* ctxt __attribute__((unused)))
{
	usleep(1000);
	atomic_fetch_add(&done, 1, memory_order_relaxed);

	if (!use_group_async) dispatch_group_leave(gw);
}

static void
submit_work(void* ctxt)
{
	size_t c =atomic_fetch_add(&concur, 1, memory_order_relaxed) + 1, *m = (size_t *)ctxt, i;
	if (c > *m) *m = c;

	for (i = 0; i < workers; ++i) {
		if (use_group_async) {
			dispatch_group_async_f(gw, q, NULL, work);
		} else {
			dispatch_group_enter(gw);
			dispatch_async_f(q, NULL, work);
		}
	}

	usleep(10000);
	atomic_fetch_sub(&concur, 1, memory_order_relaxed);

	if (!use_group_async) dispatch_group_leave(g);
}

static void
test_concur_async(size_t n, size_t qw)
{
	size_t i, max_concur = 0, *mcs = calloc(n, sizeof(size_t)), *mc;
	done = concur = 0;

	dispatch_suspend(q);
	for (i = 0, mc = mcs; i < n; i++, mc++) {
		if (use_group_async) {
			dispatch_group_async_f(g, q, mc, submit_work);
		} else {
			dispatch_group_enter(g);
			dispatch_async_f(q, mc, submit_work);
		}
	}
	dispatch_resume(q);

	dispatch_group_wait(g, DISPATCH_TIME_FOREVER);

	if (qw > 1) {
		size_t concurrency = MIN(n * workers, qw);
		if (done > min_acceptable_concurrency) {
			test_sizet_less_than_or_equal("concurrently completed workers", done, concurrency);
		} else {
			test_sizet("concurrently completed workers", done, concurrency);
		}
	} else {
		test_sizet_less_than_or_equal("concurrently completed workers", done, 1);
	}

	for (i = 0, mc = mcs; i < n; i++, mc++) {
		if (*mc > max_concur) max_concur = *mc;
	}
	free(mcs);

	size_t expect = MIN(n, qw);
	if (max_concur > min_acceptable_concurrency) {
		test_sizet_less_than_or_equal("max submission concurrency", max_concur, expect);
	} else {
		test_sizet("max submission concurrency", max_concur, expect);
	}

	dispatch_group_wait(gw, DISPATCH_TIME_FOREVER);
	usleep(1000);
}

static void
sync_work(void* ctxt)
{
	size_t c = atomic_fetch_add(&concur, 1, memory_order_relaxed) + 1, *m = (size_t *)ctxt;
	if (c > *m) *m = c;

	usleep(10000);
	atomic_fetch_sub(&concur, 1, memory_order_relaxed);
}

static void
test_concur_sync(size_t n, size_t qw)
{
	size_t i, max_concur = 0, *mcs = calloc(n, sizeof(size_t)), *mc;
	concur = 0;

	for (i = 0, mc = mcs; i < n; i++, mc++) {
		dispatch_group_async(g,
				dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH,
				DISPATCH_QUEUE_OVERCOMMIT), ^{
			usleep(100000);
			dispatch_sync_f(q, mc, sync_work);
		});
	}

	dispatch_group_wait(g, DISPATCH_TIME_FOREVER);

	for (i = 0, mc = mcs; i < n; i++, mc++) {
		if (*mc > max_concur) max_concur = *mc;
	}
	free(mcs);

	size_t expect = qw == 1 ? 1 : n;
	if (max_concur > min_acceptable_concurrency) {
		test_sizet_less_than_or_equal("max sync concurrency", max_concur, expect);
	} else {
		test_sizet("max sync concurrency", max_concur, expect);
	}
}

static void
apply_work(void* ctxt, size_t i)
{
	size_t c = atomic_fetch_add(&concur, 1, memory_order_relaxed) + 1, *m = ((size_t *)ctxt) + i;
	if (c > *m) *m = c;

	usleep(100000);
	atomic_fetch_sub(&concur, 1, memory_order_relaxed);
}

static void
test_concur_apply(size_t n, size_t qw)
{
	size_t i, max_concur = 0, *mcs = calloc(n, sizeof(size_t)), *mc;
	concur = 0;

	dispatch_apply_f(n, q, mcs, apply_work);

	for (i = 0, mc = mcs; i < n; i++, mc++) {
		if (*mc > max_concur) max_concur = *mc;
	}
	free(mcs);

	size_t expect = MIN(n, qw);
	if (max_concur > min_acceptable_concurrency) {
		test_sizet_less_than_or_equal("max apply concurrency", max_concur, expect);
	} else {
		test_sizet("max apply concurrency", max_concur, expect);
	}
}

static dispatch_queue_t
create_queue(long width, dispatch_queue_t tq, long *qw, const char **ql)
{
	if (!width) {
		*qw = LONG_MAX;
		*ql = "global";
		return dispatch_get_global_queue(0, 0);
	};
	dispatch_queue_t queue;
	dispatch_queue_attr_t qattr = NULL;

	*qw = width;
	*ql = width < LONG_MAX ? ( width == 1 ? "serial": "wide" ) : "concurrent";
#if DISPATCH_API_VERSION >= 20100518 // <rdar://problem/7790099>
	qattr = width < LONG_MAX ? NULL : DISPATCH_QUEUE_CONCURRENT;
#endif
	queue = dispatch_queue_create(*ql, qattr);
	if (!qattr) {
		dispatch_queue_set_width(queue, width);
	}
	if (tq) {
		dispatch_set_target_queue(queue, tq);
	}
	if (!qattr || tq) {
		dispatch_barrier_sync_f(queue, NULL, nop); // wait for changes to take effect
	}
	return queue;
}

int
main(int argc __attribute__((unused)), char* argv[] __attribute__((unused)))
{
	dispatch_test_start("Dispatch Private Concurrent/Wide Queue"); // <rdar://problem/8049506&8169448&8186485>

#ifdef __linux__
	activecpu = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_WIN32)
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	activecpu = si.dwNumberOfProcessors;
#else
	size_t s = sizeof(activecpu);
	sysctlbyname("hw.activecpu", &activecpu, &s, NULL, 0);
#endif
	size_t n = activecpu / 2 > 1 ? activecpu / 2 : 1, w = activecpu * 2;
	min_acceptable_concurrency = (uint32_t)n;
	dispatch_queue_t tq, ttq;
	long qw, tqw, ttqw;
	const char *ql, *tql, *ttql;
	size_t qi, tqi, ttqi;
	long qws[] = {
		0, LONG_MAX, (long)w, 1, // 0 <=> global queue
	};

	g = dispatch_group_create();
	gw = dispatch_group_create();

	for (ttqi = 0; ttqi < sizeof(qws)/sizeof(*qws); ttqi++) {
		ttq = create_queue(qws[ttqi], NULL, &ttqw, &ttql);
		for (tqi = 0; tqi < sizeof(qws)/sizeof(*qws); tqi++) {
			if (!qws[tqi] && qws[ttqi]) continue;
			tq = create_queue(qws[tqi], ttq, &tqw, &tql);
			for (qi = 0; qi < sizeof(qws)/sizeof(*qws); qi++) {
				if (!qws[qi] && qws[tqi]) continue;
				q = create_queue(qws[qi], tq, &qw, &ql);
				for (use_group_async = 0; use_group_async < 2; use_group_async++) {
					fprintf(stdout, "Testing dispatch%s_async on "
							"queue hierarchy: %s -> %s -> %s\n",
							use_group_async ? "_group" : "", ql, tql, ttql);
					fflush(stdout);
					test_concur_async(n, (size_t)MIN(qw, MIN(tqw, ttqw)));
				}
				fprintf(stdout, "Testing dispatch_sync on "
						"queue hierarchy: %s -> %s -> %s\n", ql, tql, ttql);
				fflush(stdout);
				test_concur_sync(w, (size_t)MIN(qw, MIN(tqw, ttqw)));
				fprintf(stdout, "Testing dispatch_apply on "
						"queue hierarchy: %s -> %s -> %s\n", ql, tql, ttql);
				fflush(stdout);
				test_concur_apply(activecpu, (size_t)MIN(qw, MIN(tqw, ttqw)));
				dispatch_release(q);
			}
			dispatch_release(tq);
		}
		dispatch_release(ttq);
	}

	dispatch_release(g);
	dispatch_release(gw);

	test_stop();
	return 0;
}
