/*
 * Copyright (c) 2018 Apple Inc. All rights reserved.
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
#if DISPATCH_EVENT_BACKEND_WINDOWS

static HANDLE hPort = NULL;
enum _dispatch_iocp_completion_key
{
	DISPATCH_PORT_POKE = 0,
	DISPATCH_PORT_TIMER_CLOCK_UPTIME,
};

typedef struct _dispatch_windows_timeout_s {
	PTP_TIMER pTimer;
	enum _dispatch_iocp_completion_key ullIdent;
	BOOL bArmed;
} *dispatch_windows_timeout_t;

#define DISPATCH_WINDOWS_TIMEOUT_INITIALIZER(clock)                            \
	[DISPATCH_CLOCK_##clock] = {                                           \
		.pTimer = NULL,                                                \
		.ullIdent = DISPATCH_PORT_TIMER_CLOCK_##clock,                 \
		.bArmed = FALSE,                                               \
	}
static struct _dispatch_windows_timeout_s _dispatch_windows_timeout[] = {
//	DISPATCH_WINDOWS_TIMEOUT_INITIALIZER(WALL),
	DISPATCH_WINDOWS_TIMEOUT_INITIALIZER(UPTIME),
//	DISPATCH_WINDOWS_TIMEOUT_INITIALIZER(MONOTONIC),
};

#pragma mark Helpers

#pragma mark dispatch_unote_t

bool
_dispatch_unote_register_muxed(dispatch_unote_t du DISPATCH_UNUSED)
{
	WIN_PORT_ERROR();
	return false;
}

void
_dispatch_unote_resume_muxed(dispatch_unote_t du DISPATCH_UNUSED)
{
	WIN_PORT_ERROR();
}

bool
_dispatch_unote_unregister_muxed(dispatch_unote_t du DISPATCH_UNUSED)
{
	WIN_PORT_ERROR();
	return false;
}

#pragma mark timers

static void CALLBACK
_dispatch_timer_callback(PTP_CALLBACK_INSTANCE Instance, PVOID Context,
	PTP_TIMER Timer)
{
	BOOL bSuccess;

	bSuccess = PostQueuedCompletionStatus(hPort, 0, (ULONG_PTR)Context, NULL);
	if (bSuccess == FALSE)
		DISPATCH_INTERNAL_CRASH(GetLastError(), "PostQueuedCompletionStatus");
}

void
_dispatch_event_loop_timer_arm(dispatch_timer_heap_t dth DISPATCH_UNUSED,
		uint32_t tidx, dispatch_timer_delay_s range,
		dispatch_clock_now_cache_t nows DISPATCH_UNUSED)
{
	dispatch_windows_timeout_t timer;
	FILETIME ftDueTime;
	LARGE_INTEGER ulTime;

	switch (DISPATCH_TIMER_CLOCK(tidx)) {
	case DISPATCH_CLOCK_UPTIME:
		timer = &_dispatch_windows_timeout[DISPATCH_CLOCK_UPTIME];
		ulTime.QuadPart = -((range.delay + 99) / 100);
		break;
	default:
		WIN_PORT_ERROR();
		__assume(0);
	}

	if (timer->pTimer == NULL)
	{
		timer->pTimer = CreateThreadpoolTimer(_dispatch_timer_callback,
				(LPVOID)timer->ullIdent, NULL);
		if (timer->pTimer == NULL)
			DISPATCH_INTERNAL_CRASH(GetLastError(), "CreateThreadpoolTimer");
	}

	ftDueTime.dwHighDateTime = ulTime.HighPart;
	ftDueTime.dwLowDateTime = ulTime.LowPart;

	SetThreadpoolTimer(timer->pTimer, &ftDueTime, /*msPeriod=*/0,
		/*msWindowLength=*/0);
	timer->bArmed = TRUE;
}

void
_dispatch_event_loop_timer_delete(dispatch_timer_heap_t dth DISPATCH_UNUSED,
		uint32_t tidx)
{
	dispatch_windows_timeout_t timer;

	switch (DISPATCH_TIMER_CLOCK(tidx)) {
	case DISPATCH_CLOCK_UPTIME:
		timer = &_dispatch_windows_timeout[DISPATCH_PORT_TIMER_CLOCK_UPTIME];
		break;
	default:
		WIN_PORT_ERROR();
		__assume(0);
	}

	SetThreadpoolTimer(timer->pTimer, NULL, /*msPeriod=*/0,
		/*msWindowLength=*/0);
	timer->bArmed = FALSE;
}

