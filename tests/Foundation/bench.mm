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

#include <Foundation/Foundation.h>
#include <libkern/OSAtomic.h>
#ifdef __ANDROID__
#include <linux/sysctl.h>
#else
#include <sys/sysctl.h>
#endif /* __ANDROID__ */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>
#ifdef __BLOCKS__
#include <Block.h>
#endif
#include <dispatch/dispatch.h>
#include <dispatch/private.h>

//#define BENCH_SLOW 1

extern "C" {
__private_extern__ void func(void);
#ifdef __BLOCKS__
__private_extern__ void (^block)(void);
#endif
static void backflip(void *ctxt);
static void backflip_done(void);
}

@interface BasicObject : NSObject
{
}
- (void) method;
@end

@implementation BasicObject
- (void) method
{
}
@end

class BasicClass {
public:
	virtual void virtfunc(void) {
	};
};

static void *
force_a_thread(void *arg)
{
	pause();
	abort();
	return arg;
}

static volatile int32_t global;
static volatile int64_t w_global;

#if TARGET_OS_EMBEDDED
static const size_t cnt = 5000000;
#else
static const size_t cnt = 200000000;
#endif
static const size_t cnt2 = cnt/100;

static uint64_t bfs;
static long double loop_cost;
static long double cycles_per_nanosecond;
static mach_timebase_info_data_t tbi;

static void __attribute__((noinline))
print_result(uint64_t s, const char *str)
{
	uint64_t d, e = mach_absolute_time();
	long double dd;

	d = e - s;

	if (tbi.numer != tbi.denom) {
		d *= tbi.numer;
		d /= tbi.denom;
	}

	dd = (typeof(dd))d / (typeof(dd))cnt;

	dd -= loop_cost;

	if (loop_cost == 0.0) {
		loop_cost = dd;
	}

	dd *= cycles_per_nanosecond;
	dd = roundl(dd * 200.0)/200.0;

	printf("%-45s%15.3Lf cycles\n", str, dd);
}

#if BENCH_SLOW || !TARGET_OS_EMBEDDED
static void __attribute__((noinline))
print_result2(uint64_t s, const char *str)
{
	uint64_t d, e = mach_absolute_time();
	long double dd;

	d = e - s;

	if (tbi.numer != tbi.denom) {
		d *= tbi.numer;
		d /= tbi.denom;
	}

	dd = (typeof(dd))d / (typeof(dd))cnt2;

	dd -= loop_cost;
	dd *= cycles_per_nanosecond;

	printf("%-45s%15.3Lf cycles\n", str, dd);
}
#endif

#if defined(__i386__) || defined(__x86_64__)
static inline uint64_t
rdtsc(void)
{
	uint32_t lo, hi;

	asm volatile("rdtsc" : "=a" (lo), "=d" (hi));

	return (uint64_t)hi << 32 | lo;
}
#endif

static struct fml {
	struct fml *fml_next;
} *fixed_malloc_lifo_head;

struct fml *fixed_malloc_lifo(void);// __attribute__((noinline));
void fixed_free_lifo(struct fml *fml);// __attribute__((noinline));

struct fml *
fixed_malloc_lifo(void)
{
	struct fml *fml_r = fixed_malloc_lifo_head;

	if (fml_r) {
		fixed_malloc_lifo_head = fml_r->fml_next;
		return fml_r;
	} else {
		return (struct fml *)malloc(32);
	}
}

void
fixed_free_lifo(struct fml *fml)
{
	fml->fml_next = fixed_malloc_lifo_head;
	fixed_malloc_lifo_head = fml;
}

