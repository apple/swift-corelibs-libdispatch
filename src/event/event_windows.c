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
enum _dispatch_windows_port {
	DISPATCH_PORT_POKE = 0,
	DISPATCH_PORT_TIMER_CLOCK_WALL,
	DISPATCH_PORT_TIMER_CLOCK_UPTIME,
	DISPATCH_PORT_TIMER_CLOCK_MONOTONIC,
};

#pragma mark dispatch_unote_t

bool
_dispatch_unote_register(dispatch_unote_t du DISPATCH_UNUSED,
		dispatch_wlh_t wlh DISPATCH_UNUSED,
		dispatch_priority_t pri DISPATCH_UNUSED)
{
	WIN_PORT_ERROR();
	return false;
}

void
_dispatch_unote_resume(dispatch_unote_t du DISPATCH_UNUSED)
{
	WIN_PORT_ERROR();
}

bool
_dispatch_unote_unregister(dispatch_unote_t du DISPATCH_UNUSED,
		uint32_t flags DISPATCH_UNUSED)
{
	WIN_PORT_ERROR();
	return false;
}

#pragma mark timers

typedef struct _dispatch_windows_timeout_s {
	PTP_TIMER pTimer;
	enum _dispatch_windows_port ullIdent;
	bool bArmed;
} *dispatch_windows_timeout_t;

#define DISPATCH_WINDOWS_TIMEOUT_INITIALIZER(clock)                             \
	[DISPATCH_CLOCK_##clock] = {                                            \
		.pTimer = NULL,                                                 \
		.ullIdent = DISPATCH_PORT_TIMER_CLOCK_##clock,                  \
		.bArmed = FALSE,                                                \
	}

static struct _dispatch_windows_timeout_s _dispatch_windows_timeout[] = {
	DISPATCH_WINDOWS_TIMEOUT_INITIALIZER(WALL),
	DISPATCH_WINDOWS_TIMEOUT_INITIALIZER(UPTIME),
	DISPATCH_WINDOWS_TIMEOUT_INITIALIZER(MONOTONIC),
};

static void
_dispatch_event_merge_timer(dispatch_clock_t clock)
{
	uint32_t tidx = DISPATCH_TIMER_INDEX(clock, 0);

	_dispatch_windows_timeout[clock].bArmed = FALSE;

	_dispatch_timers_heap_dirty(_dispatch_timers_heap, tidx);
	_dispatch_timers_heap[tidx].dth_needs_program = true;
	_dispatch_timers_heap[tidx].dth_armed = false;
}

static void CALLBACK
_dispatch_timer_callback(PTP_CALLBACK_INSTANCE Instance, PVOID Context,
	PTP_TIMER Timer)
{
	BOOL bSuccess;

	bSuccess = PostQueuedCompletionStatus(hPort, 0, (ULONG_PTR)Context,
		NULL);
	if (bSuccess == FALSE) {
		DISPATCH_INTERNAL_CRASH(GetLastError(),
			"PostQueuedCompletionStatus");
	}
}

void
_dispatch_event_loop_timer_arm(uint32_t tidx,
		dispatch_timer_delay_s range,
		dispatch_clock_now_cache_t nows)
{
	dispatch_windows_timeout_t timer;
	FILETIME ftDueTime;
	LARGE_INTEGER liTime;

	switch (DISPATCH_TIMER_CLOCK(tidx)) {
	case DISPATCH_CLOCK_WALL:
		timer = &_dispatch_windows_timeout[DISPATCH_CLOCK_WALL];
		liTime.QuadPart = range.delay +
			_dispatch_time_now_cached(DISPATCH_TIMER_CLOCK(tidx), nows);
		break;

	case DISPATCH_CLOCK_UPTIME:
	case DISPATCH_CLOCK_MONOTONIC:
		timer = &_dispatch_windows_timeout[DISPATCH_TIMER_CLOCK(tidx)];
		liTime.QuadPart = -((range.delay + 99) / 100);
		break;
	}

	if (timer->pTimer == NULL) {
		timer->pTimer = CreateThreadpoolTimer(_dispatch_timer_callback,
			(LPVOID)timer->ullIdent, NULL);
		if (timer->pTimer == NULL) {
			DISPATCH_INTERNAL_CRASH(GetLastError(),
				"CreateThreadpoolTimer");
		}
	}

	ftDueTime.dwHighDateTime = liTime.HighPart;
	ftDueTime.dwLowDateTime = liTime.LowPart;

	SetThreadpoolTimer(timer->pTimer, &ftDueTime, /*msPeriod=*/0,
		/*msWindowLength=*/0);
	timer->bArmed = TRUE;
}

