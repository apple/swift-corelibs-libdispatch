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

// forward declarations for functions we are stubbing out
// in the intial linux port.

#ifndef __DISPATCH__STUBS__INTERNAL
#define __DISPATCH__STUBS__INTERNAL

int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
		 void *newp, size_t newlen);

mach_port_t pthread_mach_thread_np();

mach_port_t mach_task_self();

void mach_vm_deallocate(mach_port_t, mach_vm_address_t, mach_vm_size_t);

char* mach_error_string(mach_msg_return_t);
#endif


