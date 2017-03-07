/*
 * This source file is part of the Swift.org open source project
 *
 * Copyright (c) 2015 Apple Inc. and the Swift project authors
 *
 * Licensed under Apache License v2.0 with Runtime Library Exception
 *
 * See http://swift.org/LICENSE.txt for license information
 * See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
 *
 */

// CUtil License:

/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// membarrier definitionis taken from cutils/atomic-arm.h / cutils/atomic-x86.h 

#ifndef __DISPATCH__ANDROID__MEMBARRIER_SHIMS__INTERNAL
#define __DISPATCH__ANDROID__MEMBARRIER_SHIMS__INTERNAL

#include <machine/cpu-features.h>

extern inline void android_compiler_barrier(void)
{
    __asm__ __volatile__ ("" : : : "memory");
}
#if ANDROID_SMP == 0
extern inline void android_memory_barrier(void)
{
    android_compiler_barrier();
}
extern inline void android_memory_store_barrier(void)
{
    android_compiler_barrier();
}
#elif defined(__i386__) || defined(__x86_64__)
extern inline void android_memory_barrier(void)
{
    __asm__ __volatile__ ("mfence" : : : "memory");
}
extern inline void android_memory_store_barrier(void)
{
    android_compiler_barrier();
}
#elif defined(__arm__)
#	if defined(__ARM_HAVE_DMB)
extern inline void android_memory_barrier(void)
{
    __asm__ __volatile__ ("dmb" : : : "memory");
}
extern inline void android_memory_store_barrier(void)
{
    __asm__ __volatile__ ("dmb st" : : : "memory");
}
#	elif defined(__ARM_HAVE_LDREX_STREX)
extern inline void android_memory_barrier(void)
{
    __asm__ __volatile__ ("mcr p15, 0, %0, c7, c10, 5" : : "r" (0) : "memory");
}
extern inline void android_memory_store_barrier(void)
{
    android_memory_barrier();
}
#	else
extern inline void android_memory_barrier(void)
{
    typedef void (kuser_memory_barrier)(void);
    (*(kuser_memory_barrier *)0xffff0fa0)();
}
extern inline void android_memory_store_barrier(void)
{
    android_memory_barrier();
}
#	endif
#else
#error atomic operations are unsupported
#endif

#if ANDROID_SMP == 0
#define ANDROID_MEMBAR_FULL android_compiler_barrier
#else
#define ANDROID_MEMBAR_FULL android_memory_barrier
#endif

#if ANDROID_SMP == 0
#define ANDROID_MEMBAR_STORE android_compiler_barrier
#else
#define ANDROID_MEMBAR_STORE android_memory_store_barrier
#endif

#endif /* __DISPATCH__ANDROID__MEMBARRIER_SHIMS__INTERNAL */