int
main(void)
{
	pthread_mutex_t plock = PTHREAD_MUTEX_INITIALIZER;
	OSSpinLock slock = OS_SPINLOCK_INIT;
	BasicObject *bo;
	BasicClass *bc;
	pthread_t pthr_pause;
	dispatch_queue_t q, mq;
	kern_return_t kr;
#if BENCH_SLOW
	semaphore_t sem;
#endif
	uint64_t freq;
	uint64_t s;
	size_t freq_len = sizeof(freq);
	size_t bf_cnt = cnt;
	unsigned i;
	int r;

	printf("\n====================================================================\n");
	printf("[TEST] dispatch benchmark\n");
	printf("[PID] %d\n", getpid());
	printf("====================================================================\n\n");

	r = sysctlbyname("hw.cpufrequency", &freq, &freq_len, NULL, 0);
	assert(r != -1);
	assert(freq_len == sizeof(freq));

	cycles_per_nanosecond = (long double)freq / (long double)NSEC_PER_SEC;

#if BENCH_SLOW
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	assert(pool);
#endif

	/* Malloc has different logic for threaded apps. */
	r = pthread_create(&pthr_pause, NULL, force_a_thread, NULL);
	assert(r == 0);

	kr = mach_timebase_info(&tbi);
	assert(kr == 0);
#if defined(__i386__) || defined(__x86_64__)
	assert(tbi.numer == tbi.denom); /* This will fail on PowerPC. */
#endif

	bo = [[BasicObject alloc] init];
	assert(bo);

	bc = new BasicClass();
	assert(bc);

	q = dispatch_queue_create("com.apple.bench-dispatch", NULL);
	assert(q);

	mq = dispatch_get_main_queue();
	assert(mq);

	printf("%-45s%15Lf\n\n", "Cycles per nanosecond:", cycles_per_nanosecond);

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		asm volatile("");
	}
	print_result(s, "Empty loop:");

	printf("\nLoop cost subtracted from the following:\n\n");

#if TARGET_OS_EMBEDDED
	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		mach_absolute_time();
	}
	print_result(s, "mach_absolute_time():");
#else
	s = mach_absolute_time();
	for (i = cnt2; i; i--) {
		mach_absolute_time();
	}
	print_result2(s, "mach_absolute_time():");
#endif

#if defined(__i386__) || defined(__x86_64__)
	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		rdtsc();
	}
	print_result(s, "rdtsc():");
#endif

#if BENCH_SLOW
	s = mach_absolute_time();
	for (i = cnt2; i; i--) {
		pthread_t pthr;
		void *pr;

		r = pthread_create(&pthr, NULL, (void *(*)(void *))func, NULL);
		assert(r == 0);
		r = pthread_join(pthr, &pr);
		assert(r == 0);
	}
	print_result2(s, "pthread create+join:");

	s = mach_absolute_time();
	for (i = cnt2; i; i--) {
		kr = semaphore_create(mach_task_self(), &sem, SYNC_POLICY_FIFO, 0);
		assert(kr == 0);
		kr = semaphore_destroy(mach_task_self(), sem);
		assert(kr == 0);
	}
	print_result2(s, "Mach semaphore create/destroy:");

	kr = semaphore_create(mach_task_self(), &sem, SYNC_POLICY_FIFO, 0);
	assert(kr == 0);
	s = mach_absolute_time();
	for (i = cnt2; i; i--) {
		kr = semaphore_signal(sem);
		assert(kr == 0);
	}
	print_result2(s, "Mach semaphore signal:");
	kr = semaphore_destroy(mach_task_self(), sem);
	assert(kr == 0);

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		free(malloc(32));
	}
	print_result(s, "free(malloc(32)):");

	s = mach_absolute_time();
	for (i = cnt / 2; i; i--) {
		void *m1 = malloc(32);
		void *m2 = malloc(32);
		free(m1);
		free(m2);
	}
	print_result(s, "Avoiding the MRU cache of free(malloc(32)):");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		fixed_free_lifo(fixed_malloc_lifo());
	}
	print_result(s, "per-thread/fixed free(malloc(32)):");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		assert(strtoull("18446744073709551615", NULL, 0) == ~0ull);
	}
	print_result(s, "strtoull(\"18446744073709551615\") == ~0ull:");
#endif

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		func();
	}
	print_result(s, "Empty function call:");

#ifdef __BLOCKS__
	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		block();
	}
	print_result(s, "Empty block call:");
#endif

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		bc->virtfunc();
	}
	print_result(s, "Empty C++ virtual call:");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		[bo method];
	}
	print_result(s, "Empty ObjC call:");

#if BENCH_SLOW
	s = mach_absolute_time();
	for (i = cnt2; i; i--) {
		[bo description];
	}
	print_result2(s, "\"description\" ObjC call:");

	[pool release];

	pool = NULL;
#endif

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		asm("nop");
	}
	print_result(s, "raw 'nop':");

