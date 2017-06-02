/*
 * Copyright (c) 2008-2013 Apple Inc. All rights reserved.
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

#define DISPATCH_SEMAPHORE_HEADER(cls, ns) \
	DISPATCH_OBJECT_HEADER(cls); \
	long volatile ns##_value; \
	_dispatch_sema4_t ns##_sema

struct dispatch_semaphore_header_s {
	DISPATCH_SEMAPHORE_HEADER(semaphore, dsema);
};

DISPATCH_CLASS_DECL(semaphore);
struct dispatch_semaphore_s {
	DISPATCH_SEMAPHORE_HEADER(semaphore, dsema);
	long dsema_orig;
};

DISPATCH_CLASS_DECL(group);
struct dispatch_group_s {
	DISPATCH_SEMAPHORE_HEADER(group, dg);
	int volatile dg_waiters;
	struct dispatch_continuation_s *volatile dg_notify_head;
	struct dispatch_continuation_s *volatile dg_notify_tail;
};

typedef union {
	struct dispatch_semaphore_header_s *_dsema_hdr;
	struct dispatch_semaphore_s *_dsema;
	struct dispatch_group_s *_dg;
#if USE_OBJC
	dispatch_semaphore_t _objc_dsema;
	dispatch_group_t _objc_dg;
#endif
} dispatch_semaphore_class_t DISPATCH_TRANSPARENT_UNION;

dispatch_group_t _dispatch_group_create_and_enter(void);
void _dispatch_group_dispose(dispatch_object_t dou, bool *allow_free);
size_t _dispatch_group_debug(dispatch_object_t dou, char *buf,
		size_t bufsiz);

void _dispatch_semaphore_dispose(dispatch_object_t dou, bool *allow_free);
size_t _dispatch_semaphore_debug(dispatch_object_t dou, char *buf,
		size_t bufsiz);

#endif