void
_dispatch_event_loop_timer_delete(uint32_t tidx)
{
	dispatch_windows_timeout_t timer;

	switch (DISPATCH_TIMER_CLOCK(tidx)) {
	case DISPATCH_CLOCK_WALL:
		timer = &_dispatch_windows_timeout[DISPATCH_CLOCK_WALL];
		break;

	case DISPATCH_CLOCK_UPTIME:
	case DISPATCH_CLOCK_MONOTONIC:
		timer = &_dispatch_windows_timeout[DISPATCH_TIMER_CLOCK(tidx)];
		break;
	}

	SetThreadpoolTimer(timer->pTimer, NULL, /*msPeriod=*/0,
		/*msWindowLength=*/0);
	timer->bArmed = FALSE;
}

#pragma mark dispatch_loop

static void
_dispatch_windows_port_init(void *context DISPATCH_UNUSED)
{
	hPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	if (hPort == NULL) {
		DISPATCH_INTERNAL_CRASH(GetLastError(),
			"CreateIoCompletionPort");
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
	static dispatch_once_t _dispatch_windows_port_init_pred;
	BOOL bSuccess;

	dispatch_once_f(&_dispatch_windows_port_init_pred, NULL,
		_dispatch_windows_port_init);
	bSuccess = PostQueuedCompletionStatus(hPort, 0, DISPATCH_PORT_POKE,
		NULL);
	(void)dispatch_assume(bSuccess);
}

DISPATCH_NOINLINE
void
_dispatch_event_loop_drain(uint32_t flags)
{
	DWORD dwNumberOfBytesTransferred;
	ULONG_PTR ulCompletionKey;
	LPOVERLAPPED pOV;
	BOOL bSuccess;

	pOV = (LPOVERLAPPED)&pOV;
	bSuccess = GetQueuedCompletionStatus(hPort, &dwNumberOfBytesTransferred,
		&ulCompletionKey, &pOV,
		(flags & KEVENT_FLAG_IMMEDIATE) ? 0 : INFINITE);
	while (bSuccess) {
		switch (ulCompletionKey) {
		case DISPATCH_PORT_POKE:
			break;

		case DISPATCH_PORT_TIMER_CLOCK_WALL:
			_dispatch_event_merge_timer(DISPATCH_CLOCK_WALL);
			break;

		case DISPATCH_PORT_TIMER_CLOCK_UPTIME:
			_dispatch_event_merge_timer(DISPATCH_CLOCK_UPTIME);
			break;

		case DISPATCH_PORT_TIMER_CLOCK_MONOTONIC:
			_dispatch_event_merge_timer(DISPATCH_CLOCK_MONOTONIC);
			break;

		default:
			DISPATCH_INTERNAL_CRASH(ulCompletionKey,
				"unsupported completion key");
		}

		bSuccess = GetQueuedCompletionStatus(hPort,
			&dwNumberOfBytesTransferred, &ulCompletionKey, &pOV, 0);
	}

	if (bSuccess == FALSE && pOV != NULL) {
		DISPATCH_INTERNAL_CRASH(GetLastError(),
			"GetQueuedCompletionStatus");
	}
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
_dispatch_event_loop_leave_immediate(dispatch_wlh_t wlh, uint64_t dq_state)
{
	(void)wlh; (void)dq_state;
}

#endif // DISPATCH_EVENT_BACKEND_WINDOWS
