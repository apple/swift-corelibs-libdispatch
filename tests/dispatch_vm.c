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

#include <sys/event.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libkern/OSAtomic.h>
#endif
#include <assert.h>
#ifdef __ANDROID__
#include <linux/sysctl.h>
#else
#include <sys/sysctl.h>
#endif /* __ANDROID__ */
#include <stdarg.h>
#include <time.h>

#include <dispatch/dispatch.h>
#include <dispatch/private.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#if defined(DISPATCH_SOURCE_TYPE_VM) && defined(NOTE_VM_PRESSURE)

#if TARGET_OS_EMBEDDED
#define ALLOC_SIZE ((size_t)(1024*1024*1ul))	// 1MB
#define NOTIFICATIONS 1
#else
#define ALLOC_SIZE ((size_t)(1024*1024*20ul))	// 20MB
#define NOTIFICATIONS 2
#endif
#define pg2mb(p) ((p) * ALLOC_SIZE/(1024*1024))
#ifdef __LP64__
#define MAXMEM ((size_t)SIZE_MAX)
#else
#define MAXMEM ((size_t)(3200ul*1024*1024))		// 3200MB
#endif

static char **pages;
static volatile int32_t handler_call_count;
static volatile int32_t page_count;
static int32_t max_page_count;
static dispatch_source_t vm_source;
static dispatch_queue_t vm_queue;
static time_t initial;
static int interval = 16;

#define log_msg(msg, ...) \
do { \
	fprintf(stderr, "[%2ds] " msg, (int)(time(NULL) - initial), ##__VA_ARGS__);\
} while (0)

static bool
dispatch_test_check_evfilt_vm(void)
{
	int kq = kqueue();
	assert(kq != -1);
	struct kevent ke = {
		.filter = EVFILT_VM,
		.flags = EV_ADD|EV_ENABLE|EV_RECEIPT,
		.fflags = NOTE_VM_PRESSURE,
	};
	int r = kevent(kq, &ke, 1, &ke, 1, NULL);
	close(kq);
	return !(r > 0 && ke.flags & EV_ERROR && ke.data == ENOTSUP);
}

static void
cleanup(void)
{
	dispatch_source_cancel(vm_source);
	dispatch_release(vm_source);
	dispatch_release(vm_queue);

	int32_t pc = 0, i;
	for (i = 0; i < max_page_count; ++i) {
	   if (pages[i]) {
		   pc++;
		   free(pages[i]);
	   }
	}
	if (pc) {
		log_msg("Freed %ldMB\n", pg2mb(pc));
	}
	free(pages);
	test_stop();
}

int
main(void)
{
	dispatch_test_start("Dispatch VM Pressure test"); // rdar://problem/7000945
	if (!dispatch_test_check_evfilt_vm()) {
		test_skip("EVFILT_VM not supported");
		test_stop();
		return 0;
	}
	initial = time(NULL);
	uint64_t memsize;
#ifdef __linux__
	memsize = sysconf(_SC_PAGESIZE) * sysconf(_SC_PHYS_PAGES);
#else
	size_t s = sizeof(memsize);
	int rc = sysctlbyname("hw.memsize", &memsize, &s, NULL, 0);
	assert(rc == 0);
#endif
	max_page_count = MIN(memsize, MAXMEM) / ALLOC_SIZE;
	pages = calloc(max_page_count, sizeof(char*));

	vm_queue = dispatch_queue_create("VM Pressure", NULL);
	vm_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_VM, 0,
			DISPATCH_VM_PRESSURE, vm_queue);
	dispatch_source_set_event_handler(vm_source, ^{
		if (!page_count) {
			// Too much memory pressure already to start the test
			test_skip("Memory pressure at start of test");
			cleanup();
		}
		if (OSAtomicIncrement32Barrier(&handler_call_count) != NOTIFICATIONS) {
			log_msg("Ignoring vm pressure notification\n");
			interval = 1;
			return;
		}
		test_long("dispatch_source_get_data()",
				dispatch_source_get_data(vm_source), NOTE_VM_PRESSURE);
		int32_t i, pc = page_count + 1;
		for (i = 0; i < pc && pages[i]; ++i) {
				free(pages[i]);
				pages[i] = NULL;
		}
		log_msg("Freed %ldMB\n", pg2mb(i));
	});
	dispatch_resume(vm_source);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC),
			dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
		while (handler_call_count < NOTIFICATIONS &&
				page_count < max_page_count) {
			void *p = valloc(ALLOC_SIZE);
			if (!p) {
				break;
			}
			bzero(p, ALLOC_SIZE);
			pages[page_count] = p;
			if (!(OSAtomicIncrement32Barrier(&page_count) % interval)) {
				log_msg("Allocated %ldMB\n", pg2mb(page_count));
				usleep(200000);
			}
		}
		if (page_count % interval) {
			log_msg("Allocated %ldMB\n", pg2mb(page_count));
		}
		if (handler_call_count < NOTIFICATIONS) {
			// Cannot allocate enough memory for test (e.g. on 32 bit)
			test_skip("Cannot allocate enough memory for test");
			dispatch_async(vm_queue, ^{cleanup();});
			return;
		}
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
				vm_queue, ^{
			test_long_greater_than_or_equal("VM Pressure fired",
					handler_call_count, NOTIFICATIONS);
			test_long_less_than("VM Pressure stopped firing",
					handler_call_count, 4);
			cleanup();
		});
	});
	dispatch_main();
	return 0;
}
#else //DISPATCH_SOURCE_TYPE_VM
int
main(void)
{
	dispatch_test_start("Dispatch VM Pressure test"
			" - No DISPATCH_SOURCE_TYPE_VM");
	test_stop();
	return 0;
}
#endif //DISPATCH_SOURCE_TYPE_VM
