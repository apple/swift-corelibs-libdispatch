/*
 * Copyright (c) 2012-2013 Apple Inc. All rights reserved.
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

#ifndef __DISPATCH_SHIMS_ATOMIC_SFB__
#define __DISPATCH_SHIMS_ATOMIC_SFB__

#if defined(__x86_64__) || defined(__i386__)

// Returns UINT_MAX if all the bits in p were already set.
DISPATCH_ALWAYS_INLINE
static inline unsigned int
os_atomic_set_first_bit(unsigned long *p, unsigned int max)
{
	unsigned long val, bit;
	if (max > (sizeof(val) * 8)) {
		__asm__ (
				 "1: \n\t"
				 "mov	%[_p], %[_val] \n\t"
				 "not	%[_val] \n\t"
				 "bsf	%[_val], %[_bit] \n\t" /* val is 0 => set zf */
				 "jz	2f \n\t"
				 "lock \n\t"
				 "bts	%[_bit], %[_p] \n\t" /* cf = prev bit val */
				 "jc	1b \n\t" /* lost race, retry */
				 "jmp	3f \n\t"
				 "2: \n\t"
				 "mov	%[_all_ones], %[_bit]" "\n\t"
				 "3: \n\t"
				 : [_p] "=m" (*p), [_val] "=&r" (val), [_bit] "=&r" (bit)
				 : [_all_ones] "i" ((__typeof__(bit))UINT_MAX) : "memory", "cc");
	} else {
		__asm__ (
				 "1: \n\t"
				 "mov	%[_p], %[_val] \n\t"
				 "not	%[_val] \n\t"
				 "bsf	%[_val], %[_bit] \n\t" /* val is 0 => set zf */
				 "jz	2f \n\t"
				 "cmp	%[_max], %[_bit] \n\t"
				 "jg	2f \n\t"
				 "lock \n\t"
				 "bts	%[_bit], %[_p] \n\t" /* cf = prev bit val */
				 "jc	1b \n\t" /* lost race, retry */
				 "jmp	3f \n\t"
				 "2: \n\t"
				 "mov	%[_all_ones], %[_bit]" "\n\t"
				 "3: \n\t"
				 : [_p] "=m" (*p), [_val] "=&r" (val), [_bit] "=&r" (bit)
				 : [_all_ones] "i" ((__typeof__(bit))UINT_MAX),
				   [_max] "g" ((__typeof__(bit))max) : "memory", "cc");
	}
	return (unsigned int)bit;
}

#else

#if __clang__ && __clang_major__ < 5 // <rdar://problem/13833871>
#define __builtin_ffs(x) __builtin_ffs((unsigned int)(x))
#endif

DISPATCH_ALWAYS_INLINE
static inline unsigned int
os_atomic_set_first_bit(unsigned long *p, unsigned int max)
{
	unsigned int index;
	unsigned long b, b_masked;

	os_atomic_rmw_loop(p, b, b_masked, relaxed, {
		// ffs returns 1 + index, or 0 if none set
		index = (unsigned int)__builtin_ffsl((long)~b);
		if (unlikely(index == 0)) {
			os_atomic_rmw_loop_give_up(return UINT_MAX);
		}
		index--;
		if (unlikely(index > max)) {
			os_atomic_rmw_loop_give_up(return UINT_MAX);
		}
		b_masked = b | (1UL << index);
	});

	return index;
}

#endif

#endif // __DISPATCH_SHIMS_ATOMIC_SFB__
