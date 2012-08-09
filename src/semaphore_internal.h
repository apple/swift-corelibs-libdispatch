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

#ifndef __DISPATCH_SEMAPHORE_INTERNAL__
#define __DISPATCH_SEMAPHORE_INTERNAL__

struct dispatch_queue_s;

struct dispatch_sema_notify_s {
	struct dispatch_sema_notify_s *volatile dsn_next;
	struct dispatch_queue_s *dsn_queue;
	void *dsn_ctxt;
	void (*dsn_func)(void *);
};

DISPATCH_CLASS_DECL(semaphore);
struct dispatch_semaphore_s {
	DISPATCH_STRUCT_HEADER(semaphore);
	long dsema_value;
	long dsema_orig;
	size_t dsema_sent_ksignals;
#if USE_MACH_SEM && USE_POSIX_SEM
#error "Too many supported semaphore types"
#elif USE_MACH_SEM
	semaphore_t dsema_port;
	semaphore_t dsema_waiter_port;
#elif USE_POSIX_SEM
	sem_t dsema_sem;
#else
#error "No supported semaphore type"
#endif
	size_t dsema_group_waiters;
	struct dispatch_sema_notify_s *dsema_notify_head;
	struct dispatch_sema_notify_s *dsema_notify_tail;
};

DISPATCH_CLASS_DECL(group);

void _dispatch_semaphore_dispose(dispatch_object_t dou);
size_t _dispatch_semaphore_debug(dispatch_object_t dou, char *buf,
		size_t bufsiz);

typedef uintptr_t _dispatch_thread_semaphore_t;
_dispatch_thread_semaphore_t _dispatch_get_thread_semaphore(void);
void _dispatch_put_thread_semaphore(_dispatch_thread_semaphore_t);
void _dispatch_thread_semaphore_wait(_dispatch_thread_semaphore_t);
void _dispatch_thread_semaphore_signal(_dispatch_thread_semaphore_t);
void _dispatch_thread_semaphore_dispose(_dispatch_thread_semaphore_t);

#endif
