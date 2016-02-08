/*
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

#ifndef __DISPATCH_SHIMS_PTHREAD_MAIN_NP__
#define __DISPATCH_SHIMS_PTHREAD_MAIN_NP__

#if !HAVE_PTHREAD_MAIN_NP

#if __linux__
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#endif

static inline int
pthread_main_np()
{
#if __linux__
	return syscall(SYS_gettid) == getpid() ? 1 : 0;
#else
#error "No suported way to determine if the current thread is the main thread."
#endif
}
#endif /* !HAVE_PTHREAD_MAIN_NP */
#endif /* __DISPATCH_SHIMS_PTHREAD_MAIN_NP__ */