#if defined(__i386__) || defined(__x86_64__)
	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		asm("pause");
	}
	print_result(s, "raw 'pause':");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		asm("mfence");
	}
	print_result(s, "Atomic mfence:");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		asm("lfence");
	}
	print_result(s, "Atomic lfence:");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		asm("sfence");
	}
	print_result(s, "Atomic sfence:");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		uint64_t sidt_rval;
		asm("sidt %0" : "=m" (sidt_rval));
	}
	print_result(s, "'sidt' instruction:");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		long prev;
		asm volatile("cmpxchg %1,%2"
				: "=a" (prev) : "r" (0l), "m" (global), "0" (1l));
	}
	print_result(s, "'cmpxchg' without the 'lock' prefix:");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		global = 0;
		asm volatile("mfence" ::: "memory");
	}
	print_result(s, "Store + mfence:");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		unsigned long _clbr;
#ifdef __LP64__
		asm volatile("cpuid" : "=a" (_clbr)
				: "0" (0) : "rbx", "rcx", "rdx", "cc", "memory");
#else
#ifdef __llvm__
		asm volatile("cpuid" : "=a" (_clbr) : "0" (0)
				: "ebx", "ecx", "edx", "cc", "memory" );
#else // gcc does not allow inline i386 asm to clobber ebx
		asm volatile("pushl %%ebx\n\tcpuid\n\tpopl %%ebx"
				: "=a" (_clbr) : "0" (0) : "ecx", "edx", "cc", "memory" );
#endif
#endif
	}
	print_result(s, "'cpuid' instruction:");

#elif defined(__arm__)

#include <arm/arch.h>

#if !defined(_ARM_ARCH_7) && defined(__thumb__)
#error "GCD requires instructions unvailable in ARMv6 Thumb1"
#endif

#ifdef _ARM_ARCH_7
	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		asm("yield");
	}
	print_result(s, "raw 'yield':");
#endif

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
#ifdef _ARM_ARCH_7
		asm volatile("dmb ish" : : : "memory");
#else
		asm volatile("mcr	p15, 0, %0, c7, c10, 5" : : "r" (0) : "memory");
#endif
	}
	print_result(s, "'dmb ish' instruction:");

#ifdef _ARM_ARCH_7
	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		asm volatile("dmb ishst" : : : "memory");
	}
	print_result(s, "'dmb ishst' instruction:");
#endif

#ifdef _ARM_ARCH_7
	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		asm volatile("str	%[_r], [%[_p], %[_o]]" :
				: [_p] "p" (&global), [_o] "M" (0), [_r] "r" (0) : "memory");
		asm volatile("dmb ishst" : : : "memory");
	}
	print_result(s, "'str + dmb ishst' instructions:");
#endif

#ifdef _ARM_ARCH_7
	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		uintptr_t prev;
		uint32_t t;
		do {
		asm volatile("ldrex	%[_r], [%[_p], %[_o]]"
				: [_r] "=&r" (prev) \
				: [_p] "p" (&global), [_o] "M" (0) : "memory");
		asm volatile("strex	%[_t], %[_r], [%[_p], %[_o]]"
				: [_t] "=&r" (t) \
				: [_p] "p" (&global), [_o] "M" (0), [_r] "r" (0) : "memory");
		} while (t);
	}
	print_result(s, "'ldrex + strex' instructions:");
#endif

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
#ifdef _ARM_ARCH_7
		asm volatile("dsb ish" : : : "memory");
#else
		asm volatile("mcr	p15, 0, %0, c7, c10, 4" : : "r" (0) : "memory");
#endif
	}
	print_result(s, "'dsb ish' instruction:");

#if BENCH_SLOW
	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		register long _swtch_pri asm("ip") = -59;
		asm volatile("svc	0x80" : : "r" (_swtch_pri) : "r0", "memory");
	}
	print_result(s, "swtch_pri syscall:");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		register long _r0 asm("r0") = 0, _r1 asm("r1") = 1, _r2 asm("r2") = 1;
		register long _thread_switch asm("ip") = -61;
		asm volatile("svc	0x80" : "+r" (_r0)
				: "r" (_r1), "r" (_r2), "r" (_thread_switch): "memory");
	}
	print_result(s, "thread_switch syscall:");
#endif

#endif // __arm__

