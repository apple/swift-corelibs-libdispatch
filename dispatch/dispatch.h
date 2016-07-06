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

#ifndef __DISPATCH_PUBLIC__
#define __DISPATCH_PUBLIC__

#ifdef __APPLE__
#include <Availability.h>
#include <TargetConditionals.h>
#endif
#include <sys/cdefs.h>
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef __has_attribute
#if __has_attribute(unavailable)
#define __DISPATCH_UNAVAILABLE(msg) __attribute__((__unavailable__(msg)))
#endif
#endif
#ifndef __DISPATCH_UNAVAILABLE
#define __DISPATCH_UNAVAILABLE(msg)
#endif

#ifdef __linux__
#if __has_feature(modules)
#include <stdio.h> // for off_t (to match Glibc.modulemap)
#endif
#define DISPATCH_LINUX_UNAVAILABLE() \
		__DISPATCH_UNAVAILABLE("This interface is unavailable on linux systems")
#else
#define DISPATCH_LINUX_UNAVAILABLE()
#endif

#ifndef __OSX_AVAILABLE_STARTING
#define __OSX_AVAILABLE_STARTING(x, y)
#endif
#ifndef __OSX_AVAILABLE_BUT_DEPRECATED
#define __OSX_AVAILABLE_BUT_DEPRECATED(...)
#endif
#ifndef __OSX_AVAILABLE_BUT_DEPRECATED_MSG
#define __OSX_AVAILABLE_BUT_DEPRECATED_MSG(...)
#endif

#ifndef __OSX_AVAILABLE
#define __OSX_AVAILABLE(...)
#endif
#ifndef __IOS_AVAILABLE
#define __IOS_AVAILABLE(...)
#endif
#ifndef __TVOS_AVAILABLE
#define __TVOS_AVAILABLE(...)
#endif
#ifndef __WATCHOS_AVAILABLE
#define __WATCHOS_AVAILABLE(...)
#endif
#ifndef __OSX_DEPRECATED
#define __OSX_DEPRECATED(...)
#endif
#ifndef __IOS_DEPRECATED
#define __IOS_DEPRECATED(...)
#endif
#ifndef __TVOS_DEPRECATED
#define __TVOS_DEPRECATED(...)
#endif
#ifndef __WATCHOS_DEPRECATED
#define __WATCHOS_DEPRECATED(...)
#endif

#define DISPATCH_API_VERSION 20160612

#ifndef __DISPATCH_BUILDING_DISPATCH__

#ifndef __DISPATCH_INDIRECT__
#define __DISPATCH_INDIRECT__
#endif

#include <os/object.h>
#include <dispatch/base.h>
#include <dispatch/time.h>
#include <dispatch/object.h>
#include <dispatch/queue.h>
#include <dispatch/block.h>
#include <dispatch/source.h>
#include <dispatch/group.h>
#include <dispatch/semaphore.h>
#include <dispatch/once.h>
#include <dispatch/data.h>
#include <dispatch/io.h>

#undef __DISPATCH_INDIRECT__

#endif /* !__DISPATCH_BUILDING_DISPATCH__ */

#endif
