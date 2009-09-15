/*
 * Copyright (c) 2008-2009 Apple Inc. All rights reserved.
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


void
dispatch_debug(dispatch_object_t dou, const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);

	dispatch_debugv(dou._do, msg, ap);

	va_end(ap);
}

void
dispatch_debugv(dispatch_object_t dou, const char *msg, va_list ap)
{
	char buf[4096];
	size_t offs;

	if (dou._do && dou._do->do_vtable->do_debug) {
		offs = dx_debug(dou._do, buf, sizeof(buf));
	} else {
		offs = snprintf(buf, sizeof(buf), "NULL vtable slot");
	}

	snprintf(buf + offs, sizeof(buf) - offs, ": %s", msg);

	_dispatch_logv(buf, ap);
}

void
dispatch_retain(dispatch_object_t dou)
{
	if (dou._do->do_xref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT) {
		return; // global object
	}
	if ((dispatch_atomic_inc(&dou._do->do_xref_cnt) - 1) == 0) {
		DISPATCH_CLIENT_CRASH("Resurrection of an object");
	}
}

void
_dispatch_retain(dispatch_object_t dou)
{
	if (dou._do->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT) {
		return; // global object
	}
	if ((dispatch_atomic_inc(&dou._do->do_ref_cnt) - 1) == 0) {
		DISPATCH_CLIENT_CRASH("Resurrection of an object");
	}
}

void
dispatch_release(dispatch_object_t dou)
{
	typeof(dou._do->do_xref_cnt) oldval;

	if (dou._do->do_xref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT) {
		return;
	}

	oldval = dispatch_atomic_dec(&dou._do->do_xref_cnt) + 1;
	
	if (fastpath(oldval > 1)) {
		return;
	}
	if (oldval == 1) {
#ifndef DISPATCH_NO_LEGACY
		if (dou._do->do_vtable == (void*)&_dispatch_source_kevent_vtable) {
			return _dispatch_source_legacy_xref_release(dou._ds);
		}
#endif
		if (slowpath(DISPATCH_OBJECT_SUSPENDED(dou._do))) {
			// Arguments for and against this assert are within 6705399
			DISPATCH_CLIENT_CRASH("Release of a suspended object");
		}
		return _dispatch_release(dou._do);
	}
	DISPATCH_CLIENT_CRASH("Over-release of an object");
}

void
_dispatch_dispose(dispatch_object_t dou)
{
	dispatch_queue_t tq = dou._do->do_targetq;
	dispatch_function_t func = dou._do->do_finalizer;
	void *ctxt = dou._do->do_ctxt;

	dou._do->do_vtable = (void *)0x200;

	free(dou._do);

	if (func && ctxt) {
		dispatch_async_f(tq, ctxt, func);
	}
	_dispatch_release(tq);
}

void
_dispatch_release(dispatch_object_t dou)
{
	typeof(dou._do->do_ref_cnt) oldval;

	if (dou._do->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT) {
		return; // global object
	}

	oldval = dispatch_atomic_dec(&dou._do->do_ref_cnt) + 1;
	
	if (fastpath(oldval > 1)) {
		return;
	}
	if (oldval == 1) {
		if (dou._do->do_next != DISPATCH_OBJECT_LISTLESS) {
			DISPATCH_CRASH("release while enqueued");
		}
		if (dou._do->do_xref_cnt) {
			DISPATCH_CRASH("release while external references exist");
		}

		return dx_dispose(dou._do);
	}
	DISPATCH_CRASH("over-release");
}

void *
dispatch_get_context(dispatch_object_t dou)
{
	return dou._do->do_ctxt;
}

void
dispatch_set_context(dispatch_object_t dou, void *context)
{
	if (dou._do->do_ref_cnt != DISPATCH_OBJECT_GLOBAL_REFCNT) {
		dou._do->do_ctxt = context;
	}
}

void
dispatch_set_finalizer_f(dispatch_object_t dou, dispatch_function_t finalizer)
{
	dou._do->do_finalizer = finalizer;
}

void
dispatch_suspend(dispatch_object_t dou)
{
	if (slowpath(dou._do->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		return;
	}
	dispatch_atomic_add(&dou._do->do_suspend_cnt, DISPATCH_OBJECT_SUSPEND_INTERVAL);
}

void
dispatch_resume(dispatch_object_t dou)
{
	if (slowpath(dou._do->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		return;
	}
	switch (dispatch_atomic_sub(&dou._do->do_suspend_cnt, DISPATCH_OBJECT_SUSPEND_INTERVAL) + DISPATCH_OBJECT_SUSPEND_INTERVAL) {
	case DISPATCH_OBJECT_SUSPEND_INTERVAL:
		_dispatch_wakeup(dou._do);
		break;
	case 0:
		DISPATCH_CLIENT_CRASH("Over-resume of an object");
		break;
	default:
		break;
	}
}

size_t
dispatch_object_debug_attr(dispatch_object_t dou, char* buf, size_t bufsiz)
{
	return snprintf(buf, bufsiz, "refcnt = 0x%x, suspend_cnt = 0x%x, ",
					dou._do->do_ref_cnt, dou._do->do_suspend_cnt);
}
