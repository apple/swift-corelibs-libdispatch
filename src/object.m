/*
 * Copyright (c) 2011 Apple Inc. All rights reserved.
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

#include "internal.h"

#if USE_OBJC

#if !__OBJC2__
#error "Cannot build with legacy ObjC runtime"
#endif
#if _OS_OBJECT_OBJC_ARC
#error "Cannot build with ARC"
#endif

#include <objc/runtime.h>
#include <objc/objc-internal.h>
#include <objc/objc-exception.h>

#pragma mark -
#pragma mark _os_object_gc

#if __OBJC_GC__
#include <objc/objc-auto.h>
#include <auto_zone.h>

static dispatch_once_t _os_object_gc_pred;
static bool _os_object_have_gc;
static malloc_zone_t *_os_object_gc_zone;

static void
_os_object_gc_init(void *ctxt DISPATCH_UNUSED)
{
	_os_object_have_gc = objc_collectingEnabled();
	if (slowpath(_os_object_have_gc)) {
		_os_object_gc_zone = objc_collectableZone();
	}
}

static _os_object_t
_os_object_make_uncollectable(_os_object_t obj)
{
	dispatch_once_f(&_os_object_gc_pred, NULL, _os_object_gc_init);
	if (slowpath(_os_object_have_gc)) {
		auto_zone_retain(_os_object_gc_zone, obj);
	}
	return obj;
}

static _os_object_t
_os_object_make_collectable(_os_object_t obj)
{
	dispatch_once_f(&_os_object_gc_pred, NULL, _os_object_gc_init);
	if (slowpath(_os_object_have_gc)) {
		auto_zone_release(_os_object_gc_zone, obj);
	}
	return obj;
}
#else
#define _os_object_make_uncollectable(obj) (obj)
#define _os_object_make_collectable(obj) (obj)
#endif // __OBJC_GC__

#pragma mark -
#pragma mark _os_object_t

void
_os_object_init(void)
{
	return _objc_init();
}

_os_object_t
_os_object_alloc(const void *_cls, size_t size)
{
	Class cls = _cls;
	_os_object_t obj;
	dispatch_assert(size >= sizeof(struct _os_object_s));
	size -= sizeof(((struct _os_object_s *)NULL)->os_obj_isa);
	if (!cls) cls = [OS_OBJECT_CLASS(object) class];
	while (!fastpath(obj = class_createInstance(cls, size))) {
		sleep(1); // Temporary resource shortage
	}
	return _os_object_make_uncollectable(obj);
}

void
_os_object_dealloc(_os_object_t obj)
{
	[_os_object_make_collectable(obj) dealloc];
}

void
_os_object_xref_dispose(_os_object_t obj)
{
	[obj _xref_dispose];
}

void
_os_object_dispose(_os_object_t obj)
{
	[obj _dispose];
}

#pragma mark -
#pragma mark _os_object

@implementation OS_OBJECT_CLASS(object)

-(id)retain {
	return _os_object_retain(self);
}

-(oneway void)release {
	return _os_object_release(self);
}

-(NSUInteger)retainCount {
	return _os_object_retain_count(self);
}

-(BOOL)retainWeakReference {
	return _os_object_retain_weak(self);
}

-(BOOL)allowsWeakReference {
	return _os_object_allows_weak_reference(self);
}

- (void)_xref_dispose {
	return _os_object_release_internal(self);
}

- (void)_dispose {
	return _os_object_dealloc(self);
}

@end

#pragma mark -
#pragma mark _dispatch_object

#include <Foundation/NSString.h>

// Force non-lazy class realization rdar://10640168
#define DISPATCH_OBJC_LOAD() + (void)load {}

@implementation DISPATCH_CLASS(object)

- (id)init {
	self = [super init];
	[self release];
	self = nil;
	return self;
}

- (void)_xref_dispose {
	_dispatch_xref_dispose(self);
	[super _xref_dispose];
}

- (void)_dispose {
	return _dispatch_dispose(self); // calls _os_object_dealloc()
}

- (NSString *)debugDescription {
	Class nsstring = objc_lookUpClass("NSString");
	if (!nsstring) return nil;
	char buf[4096];
	dx_debug((struct dispatch_object_s *)self, buf, sizeof(buf));
	return [nsstring stringWithFormat:
			[nsstring stringWithUTF8String:"<%s: %s>"],
			class_getName([self class]), buf];
}

@end

@implementation DISPATCH_CLASS(queue)
DISPATCH_OBJC_LOAD()

- (NSString *)description {
	Class nsstring = objc_lookUpClass("NSString");
	if (!nsstring) return nil;
	return [nsstring stringWithFormat:
			[nsstring stringWithUTF8String:"<%s: %s[%p]>"],
			class_getName([self class]), dispatch_queue_get_label(self), self];
}

@end

@implementation DISPATCH_CLASS(source)
DISPATCH_OBJC_LOAD()

- (void)_xref_dispose {
	_dispatch_source_xref_dispose(self);
	[super _xref_dispose];
}

@end

#define DISPATCH_CLASS_IMPL(name) \
		@implementation DISPATCH_CLASS(name) \
		DISPATCH_OBJC_LOAD() \
		@end

DISPATCH_CLASS_IMPL(semaphore)
DISPATCH_CLASS_IMPL(group)
DISPATCH_CLASS_IMPL(queue_root)
DISPATCH_CLASS_IMPL(queue_mgr)
DISPATCH_CLASS_IMPL(queue_specific_queue)
DISPATCH_CLASS_IMPL(queue_attr)
DISPATCH_CLASS_IMPL(io)
DISPATCH_CLASS_IMPL(operation)
DISPATCH_CLASS_IMPL(disk)
DISPATCH_CLASS_IMPL(data)

#pragma mark -
#pragma mark dispatch_autorelease_pool

#if DISPATCH_COCOA_COMPAT

void *
_dispatch_autorelease_pool_push(void) {
	return objc_autoreleasePoolPush();
}

void
_dispatch_autorelease_pool_pop(void *context) {
	return objc_autoreleasePoolPop(context);
}

#endif // DISPATCH_COCOA_COMPAT

#pragma mark -
#pragma mark dispatch_client_callout

// Abort on uncaught exceptions thrown from client callouts rdar://8577499
#if DISPATCH_USE_CLIENT_CALLOUT && !__arm__
// On platforms with zero-cost exceptions, use a compiler-generated catch-all
// exception handler.

DISPATCH_NORETURN extern void objc_terminate(void);

#undef _dispatch_client_callout
void
_dispatch_client_callout(void *ctxt, dispatch_function_t f)
{
	@try {
		return f(ctxt);
	}
	@catch (...) {
		objc_terminate();
	}
}

#undef _dispatch_client_callout2
void
_dispatch_client_callout2(void *ctxt, size_t i, void (*f)(void *, size_t))
{
	@try {
		return f(ctxt, i);
	}
	@catch (...) {
		objc_terminate();
	}
}

#endif // DISPATCH_USE_CLIENT_CALLOUT

#endif // USE_OBJC
