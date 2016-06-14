/*
 * Copyright (c) 2011-2014 Apple Inc. All rights reserved.
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

#if _OS_OBJECT_OBJC_ARC
#error "Cannot build with ARC"
#endif
#if defined(__OBJC_GC__)
#error Objective C GC isn't supported anymore
#endif

#include <objc/objc-internal.h>
#include <objc/objc-exception.h>
#include <Foundation/NSString.h>

#pragma mark -
#pragma mark _os_object_t

static inline id
_os_objc_alloc(Class cls, size_t size)
{
	id obj;
	size -= sizeof(((struct _os_object_s *)NULL)->os_obj_isa);
	while (!fastpath(obj = class_createInstance(cls, size))) {
		_dispatch_temporary_resource_shortage();
	}
	return obj;
}

static void*
_os_objc_destructInstance(id obj)
{
	// noop if only Libystem is loaded
	return obj;
}

#if DISPATCH_COCOA_COMPAT
static bool _os_object_debug_missing_pools;
#endif

void
_os_object_init(void)
{
	_objc_init();
	Block_callbacks_RR callbacks = {
		sizeof(Block_callbacks_RR),
		(void (*)(const void *))&objc_retain,
		(void (*)(const void *))&objc_release,
		(void (*)(const void *))&_os_objc_destructInstance
	};
	_Block_use_RR2(&callbacks);
#if DISPATCH_COCOA_COMPAT
	const char *v = getenv("OBJC_DEBUG_MISSING_POOLS");
	_os_object_debug_missing_pools = v && !strcmp(v, "YES");
#endif
}

_os_object_t
_os_object_alloc_realized(const void *cls, size_t size)
{
	dispatch_assert(size >= sizeof(struct _os_object_s));
	return _os_objc_alloc(cls, size);
}

_os_object_t
_os_object_alloc(const void *_cls, size_t size)
{
	dispatch_assert(size >= sizeof(struct _os_object_s));
	Class cls = _cls ? [(id)_cls class] : [OS_OBJECT_CLASS(object) class];
	return _os_objc_alloc(cls, size);
}

void
_os_object_dealloc(_os_object_t obj)
{
	[obj dealloc];
}

void
_os_object_xref_dispose(_os_object_t obj)
{
	struct _os_object_s *o = (struct _os_object_s *)obj;
	_os_object_xrefcnt_dispose_barrier(o);
	[obj _xref_dispose];
}

void
_os_object_dispose(_os_object_t obj)
{
	struct _os_object_s *o = (struct _os_object_s *)obj;
	_os_object_refcnt_dispose_barrier(o);
	[obj _dispose];
}

#undef os_retain
void*
os_retain(void *obj)
{
	return objc_retain(obj);
}

#undef os_release
void
os_release(void *obj)
{
	return objc_release(obj);
}

#pragma mark -
#pragma mark _os_object

@implementation OS_OBJECT_CLASS(object)
DISPATCH_UNAVAILABLE_INIT()

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
#pragma mark _dispatch_objc
#if OS_OBJECT_HAVE_OBJC2

id
_dispatch_objc_alloc(Class cls, size_t size)
{
	return _os_objc_alloc(cls, size);
}

void
_dispatch_objc_retain(dispatch_object_t dou)
{
	return (void)os_retain(dou);
}

void
_dispatch_objc_release(dispatch_object_t dou)
{
	return os_release(dou);
}

void
_dispatch_objc_set_context(dispatch_object_t dou, void *context)
{
	return [dou _setContext:context];
}

void *
_dispatch_objc_get_context(dispatch_object_t dou)
{
	return [dou _getContext];
}

void
_dispatch_objc_set_finalizer_f(dispatch_object_t dou,
		dispatch_function_t finalizer)
{
	return [dou _setFinalizer:finalizer];
}

void
_dispatch_objc_set_target_queue(dispatch_object_t dou, dispatch_queue_t queue)
{
	return [dou _setTargetQueue:queue];
}

void
_dispatch_objc_suspend(dispatch_object_t dou)
{
	return [dou _suspend];
}

void
_dispatch_objc_resume(dispatch_object_t dou)
{
	return [dou _resume];
}

void
_dispatch_objc_activate(dispatch_object_t dou)
{
	return [dou _activate];
}

size_t
_dispatch_objc_debug(dispatch_object_t dou, char* buf, size_t bufsiz)
{
	NSUInteger offset = 0;
	NSString *desc = [dou debugDescription];
	[desc getBytes:buf maxLength:bufsiz-1 usedLength:&offset
			encoding:NSUTF8StringEncoding options:0
			range:NSMakeRange(0, [desc length]) remainingRange:NULL];
	if (offset) buf[offset] = 0;
	return offset;
}

#endif
#pragma mark -
#pragma mark _dispatch_object

// Force non-lazy class realization rdar://10640168
#define DISPATCH_OBJC_LOAD() + (void)load {}

@implementation DISPATCH_CLASS(object)
DISPATCH_UNAVAILABLE_INIT()

- (void)_dispose {
	return _dispatch_dispose(self); // calls _os_object_dealloc()
}

- (NSString *)debugDescription {
	Class nsstring = objc_lookUpClass("NSString");
	if (!nsstring) return nil;
	char buf[2048];
	struct dispatch_object_s *obj = (struct dispatch_object_s *)self;
	if (dx_vtable(obj)->do_debug) {
		dx_debug(obj, buf, sizeof(buf));
	} else {
		strlcpy(buf, dx_kind(obj), sizeof(buf));
	}
	return [nsstring stringWithFormat:
			[nsstring stringWithUTF8String:"<%s: %s>"],
			class_getName([self class]), buf];
}

@end

@implementation DISPATCH_CLASS(queue)
DISPATCH_OBJC_LOAD()
DISPATCH_UNAVAILABLE_INIT()

- (NSString *)description {
	Class nsstring = objc_lookUpClass("NSString");
	if (!nsstring) return nil;
	return [nsstring stringWithFormat:
			[nsstring stringWithUTF8String:"<%s: %s[%p]>"],
			class_getName([self class]), dispatch_queue_get_label(self), self];
}

- (void)_xref_dispose {
	_dispatch_queue_xref_dispose((struct dispatch_queue_s *)self);
	[super _xref_dispose];
}

@end

@implementation DISPATCH_CLASS(source)
DISPATCH_OBJC_LOAD()
DISPATCH_UNAVAILABLE_INIT()

- (void)_xref_dispose {
	_dispatch_queue_xref_dispose((struct dispatch_queue_s *)self);
	_dispatch_source_xref_dispose(self);
	[super _xref_dispose];
}

@end

@implementation DISPATCH_CLASS(mach)
DISPATCH_OBJC_LOAD()
DISPATCH_UNAVAILABLE_INIT()

- (void)_xref_dispose {
	_dispatch_queue_xref_dispose((struct dispatch_queue_s *)self);
	[super _xref_dispose];
}

@end

@implementation DISPATCH_CLASS(queue_runloop)
DISPATCH_OBJC_LOAD()
DISPATCH_UNAVAILABLE_INIT()

- (void)_xref_dispose {
	_dispatch_queue_xref_dispose((struct dispatch_queue_s *)self);
	_dispatch_runloop_queue_xref_dispose(self);
	[super _xref_dispose];
}

@end

#define DISPATCH_CLASS_IMPL(name) \
		@implementation DISPATCH_CLASS(name) \
		DISPATCH_OBJC_LOAD() \
		DISPATCH_UNAVAILABLE_INIT() \
		@end

#if !DISPATCH_DATA_IS_BRIDGED_TO_NSDATA
DISPATCH_CLASS_IMPL(data)
#endif
DISPATCH_CLASS_IMPL(semaphore)
DISPATCH_CLASS_IMPL(group)
DISPATCH_CLASS_IMPL(queue_serial)
DISPATCH_CLASS_IMPL(queue_concurrent)
DISPATCH_CLASS_IMPL(queue_main)
DISPATCH_CLASS_IMPL(queue_root)
DISPATCH_CLASS_IMPL(queue_mgr)
DISPATCH_CLASS_IMPL(queue_specific_queue)
DISPATCH_CLASS_IMPL(queue_attr)
DISPATCH_CLASS_IMPL(mach_msg)
DISPATCH_CLASS_IMPL(io)
DISPATCH_CLASS_IMPL(operation)
DISPATCH_CLASS_IMPL(disk)

@implementation OS_OBJECT_CLASS(voucher)
DISPATCH_UNAVAILABLE_INIT()
DISPATCH_OBJC_LOAD()

- (void)_xref_dispose {
	return _voucher_xref_dispose(self); // calls _os_object_release_internal()
}

- (void)_dispose {
	return _voucher_dispose(self); // calls _os_object_dealloc()
}

- (NSString *)debugDescription {
	Class nsstring = objc_lookUpClass("NSString");
	if (!nsstring) return nil;
	char buf[2048];
	_voucher_debug(self, buf, sizeof(buf));
	return [nsstring stringWithFormat:
			[nsstring stringWithUTF8String:"<%s: %s>"],
			class_getName([self class]), buf];
}

@end

#if VOUCHER_ENABLE_RECIPE_OBJECTS
@implementation OS_OBJECT_CLASS(voucher_recipe)
DISPATCH_UNAVAILABLE_INIT()
DISPATCH_OBJC_LOAD()

- (void)_dispose {

}

- (NSString *)debugDescription {
	return nil; // TODO: voucher_recipe debugDescription
}

@end
#endif


#pragma mark -
#pragma mark dispatch_last_resort_autorelease_pool

#if DISPATCH_COCOA_COMPAT

void *
_dispatch_last_resort_autorelease_pool_push(void)
{
	if (!slowpath(_os_object_debug_missing_pools)) {
		return _dispatch_autorelease_pool_push();
	}
	return NULL;
}

void
_dispatch_last_resort_autorelease_pool_pop(void *context)
{
	if (!slowpath(_os_object_debug_missing_pools)) {
		return _dispatch_autorelease_pool_pop(context);
	}
}

#endif // DISPATCH_COCOA_COMPAT

#pragma mark -
#pragma mark dispatch_client_callout

// Abort on uncaught exceptions thrown from client callouts rdar://8577499
#if DISPATCH_USE_CLIENT_CALLOUT && !__USING_SJLJ_EXCEPTIONS__ && \
		OS_OBJECT_HAVE_OBJC2
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

#if HAVE_MACH
#undef _dispatch_client_callout4
void
_dispatch_client_callout4(void *ctxt, dispatch_mach_reason_t reason,
		dispatch_mach_msg_t dmsg, mach_error_t error,
		dispatch_mach_handler_function_t f)
{
	@try {
		return f(ctxt, reason, dmsg, error);
	}
	@catch (...) {
		objc_terminate();
	}
}
#endif // HAVE_MACH

#endif // DISPATCH_USE_CLIENT_CALLOUT

#endif // USE_OBJC
