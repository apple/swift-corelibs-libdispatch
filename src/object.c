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

#include "internal.h"

void
dispatch_retain(dispatch_object_t dou)
{
	if (slowpath(dou._do->do_xref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		return; // global object
	}
	if (slowpath((dispatch_atomic_inc2o(dou._do, do_xref_cnt) - 1) == 0)) {
		DISPATCH_CLIENT_CRASH("Resurrection of an object");
	}
}

void
_dispatch_retain(dispatch_object_t dou)
{
	if (slowpath(dou._do->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		return; // global object
	}
	if (slowpath((dispatch_atomic_inc2o(dou._do, do_ref_cnt) - 1) == 0)) {
		DISPATCH_CLIENT_CRASH("Resurrection of an object");
	}
}

void
dispatch_release(dispatch_object_t dou)
{
	if (slowpath(dou._do->do_xref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		return;
	}

	unsigned int xref_cnt = dispatch_atomic_dec2o(dou._do, do_xref_cnt) + 1;
	if (fastpath(xref_cnt > 1)) {
		return;
	}
	if (fastpath(xref_cnt == 1)) {
		if (dou._do->do_vtable == (void*)&_dispatch_source_kevent_vtable) {
			return _dispatch_source_xref_release(dou._ds);
		}
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
	if (slowpath(dou._do->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		return; // global object
	}

	unsigned int ref_cnt = dispatch_atomic_dec2o(dou._do, do_ref_cnt) + 1;
	if (fastpath(ref_cnt > 1)) {
		return;
	}
	if (fastpath(ref_cnt == 1)) {
		if (slowpath(dou._do->do_next != DISPATCH_OBJECT_LISTLESS)) {
			DISPATCH_CRASH("release while enqueued");
		}
		if (slowpath(dou._do->do_xref_cnt)) {
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
	// rdar://8181908 explains why we need to do an internal retain at every
	// suspension.
	(void)dispatch_atomic_add2o(dou._do, do_suspend_cnt,
			DISPATCH_OBJECT_SUSPEND_INTERVAL);
	_dispatch_retain(dou._do);
}

DISPATCH_NOINLINE
static void
_dispatch_resume_slow(dispatch_object_t dou)
{
	_dispatch_wakeup(dou._do);
	// Balancing the retain() done in suspend() for rdar://8181908
	_dispatch_release(dou._do);
}

void
dispatch_resume(dispatch_object_t dou)
{
	// Global objects cannot be suspended or resumed. This also has the
	// side effect of saturating the suspend count of an object and
	// guarding against resuming due to overflow.
	if (slowpath(dou._do->do_ref_cnt == DISPATCH_OBJECT_GLOBAL_REFCNT)) {
		return;
	}
	// Check the previous value of the suspend count. If the previous
	// value was a single suspend interval, the object should be resumed.
	// If the previous value was less than the suspend interval, the object
	// has been over-resumed.
	unsigned int suspend_cnt = dispatch_atomic_sub2o(dou._do, do_suspend_cnt,
			DISPATCH_OBJECT_SUSPEND_INTERVAL) +
			DISPATCH_OBJECT_SUSPEND_INTERVAL;
	if (fastpath(suspend_cnt > DISPATCH_OBJECT_SUSPEND_INTERVAL)) {
		// Balancing the retain() done in suspend() for rdar://8181908
		return _dispatch_release(dou._do);
	}
	if (fastpath(suspend_cnt == DISPATCH_OBJECT_SUSPEND_INTERVAL)) {
		return _dispatch_resume_slow(dou);
	}
	DISPATCH_CLIENT_CRASH("Over-resume of an object");
}

size_t
_dispatch_object_debug_attr(dispatch_object_t dou, char* buf, size_t bufsiz)
{
	return snprintf(buf, bufsiz, "xrefcnt = 0x%x, refcnt = 0x%x, "
			"suspend_cnt = 0x%x, locked = %d, ", dou._do->do_xref_cnt,
			dou._do->do_ref_cnt,
			dou._do->do_suspend_cnt / DISPATCH_OBJECT_SUSPEND_INTERVAL,
			dou._do->do_suspend_cnt & 1);
}

