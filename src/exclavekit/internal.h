/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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

#ifndef __DISPATCH_INTERNAL__
#define __DISPATCH_INTERNAL__

#define __DISPATCH_BUILDING_DISPATCH__
#define __DISPATCH_INDIRECT__

#include <dispatch/dispatch.h>
#include <dispatch/base.h>
#include <dispatch/object.h>
#include <dispatch/once.h>

#ifdef __BLOCKS__
#include <Block_private.h>
#include <Block.h>
#endif /* __BLOCKS__ */

#define DISPATCH_NOINLINE __attribute__((__noinline__))

#ifdef __BLOCKS__
#define _dispatch_Block_invoke(bb) \
		((dispatch_function_t)((struct Block_layout *)bb)->invoke)
#endif /* __BLOCKS__ */

#endif /* __DISPATCH_INTERNAL__ */
