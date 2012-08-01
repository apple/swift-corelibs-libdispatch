/*
 * Copyright (c) 2010-2011 Apple Inc. All rights reserved.
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

#ifndef __DISPATCH_TRACE__
#define __DISPATCH_TRACE__

#if DISPATCH_USE_DTRACE

#include "provider.h"

#define _dispatch_trace_callout(_c, _f, _dcc) do { \
		if (slowpath(DISPATCH_CALLOUT_ENTRY_ENABLED()) || \
				slowpath(DISPATCH_CALLOUT_RETURN_ENABLED())) { \
			dispatch_queue_t _dq = _dispatch_queue_get_current(); \
			char *_label = _dq ? _dq->dq_label : ""; \
			dispatch_function_t _func = (dispatch_function_t)(_f); \
			void *_ctxt = (_c); \
			DISPATCH_CALLOUT_ENTRY(_dq, _label, _func, _ctxt); \
			_dcc; \
			DISPATCH_CALLOUT_RETURN(_dq, _label, _func, _ctxt); \
			return; \
		} \
		return _dcc; \
	} while (0)

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_trace_client_callout(void *ctxt, dispatch_function_t f)
{
	_dispatch_trace_callout(ctxt, f == _dispatch_call_block_and_release &&
			ctxt ? ((struct Block_basic *)ctxt)->Block_invoke : f,
			_dispatch_client_callout(ctxt, f));
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_trace_client_callout2(void *ctxt, size_t i, void (*f)(void *, size_t))
{
	_dispatch_trace_callout(ctxt, f, _dispatch_client_callout2(ctxt, i, f));
}

#ifdef __BLOCKS__
DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_trace_client_callout_block(dispatch_block_t b)
{
	struct Block_basic *bb = (void*)b;
	_dispatch_trace_callout(b, bb->Block_invoke,
			_dispatch_client_callout(b, (dispatch_function_t)bb->Block_invoke));
}
#endif

#define _dispatch_client_callout		_dispatch_trace_client_callout
#define _dispatch_client_callout2		_dispatch_trace_client_callout2
#define _dispatch_client_callout_block	_dispatch_trace_client_callout_block

#define _dispatch_trace_continuation(_q, _o, _t) do { \
		dispatch_queue_t _dq = (_q); \
		char *_label = _dq ? _dq->dq_label : ""; \
		struct dispatch_object_s *_do = (_o); \
		char *_kind; \
		dispatch_function_t _func; \
		void *_ctxt; \
		if (DISPATCH_OBJ_IS_VTABLE(_do)) { \
			_ctxt = _do->do_ctxt; \
			_kind = (char*)dx_kind(_do); \
			if (dx_type(_do) == DISPATCH_SOURCE_KEVENT_TYPE && \
					(_dq) != &_dispatch_mgr_q) { \
				_func = ((dispatch_source_t)_do)->ds_refs->ds_handler_func; \
			} else { \
				_func = (dispatch_function_t)_dispatch_queue_invoke; \
			} \
		} else { \
			struct dispatch_continuation_s *_dc = (void*)(_do); \
			_ctxt = _dc->dc_ctxt; \
			if ((long)_dc->do_vtable & DISPATCH_OBJ_SYNC_SLOW_BIT) { \
				_kind = "semaphore"; \
				_func = (dispatch_function_t)dispatch_semaphore_signal; \
			} else if (_dc->dc_func == _dispatch_call_block_and_release) { \
				_kind = "block"; \
				_func = ((struct Block_basic *)_dc->dc_ctxt)->Block_invoke;\
			} else { \
				_kind = "function"; \
				_func = _dc->dc_func; \
			} \
		} \
		_t(_dq, _label, _do, _kind, _func, _ctxt); \
	} while (0)

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_trace_queue_push_list(dispatch_queue_t dq, dispatch_object_t _head,
		dispatch_object_t _tail)
{
	if (slowpath(DISPATCH_QUEUE_PUSH_ENABLED())) {
		struct dispatch_object_s *dou = _head._do;
		do {
			_dispatch_trace_continuation(dq, dou, DISPATCH_QUEUE_PUSH);
		} while (dou != _tail._do && (dou = dou->do_next));
	}
	_dispatch_queue_push_list(dq, _head, _tail);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_queue_push_notrace(dispatch_queue_t dq, dispatch_object_t dou)
{
	_dispatch_queue_push_list(dq, dou, dou);
}

#define _dispatch_queue_push_list _dispatch_trace_queue_push_list

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_trace_continuation_pop(dispatch_queue_t dq,
		dispatch_object_t dou)
{
	if (slowpath(DISPATCH_QUEUE_POP_ENABLED())) {
		_dispatch_trace_continuation(dq, dou._do, DISPATCH_QUEUE_POP);
	}
}
#else

#define _dispatch_queue_push_notrace _dispatch_queue_push
#define _dispatch_trace_continuation_pop(dq, dou)

#endif // DISPATCH_USE_DTRACE

#endif // __DISPATCH_TRACE__