#pragma mark dispatch_loop

static void
_dispatch_port_init(void *context DISPATCH_UNUSED)
{
	hPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	if (hPort == NULL) {
		DISPATCH_INTERNAL_CRASH(GetLastError(), "CreateIoCompletionPort");
	}

#if DISPATCH_USE_MGR_THREAD
	_dispatch_trace_item_push(_dispatch_mgr_q.do_targetq, &_dispatch_mgr_q);
	dx_push(_dispatch_mgr_q.do_targetq, &_dispatch_mgr_q, 0);
#endif
}

void
_dispatch_event_loop_poke(dispatch_wlh_t wlh DISPATCH_UNUSED,
		uint64_t dq_state DISPATCH_UNUSED, uint32_t flags DISPATCH_UNUSED)
{
	static dispatch_once_t _dispatch_port_pred;
	dispatch_once_f(&_dispatch_port_pred, NULL, _dispatch_port_init);
	BOOL bResult = PostQueuedCompletionStatus(hPort, 0, DISPATCH_PORT_POKE,
		NULL);
	(void)dispatch_assume(bResult);
}

static void
_dispatch_event_merge_timer(dispatch_clock_t clock)
{
	dispatch_timer_heap_t dth = _dispatch_timers_heap;
	uint32_t tidx = DISPATCH_TIMER_INDEX(clock, 0);

	_dispatch_windows_timeout[clock].bArmed = FALSE;

	_dispatch_timers_heap_dirty(dth, tidx);
	dth[tidx].dth_needs_program = true;
	dth[tidx].dth_armed = false;
}

DISPATCH_NOINLINE
void
_dispatch_event_loop_drain(uint32_t flags)
{
	DWORD dwNumberOfBytesTransferred;
	ULONG_PTR ulCompletionKey;
	LPOVERLAPPED pOV;

	pOV = (LPOVERLAPPED)&pOV;
	BOOL bResult = GetQueuedCompletionStatus(hPort,
		&dwNumberOfBytesTransferred, &ulCompletionKey, &pOV,
		(flags & KEVENT_FLAG_IMMEDIATE) ? 0 : INFINITE);

	while (bResult)
	{
		switch (ulCompletionKey)
		{
		case DISPATCH_PORT_POKE:
			break;
		case DISPATCH_PORT_TIMER_CLOCK_UPTIME:
                        _dispatch_event_merge_timer(DISPATCH_CLOCK_UPTIME);
			break;
		}

		bResult = GetQueuedCompletionStatus(hPort,
			&dwNumberOfBytesTransferred, &ulCompletionKey, &pOV, 0);
	}

	if (bResult == FALSE && pOV != NULL)
		DISPATCH_INTERNAL_CRASH(GetLastError(), "GetQueuedCompletionStatus");
}

void
_dispatch_event_loop_cancel_waiter(dispatch_sync_context_t dsc DISPATCH_UNUSED)
{
	WIN_PORT_ERROR();
}

void
_dispatch_event_loop_wake_owner(dispatch_sync_context_t dsc,
		dispatch_wlh_t wlh, uint64_t old_state, uint64_t new_state)
{
	(void)dsc; (void)wlh; (void)old_state; (void)new_state;
}

void
_dispatch_event_loop_wait_for_ownership(dispatch_sync_context_t dsc)
{
	if (dsc->dsc_release_storage) {
		_dispatch_queue_release_storage(dsc->dc_data);
	}
}

void
_dispatch_event_loop_end_ownership(dispatch_wlh_t wlh, uint64_t old_state,
		uint64_t new_state, uint32_t flags)
{
	(void)wlh; (void)old_state; (void)new_state; (void)flags;
}

#if DISPATCH_WLH_DEBUG
void
_dispatch_event_loop_assert_not_owned(dispatch_wlh_t wlh)
{
	(void)wlh;
}
#endif

void
_dispatch_event_loop_leave_immediate(uint64_t dq_state)
{
	(void)dq_state;
}

#endif // DISPATCH_EVENT_BACKEND_WINDOWS
