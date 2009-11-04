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

// for architectures that don't always return mach_absolute_time() in nanoseconds
#if !defined(__i386__) && !defined(__x86_64__) && defined(HAVE_MACH_ABSOLUTE_TIME)
static mach_timebase_info_data_t tbi;
static dispatch_once_t tbi_pred;

static void
_dispatch_convert_init(void *context __attribute__((unused)))
{
        dispatch_assume_zero(mach_timebase_info(&tbi));
}

uint64_t
_dispatch_convert_mach2nano(uint64_t val)
{
#ifdef __LP64__
        __uint128_t tmp;
#else
        long double tmp;
#endif

        dispatch_once_f(&tbi_pred, NULL, _dispatch_convert_init);

        tmp = val;
        tmp *= tbi.numer;
        tmp /= tbi.denom;

        return tmp;
}

uint64_t
_dispatch_convert_nano2mach(uint64_t val)
{
#ifdef __LP64__
        __uint128_t tmp;
#else
        long double tmp;
#endif

        dispatch_once_f(&tbi_pred, NULL, _dispatch_convert_init);

        tmp = val;
        tmp *= tbi.denom;
        tmp /= tbi.numer;

        return tmp;
}
#endif
