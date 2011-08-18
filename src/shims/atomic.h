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

/*
 * IMPORTANT: This header file describes INTERNAL interfaces to libdispatch
 * which are subject to change in future releases of Mac OS X. Any applications
 * relying on these interfaces WILL break.
 */

#ifndef __DISPATCH_SHIMS_ATOMIC__
#define __DISPATCH_SHIMS_ATOMIC__

/* x86 & cortex-a8 have a 64 byte cacheline */
#define DISPATCH_CACHELINE_SIZE 64
#define ROUND_UP_TO_CACHELINE_SIZE(x) \
		(((x) + (DISPATCH_CACHELINE_SIZE - 1)) & ~(DISPATCH_CACHELINE_SIZE - 1))
#define ROUND_UP_TO_VECTOR_SIZE(x) \
		(((x) + 15) & ~15)
#define DISPATCH_CACHELINE_ALIGN \
		__attribute__((__aligned__(DISPATCH_CACHELINE_SIZE)))

#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2)

#define _dispatch_atomic_barrier()	__sync_synchronize()
// see comment in dispatch_once.c
#define dispatch_atomic_maximally_synchronizing_barrier() \
		_dispatch_atomic_barrier()
// assume atomic builtins provide barriers
#define dispatch_atomic_barrier()
#define dispatch_atomic_acquire_barrier()
#define dispatch_atomic_release_barrier()
#define dispatch_atomic_store_barrier()

#define _dispatch_hardware_pause()	asm("")
#define _dispatch_debugger()		asm("trap")

#define dispatch_atomic_cmpxchg(p, e, n) \
		__sync_bool_compare_and_swap((p), (e), (n))
#if __has_builtin(__sync_swap)
#define dispatch_atomic_xchg(p, n) \
		((typeof(*(p)))__sync_swap((p), (n)))
#else
#define dispatch_atomic_xchg(p, n) \
		((typeof(*(p)))__sync_lock_test_and_set((p), (n)))
#endif
#define dispatch_atomic_add(p, v)	__sync_add_and_fetch((p), (v))
#define dispatch_atomic_sub(p, v)	__sync_sub_and_fetch((p), (v))
#define dispatch_atomic_or(p, v)	__sync_fetch_and_or((p), (v))
#define dispatch_atomic_and(p, v)	__sync_fetch_and_and((p), (v))

#define dispatch_atomic_inc(p)		dispatch_atomic_add((p), 1)
#define dispatch_atomic_dec(p)		dispatch_atomic_sub((p), 1)
// really just a low level abort()
#define _dispatch_hardware_crash()	__builtin_trap()

#define dispatch_atomic_cmpxchg2o(p, f, e, n) \
		dispatch_atomic_cmpxchg(&(p)->f, (e), (n))
#define dispatch_atomic_xchg2o(p, f, n) \
		dispatch_atomic_xchg(&(p)->f, (n))
#define dispatch_atomic_add2o(p, f, v) \
		dispatch_atomic_add(&(p)->f, (v))
#define dispatch_atomic_sub2o(p, f, v) \
		dispatch_atomic_sub(&(p)->f, (v))
#define dispatch_atomic_or2o(p, f, v) \
		dispatch_atomic_or(&(p)->f, (v))
#define dispatch_atomic_and2o(p, f, v) \
		dispatch_atomic_and(&(p)->f, (v))
#define dispatch_atomic_inc2o(p, f) \
		dispatch_atomic_add2o((p), f, 1)
#define dispatch_atomic_dec2o(p, f) \
		dispatch_atomic_sub2o((p), f, 1)

#else
#error "Please upgrade to GCC 4.2 or newer."
#endif

#if defined(__x86_64__) || defined(__i386__)

// GCC emits nothing for __sync_synchronize() on x86_64 & i386
#undef _dispatch_atomic_barrier
#define _dispatch_atomic_barrier() \
	__asm__ __volatile__( \
	"mfence" \
	: : : "memory")
#undef dispatch_atomic_maximally_synchronizing_barrier
#ifdef __LP64__
#define dispatch_atomic_maximally_synchronizing_barrier() \
	do { unsigned long _clbr; __asm__ __volatile__( \
	"cpuid" \
	: "=a" (_clbr) : "0" (0) : "rbx", "rcx", "rdx", "cc", "memory" \
	); } while(0)
#else
#ifdef __llvm__
#define dispatch_atomic_maximally_synchronizing_barrier() \
	do { unsigned long _clbr; __asm__ __volatile__( \
	"cpuid" \
	: "=a" (_clbr) : "0" (0) : "ebx", "ecx", "edx", "cc", "memory" \
	); } while(0)
#else // gcc does not allow inline i386 asm to clobber ebx
#define dispatch_atomic_maximally_synchronizing_barrier() \
	do { unsigned long _clbr; __asm__ __volatile__( \
	"pushl	%%ebx\n\t" \
	"cpuid\n\t" \
	"popl %%ebx" \
	: "=a" (_clbr) : "0" (0) : "ecx", "edx", "cc", "memory" \
	); } while(0)
#endif
#endif
#undef _dispatch_hardware_pause
#define _dispatch_hardware_pause() asm("pause")
#undef _dispatch_debugger
#define _dispatch_debugger() asm("int3")

#elif defined(__ppc__) || defined(__ppc64__)

// GCC emits "sync" for __sync_synchronize() on ppc & ppc64
#undef _dispatch_atomic_barrier
#ifdef __LP64__
#define _dispatch_atomic_barrier() \
	__asm__ __volatile__( \
	"isync\n\t" \
	"lwsync"
	: : : "memory")
#else
#define _dispatch_atomic_barrier() \
	__asm__ __volatile__( \
	"isync\n\t" \
	"eieio" \
	: : : "memory")
#endif
#undef dispatch_atomic_maximally_synchronizing_barrier
#define dispatch_atomic_maximally_synchronizing_barrier() \
	__asm__ __volatile__( \
	"sync" \
	: : : "memory")

#endif


#endif // __DISPATCH_SHIMS_ATOMIC__
