/*
 * Copyright (c) 2008-2009 Apple Inc. All rights reserved.
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

/*
 * IMPORTANT: This header file describes INTERNAL interfaces to libdispatch
 * which are subject to change in future releases of Mac OS X. Any applications
 * relying on these interfaces WILL break.
 */

#ifndef __DISPATCH_OS_SHIMS__
#define __DISPATCH_OS_SHIMS__

#include <pthread.h>
#ifdef HAVE_PTHREAD_MACHDEP_H
#include <pthread_machdep.h>
#endif
#ifdef HAVE_PTHREAD_WORKQUEUES
#include <pthread_workqueue.h>
#endif
#ifdef HAVE_PTHREAD_NP_H
#include <pthread_np.h>
#endif

#ifdef USE_APPLE_CRASHREPORTER_INFO
__private_extern__ const char *__crashreporter_info__;
#endif

#ifdef HAVE_PTHREAD_KEY_INIT_NP
static const unsigned long dispatch_queue_key = __PTK_LIBDISPATCH_KEY0;
static const unsigned long dispatch_sema4_key = __PTK_LIBDISPATCH_KEY1;
static const unsigned long dispatch_cache_key = __PTK_LIBDISPATCH_KEY2;
static const unsigned long dispatch_bcounter_key = __PTK_LIBDISPATCH_KEY3;
//__PTK_LIBDISPATCH_KEY4
//__PTK_LIBDISPATCH_KEY5
#else
pthread_key_t dispatch_queue_key;
pthread_key_t dispatch_sema4_key;
pthread_key_t dispatch_cache_key;
pthread_key_t dispatch_bcounter_key;
#endif

#ifdef USE_APPLE_TSD_OPTIMIZATIONS
#define SIMULATE_5491082 1
#ifndef _PTHREAD_TSD_OFFSET
#define _PTHREAD_TSD_OFFSET 0
#endif

static inline void
_dispatch_thread_setspecific(unsigned long k, void *v)
{
#if defined(SIMULATE_5491082) && defined(__i386__)
	asm("movl %1, %%gs:%0" : "=m" (*(void **)(k * sizeof(void *) + _PTHREAD_TSD_OFFSET)) : "ri" (v) : "memory");
#elif defined(SIMULATE_5491082) && defined(__x86_64__)
	asm("movq %1, %%gs:%0" : "=m" (*(void **)(k * sizeof(void *) + _PTHREAD_TSD_OFFSET)) : "rn" (v) : "memory");
#else
	int res;
	if (_pthread_has_direct_tsd()) {
		res = _pthread_setspecific_direct(k, v);
	} else {
		res = pthread_setspecific(k, v);
	}
	dispatch_assert_zero(res);
#endif
}

static inline void *
_dispatch_thread_getspecific(unsigned long k)
{
#if defined(SIMULATE_5491082) && (defined(__i386__) || defined(__x86_64__))
	void *rval;
	asm("mov %%gs:%1, %0" : "=r" (rval) : "m" (*(void **)(k * sizeof(void *) + _PTHREAD_TSD_OFFSET)));
	return rval;
#else
	if (_pthread_has_direct_tsd()) {
		return _pthread_getspecific_direct(k);
	} else {
		return pthread_getspecific(k);
	}
#endif
}

#else /* !USE_APPLE_TSD_OPTIMIZATIONS */

static inline void
_dispatch_thread_setspecific(pthread_key_t k, void *v)
{
	int res;

	res = pthread_setspecific(k, v);
	dispatch_assert_zero(res);
}

static inline void *
_dispatch_thread_getspecific(pthread_key_t k)
{

	return pthread_getspecific(k);
}
#endif /* USE_APPLE_TSD_OPTIMIZATIONS */

#ifdef HAVE_PTHREAD_KEY_INIT_NP
static inline void
_dispatch_thread_key_init_np(unsigned long k, void (*d)(void *))
{
	dispatch_assert_zero(pthread_key_init_np((int)k, d));
}
#else
static inline void
_dispatch_thread_key_create(pthread_key_t *key, void (*destructor)(void *))
{

	dispatch_assert_zero(pthread_key_create(key, destructor));
}
#endif

#define _dispatch_thread_self pthread_self


#if DISPATCH_PERF_MON

#if defined (USE_APPLE_TSD_OPTIMIZATIONS) && defined(SIMULATE_5491082) && (defined(__i386__) || defined(__x86_64__))
#ifdef __LP64__
#define _dispatch_workitem_inc()	asm("incq %%gs:%0" : "+m"	\
		(*(void **)(dispatch_bcounter_key * sizeof(void *) + _PTHREAD_TSD_OFFSET)) :: "cc")
#define _dispatch_workitem_dec()	asm("decq %%gs:%0" : "+m"	\
		(*(void **)(dispatch_bcounter_key * sizeof(void *) + _PTHREAD_TSD_OFFSET)) :: "cc")
#else
#define _dispatch_workitem_inc()	asm("incl %%gs:%0" : "+m"	\
		(*(void **)(dispatch_bcounter_key * sizeof(void *) + _PTHREAD_TSD_OFFSET)) :: "cc")
#define _dispatch_workitem_dec()	asm("decl %%gs:%0" : "+m"	\
		(*(void **)(dispatch_bcounter_key * sizeof(void *) + _PTHREAD_TSD_OFFSET)) :: "cc")
#endif
#else /* !USE_APPLE_TSD_OPTIMIZATIONS */
static inline void
_dispatch_workitem_inc(void)
{
	unsigned long cnt = (unsigned long)_dispatch_thread_getspecific(dispatch_bcounter_key);
	_dispatch_thread_setspecific(dispatch_bcounter_key, (void *)++cnt);
}
static inline void
_dispatch_workitem_dec(void)
{
	unsigned long cnt = (unsigned long)_dispatch_thread_getspecific(dispatch_bcounter_key);
	_dispatch_thread_setspecific(dispatch_bcounter_key, (void *)--cnt);
}
#endif /* USE_APPLE_TSD_OPTIMIZATIONS */

// C99 doesn't define flsll() or ffsll()
#ifdef __LP64__
#define flsll(x) flsl(x)
#else
static inline unsigned int
flsll(uint64_t val)
{
	union {
		struct {
#ifdef __BIG_ENDIAN__
			unsigned int hi, low;
#else
			unsigned int low, hi;
#endif
		} words;
		uint64_t word;
	} _bucket = {
		.word = val,
	};
	if (_bucket.words.hi) {
		return fls(_bucket.words.hi) + 32;
	}
	return fls(_bucket.words.low);
}
#endif

#else
#define _dispatch_workitem_inc()
#define _dispatch_workitem_dec()
#endif	// DISPATCH_PERF_MON

static inline uint64_t
_dispatch_absolute_time(void)
{
#ifndef HAVE_MACH_ABSOLUTE_TIME
	struct timespec ts;
	int ret;

	ret = clock_gettime(CLOCK_UPTIME, &ts);
	dispatch_assume_zero(ret);

	/* XXXRW: Some kind of overflow detection needed? */
	return (ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec);
#else
	return mach_absolute_time();
#endif
}

#endif
