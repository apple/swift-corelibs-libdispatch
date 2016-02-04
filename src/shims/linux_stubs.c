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


/*
 * This file contains stubbed out functions we are using during
 * the initial linux port.  When the port is complete, this file
 * should be empty (and thus removed).
 */

#include <stdint.h>
#include <syscall.h>
#include <config/config.h>

#include "pthread.h"
#include "os/linux_base.h"
#include "internal.h"


#undef LINUX_PORT_ERROR
#define LINUX_PORT_ERROR()  do { printf("LINUX_PORT_ERROR_CALLED %s:%d: %s\n",__FILE__,__LINE__,__FUNCTION__); abort(); } while (0)

dispatch_block_t _dispatch_block_create(dispatch_block_flags_t flags,
					voucher_t voucher, pthread_priority_t priority,
					dispatch_block_t block) {
  LINUX_PORT_ERROR();
}

unsigned long _dispatch_runloop_queue_probe(dispatch_queue_t dq) {
  LINUX_PORT_ERROR();
}
void _dispatch_runloop_queue_xref_dispose() { LINUX_PORT_ERROR();  }

void _dispatch_runloop_queue_dispose() { LINUX_PORT_ERROR();  }
char* mach_error_string(mach_msg_return_t x) {
  LINUX_PORT_ERROR();
}
void mach_vm_deallocate() { LINUX_PORT_ERROR();  }

mach_port_t pthread_mach_thread_np(void) {
  return (pid_t)syscall(SYS_gettid);
}
mach_port_t mach_task_self(void) {
  return (mach_port_t)pthread_self();
}

/*
 * Stubbed out static data
 */

pthread_key_t dispatch_voucher_key;
pthread_key_t dispatch_pthread_root_queue_observer_hooks_key;

unsigned short dispatch_timer__program_semaphore;
unsigned short dispatch_timer__wake_semaphore;
unsigned short dispatch_timer__fire_semaphore;
unsigned short dispatch_timer__configure_semaphore;
unsigned short dispatch_queue__pop_semaphore;
unsigned short dispatch_callout__entry_semaphore;
unsigned short dispatch_callout__return_semaphore;
unsigned short dispatch_queue__push_semaphore;
void (*_dispatch_block_special_invoke)(void*);
struct dispatch_queue_attr_s _dispatch_queue_attr_concurrent;
