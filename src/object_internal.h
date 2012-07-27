/*
 * Copyright (c) 2008-2010 Apple Inc. All rights reserved.
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

#ifndef __DISPATCH_OBJECT_INTERNAL__
#define __DISPATCH_OBJECT_INTERNAL__

#if OS_OBJECT_USE_OBJC
#define DISPATCH_DECL_INTERNAL_SUBCLASS(name, super) \
		OS_OBJECT_DECL_SUBCLASS(name, super)
#define DISPATCH_DECL_INTERNAL(name) \
		DISPATCH_DECL_INTERNAL_SUBCLASS(name, dispatch_object)
#define DISPATCH_DECL_SUBCLASS_INTERFACE(name, super) \
		_OS_OBJECT_DECL_SUBCLASS_INTERFACE(name, super)
#else
#define DISPATCH_DECL_INTERNAL_SUBCLASS(name, super) DISPATCH_DECL(name)
#define DISPATCH_DECL_INTERNAL(name) DISPATCH_DECL(name)
#define DISPATCH_DECL_SUBCLASS_INTERFACE(name, super)
#endif // OS_OBJECT_USE_OBJC

#if USE_OBJC
#define DISPATCH_CLASS(name) OS_OBJECT_CLASS(dispatch_##name)
// ObjC classes and dispatch vtables are co-located via linker order and alias
// files rdar://10640168
#define DISPATCH_VTABLE_SUBCLASS_INSTANCE(name, super, ...) \
		__attribute__((section("__DATA,__objc_data"), used)) \
		static const struct { \
			DISPATCH_VTABLE_HEADER(super); \
		} DISPATCH_CONCAT(_,DISPATCH_CLASS(name##_vtable)) = { \
			__VA_ARGS__ \
		}
#else
#define DISPATCH_VTABLE_SUBCLASS_INSTANCE(name, super, ...) \
		const struct dispatch_##super##_vtable_s _dispatch_##name##_vtable = { \
			._os_obj_xref_dispose = _dispatch_xref_dispose, \
			._os_obj_dispose = _dispatch_dispose, \
			__VA_ARGS__ \
		}
#endif // USE_OBJC

#define DISPATCH_SUBCLASS_DECL(name, super) \
		DISPATCH_DECL_SUBCLASS_INTERFACE(dispatch_##name, super) \
		struct dispatch_##name##_s; \
		extern const struct dispatch_##name##_vtable_s { \
			_OS_OBJECT_CLASS_HEADER(); \
			DISPATCH_VTABLE_HEADER(name); \
		} _dispatch_##name##_vtable
#define DISPATCH_CLASS_DECL(name) DISPATCH_SUBCLASS_DECL(name, dispatch_object)
#define DISPATCH_INTERNAL_SUBCLASS_DECL(name, super) \
		DISPATCH_DECL_INTERNAL_SUBCLASS(dispatch_##name, dispatch_##super); \
		DISPATCH_DECL_SUBCLASS_INTERFACE(dispatch_##name, dispatch_##super) \
		extern const struct dispatch_##super##_vtable_s _dispatch_##name##_vtable
#define DISPATCH_VTABLE_INSTANCE(name, ...) \
		DISPATCH_VTABLE_SUBCLASS_INSTANCE(name, name, __VA_ARGS__)
#define DISPATCH_VTABLE(name) &_dispatch_##name##_vtable

#define DISPATCH_VTABLE_HEADER(x) \
	unsigned long const do_type; \
	const char *const do_kind; \
	size_t (*const do_debug)(struct dispatch_##x##_s *, char *, size_t); \
	struct dispatch_queue_s *(*const do_invoke)(struct dispatch_##x##_s *); \
	bool (*const do_probe)(struct dispatch_##x##_s *); \
	void (*const do_dispose)(struct dispatch_##x##_s *)

#define dx_type(x) (x)->do_vtable->do_type
#define dx_kind(x) (x)->do_vtable->do_kind
#define dx_debug(x, y, z) (x)->do_vtable->do_debug((x), (y), (z))
#define dx_dispose(x) (x)->do_vtable->do_dispose(x)
#define dx_invoke(x) (x)->do_vtable->do_invoke(x)
#define dx_probe(x) (x)->do_vtable->do_probe(x)

#define DISPATCH_STRUCT_HEADER(x) \
	_OS_OBJECT_HEADER( \
	const struct dispatch_##x##_vtable_s *do_vtable, \
	do_ref_cnt, \
	do_xref_cnt); \
	struct dispatch_##x##_s *volatile do_next; \
	struct dispatch_queue_s *do_targetq; \
	void *do_ctxt; \
	void *do_finalizer; \
	unsigned int do_suspend_cnt;

#define DISPATCH_OBJECT_GLOBAL_REFCNT		_OS_OBJECT_GLOBAL_REFCNT
// "word and bit" must be a power of two to be safely subtracted
#define DISPATCH_OBJECT_SUSPEND_LOCK		1u
#define DISPATCH_OBJECT_SUSPEND_INTERVAL	2u
#define DISPATCH_OBJECT_SUSPENDED(x) \
		((x)->do_suspend_cnt >= DISPATCH_OBJECT_SUSPEND_INTERVAL)
#ifdef __LP64__
// the bottom nibble must not be zero, the rest of the bits should be random
// we sign extend the 64-bit version so that a better instruction encoding is
// generated on Intel
#define DISPATCH_OBJECT_LISTLESS ((void *)0xffffffff89abcdef)
#else
#define DISPATCH_OBJECT_LISTLESS ((void *)0x89abcdef)
#endif

enum {
	_DISPATCH_CONTINUATION_TYPE	=    0x00000, // meta-type for continuations
	_DISPATCH_QUEUE_TYPE			=    0x10000, // meta-type for queues
	_DISPATCH_SOURCE_TYPE			=    0x20000, // meta-type for sources
	_DISPATCH_SEMAPHORE_TYPE		=    0x30000, // meta-type for semaphores
	_DISPATCH_NODE_TYPE			=    0x40000, // meta-type for data node
	_DISPATCH_IO_TYPE				=    0x50000, // meta-type for io channels
	_DISPATCH_OPERATION_TYPE		=    0x60000, // meta-type for io operations
	_DISPATCH_DISK_TYPE			=    0x70000, // meta-type for io disks
	_DISPATCH_META_TYPE_MASK		=  0xfff0000, // mask for object meta-types
	_DISPATCH_ATTR_TYPE			= 0x10000000, // meta-type for attributes

	DISPATCH_CONTINUATION_TYPE		= _DISPATCH_CONTINUATION_TYPE,

	DISPATCH_DATA_TYPE				= _DISPATCH_NODE_TYPE,

	DISPATCH_IO_TYPE				= _DISPATCH_IO_TYPE,
	DISPATCH_OPERATION_TYPE			= _DISPATCH_OPERATION_TYPE,
	DISPATCH_DISK_TYPE				= _DISPATCH_DISK_TYPE,

	DISPATCH_QUEUE_ATTR_TYPE		= _DISPATCH_QUEUE_TYPE |_DISPATCH_ATTR_TYPE,

	DISPATCH_QUEUE_TYPE			= 1 | _DISPATCH_QUEUE_TYPE,
	DISPATCH_QUEUE_GLOBAL_TYPE		= 2 | _DISPATCH_QUEUE_TYPE,
	DISPATCH_QUEUE_MGR_TYPE		= 3 | _DISPATCH_QUEUE_TYPE,
	DISPATCH_QUEUE_SPECIFIC_TYPE	= 4 | _DISPATCH_QUEUE_TYPE,

	DISPATCH_SEMAPHORE_TYPE		= 1 | _DISPATCH_SEMAPHORE_TYPE,
	DISPATCH_GROUP_TYPE			= 2 | _DISPATCH_SEMAPHORE_TYPE,

	DISPATCH_SOURCE_KEVENT_TYPE	= 1 | _DISPATCH_SOURCE_TYPE,
};

DISPATCH_SUBCLASS_DECL(object, object);
struct dispatch_object_s {
	DISPATCH_STRUCT_HEADER(object);
};

size_t _dispatch_object_debug_attr(dispatch_object_t dou, char* buf,
		size_t bufsiz);
void *_dispatch_alloc(const void *vtable, size_t size);
void _dispatch_retain(dispatch_object_t dou);
void _dispatch_release(dispatch_object_t dou);
void _dispatch_xref_dispose(dispatch_object_t dou);
void _dispatch_dispose(dispatch_object_t dou);
#if DISPATCH_COCOA_COMPAT
void *_dispatch_autorelease_pool_push(void);
void _dispatch_autorelease_pool_pop(void *context);
#endif

typedef struct _os_object_class_s {
	_OS_OBJECT_CLASS_HEADER();
} _os_object_class_s;

typedef struct _os_object_s {
	_OS_OBJECT_HEADER(
	const _os_object_class_s *os_obj_isa,
	os_obj_ref_cnt,
	os_obj_xref_cnt);
} _os_object_s;

void _os_object_init(void);
unsigned long _os_object_retain_count(_os_object_t obj);
bool _os_object_retain_weak(_os_object_t obj);
bool _os_object_allows_weak_reference(_os_object_t obj);
void _os_object_dispose(_os_object_t obj);
void _os_object_xref_dispose(_os_object_t obj);

#endif