#if BENCH_SLOW
	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		pthread_yield_np();
	}
	print_result(s, "pthread_yield_np():");

	s = mach_absolute_time();
	for (i = cnt2; i; i--) {
		usleep(0);
	}
	print_result2(s, "usleep(0):");
#endif

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		__sync_lock_test_and_set(&global, 0);
	}
	print_result(s, "Atomic xchg:");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		__sync_val_compare_and_swap(&global, 1, 0);
	}
	print_result(s, "Atomic cmpxchg:");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		__sync_fetch_and_add(&global, 1);
	}
	print_result(s, "Atomic increment:");

	{
		global = 0;
		volatile int32_t *g = &global;

		s = mach_absolute_time();
		for (i = cnt; i; i--) {
			uint32_t result;
			__sync_and_and_fetch(g, 1);
			result = *g;
			if (result) {
				abort();
			}
		}
		print_result(s, "Atomic and-and-fetch, reloading result:");
	}

	{
		global = 0;
		volatile int32_t *g = &global;

		s = mach_absolute_time();
		for (i = cnt; i; i--) {
			uint32_t result;
			result = __sync_and_and_fetch(g, 1);
			if (result) {
				abort();
			}
		}
		print_result(s, "Atomic and-and-fetch, using result:");
	}

	global = 0;

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		OSAtomicIncrement32Barrier(&global);
	}
	print_result(s, "OSAtomicIncrement32Barrier:");

	global = 0;

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		OSAtomicIncrement32(&global);
	}
	print_result(s, "OSAtomicIncrement32:");

	w_global = 0;

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		OSAtomicIncrement64Barrier(&w_global);
	}
	print_result(s, "OSAtomicIncrement64Barrier:");

	w_global = 0;

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		OSAtomicIncrement64(&w_global);
	}
	print_result(s, "OSAtomicIncrement64:");

	global = 0;

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		while (!__sync_bool_compare_and_swap(&global, 0, 1)) {
			do {
#if defined(__i386__) || defined(__x86_64__)
				asm("pause");
#elif defined(__arm__) && defined _ARM_ARCH_7
				asm("yield");
#endif
			} while (global);
		}
		global = 0;
	}
	print_result(s, "Inlined spin lock/unlock:");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		OSSpinLockLock(&slock);
		OSSpinLockUnlock(&slock);
	}
	print_result(s, "OSSpinLock/Unlock:");

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		r = pthread_mutex_lock(&plock);
		assert(r == 0);
		r = pthread_mutex_unlock(&plock);
		assert(r == 0);
	}
	print_result(s, "pthread lock/unlock:");

#ifdef __BLOCKS__
	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		dispatch_sync(q, ^{ });
	}
	print_result(s, "dispatch_sync:");
#endif

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		dispatch_sync_f(q, NULL, (void (*)(void *))func);
	}
	print_result(s, "dispatch_sync_f:");

#ifdef __BLOCKS__
	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		dispatch_barrier_sync(q, ^{ });
	}
	print_result(s, "dispatch_barrier_sync:");
#endif

	s = mach_absolute_time();
	for (i = cnt; i; i--) {
		dispatch_barrier_sync_f(q, NULL, (void (*)(void *))func);
	}
	print_result(s, "dispatch_barrier_sync_f:");

	s = mach_absolute_time();
	dispatch_apply_f(cnt,
			dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
			NULL, (void (*)(void *, size_t))func);
	s += loop_cost; // cancel out the implicit subtraction done by the next line
	print_result(s, "dispatch_apply_f():");

	// do a "double backflip" to hit the fast-path of the enqueue/dequeue logic
	bfs = mach_absolute_time();
	dispatch_async_f(dispatch_get_main_queue(), &bf_cnt, backflip);
	dispatch_async_f(dispatch_get_main_queue(), &bf_cnt, backflip);

	dispatch_main();
}

__attribute__((noinline))
void
backflip_done(void)
{
	print_result(bfs, "dispatch_async_f():");
	exit(EXIT_SUCCESS);
}

void
backflip(void *ctxt)
{
	size_t *bf_cnt = (size_t *)ctxt;
	if (--(*bf_cnt)) {
		return dispatch_async_f(dispatch_get_main_queue(), ctxt, backflip);
	}
	backflip_done();
}
