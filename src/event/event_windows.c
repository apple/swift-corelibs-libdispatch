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
	DISPATCH_PORT_FILE_HANDLE,
};

#pragma mark dispatch_unote_t

typedef struct dispatch_muxnote_s {
	LIST_ENTRY(dispatch_muxnote_s) dmn_list;
	dispatch_unote_ident_t dmn_ident;
	int8_t dmn_filter;
	enum _dispatch_muxnote_handle_type {
		DISPATCH_MUXNOTE_HANDLE_TYPE_INVALID,
		DISPATCH_MUXNOTE_HANDLE_TYPE_FILE,
	} dmn_handle_type;
} *dispatch_muxnote_t;

static LIST_HEAD(dispatch_muxnote_bucket_s, dispatch_muxnote_s)
    _dispatch_sources[DSL_HASH_SIZE];

static SRWLOCK _dispatch_file_handles_lock = SRWLOCK_INIT;
static LIST_HEAD(, dispatch_unote_linkage_s) _dispatch_file_handles;

DISPATCH_ALWAYS_INLINE
static inline struct dispatch_muxnote_bucket_s *
_dispatch_unote_muxnote_bucket(uint32_t ident)
{
	return &_dispatch_sources[DSL_HASH(ident)];
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_muxnote_t
_dispatch_unote_muxnote_find(struct dispatch_muxnote_bucket_s *dmb,
		dispatch_unote_ident_t ident, int8_t filter)
{
	dispatch_muxnote_t dmn;
	if (filter == EVFILT_WRITE) filter = EVFILT_READ;
	LIST_FOREACH(dmn, dmb, dmn_list) {
		if (dmn->dmn_ident == ident && dmn->dmn_filter == filter) {
			break;
		}
	}
	return dmn;
}

static dispatch_muxnote_t
_dispatch_muxnote_create(dispatch_unote_t du)
{
	dispatch_muxnote_t dmn;
	int8_t filter = du._du->du_filter;
	HANDLE handle = (HANDLE)du._du->du_ident;

	dmn = _dispatch_calloc(1, sizeof(*dmn));
	if (dmn == NULL) {
		DISPATCH_INTERNAL_CRASH(0, "_dispatch_calloc");
	}
	dmn->dmn_ident = (dispatch_unote_ident_t)handle;
	dmn->dmn_filter = filter;

	switch (filter) {
	case EVFILT_SIGNAL:
		WIN_PORT_ERROR();

	case EVFILT_WRITE:
	case EVFILT_READ:
		switch (GetFileType(handle)) {
		case FILE_TYPE_UNKNOWN:
			// ensure that an invalid handle was not passed
			(void)dispatch_assume(GetLastError() == NO_ERROR);
			DISPATCH_INTERNAL_CRASH(0, "unknown handle type");

		case FILE_TYPE_REMOTE:
			DISPATCH_INTERNAL_CRASH(0, "unused handle type");

		case FILE_TYPE_CHAR:
			// The specified file is a character file, typically a
			// LPT device or a console.
			WIN_PORT_ERROR();

		case FILE_TYPE_DISK:
			// The specified file is a disk file
			dmn->dmn_handle_type =
				DISPATCH_MUXNOTE_HANDLE_TYPE_FILE;
			break;

		case FILE_TYPE_PIPE:
			// The specified file is a socket, a named pipe, or an
			// anonymous pipe.
			WIN_PORT_ERROR();
		}

		break;

	default:
		DISPATCH_INTERNAL_CRASH(0, "unexpected filter");
	}


	return dmn;
}

static void
_dispatch_muxnote_dispose(dispatch_muxnote_t dmn)
{
	free(dmn);
}

DISPATCH_ALWAYS_INLINE
static BOOL
_dispatch_io_trigger(dispatch_muxnote_t dmn)
{
	BOOL bSuccess;

	switch (dmn->dmn_handle_type) {
	case DISPATCH_MUXNOTE_HANDLE_TYPE_INVALID:
		DISPATCH_INTERNAL_CRASH(0, "invalid handle");

	case DISPATCH_MUXNOTE_HANDLE_TYPE_FILE:
		bSuccess = PostQueuedCompletionStatus(hPort, 0,
			(ULONG_PTR)DISPATCH_PORT_FILE_HANDLE, NULL);
		if (bSuccess == FALSE) {
			DISPATCH_INTERNAL_CRASH(GetLastError(),
				"PostQueuedCompletionStatus");
		}
		break;
	}

	return bSuccess;
}

bool
_dispatch_unote_register_muxed(dispatch_unote_t du)
{
	struct dispatch_muxnote_bucket_s *dmb;
	dispatch_muxnote_t dmn;

	dmb = _dispatch_unote_muxnote_bucket(du._du->du_ident);
	dmn = _dispatch_unote_muxnote_find(dmb, du._du->du_ident,
		du._du->du_filter);
	if (dmn) {
		WIN_PORT_ERROR();
	} else {
		dmn = _dispatch_muxnote_create(du);
		if (dmn) {
			if (_dispatch_io_trigger(dmn) == FALSE) {
				_dispatch_muxnote_dispose(dmn);
				dmn = NULL;
			} else {
				LIST_INSERT_HEAD(dmb, dmn, dmn_list);
			}
		}
	}

	if (dmn) {
		dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);

		AcquireSRWLockExclusive(&_dispatch_file_handles_lock);
		LIST_INSERT_HEAD(&_dispatch_file_handles, dul, du_link);
		ReleaseSRWLockExclusive(&_dispatch_file_handles_lock);

		dul->du_muxnote = dmn;
		_dispatch_unote_state_set(du, DISPATCH_WLH_ANON,
			DU_STATE_ARMED);
	}

	return dmn != NULL;
}

void
_dispatch_unote_resume_muxed(dispatch_unote_t du)
{
	dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);
	dispatch_muxnote_t dmn = dul->du_muxnote;
	dispatch_assert(_dispatch_unote_registered(du));
	_dispatch_io_trigger(dmn);
}

bool
_dispatch_unote_unregister_muxed(dispatch_unote_t du)
{
	dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);
	dispatch_muxnote_t dmn = dul->du_muxnote;

	AcquireSRWLockExclusive(&_dispatch_file_handles_lock);
	LIST_REMOVE(dul, du_link);
	_LIST_TRASH_ENTRY(dul, du_link);
	ReleaseSRWLockExclusive(&_dispatch_file_handles_lock);
	dul->du_muxnote = NULL;

	LIST_REMOVE(dmn, dmn_list);
	_dispatch_muxnote_dispose(dmn);

	_dispatch_unote_state_set(du, DU_STATE_UNREGISTERED);
	return true;
}

static void
_dispatch_event_merge_file_handle()
{
	dispatch_unote_linkage_t dul, dul_next;

	AcquireSRWLockExclusive(&_dispatch_file_handles_lock);
	LIST_FOREACH_SAFE(dul, &_dispatch_file_handles, du_link, dul_next) {
		dispatch_unote_t du = _dispatch_unote_linkage_get_unote(dul);

		// consumed by dux_merge_evt()
		_dispatch_retain_unote_owner(du);
		dispatch_assert(dux_needs_rearm(du._du));
		_dispatch_unote_state_clear_bit(du, DU_STATE_ARMED);
		os_atomic_store2o(du._dr, ds_pending_data, ~1, relaxed);
		dux_merge_evt(du._du, EV_ADD | EV_ENABLE | EV_DISPATCH, 1, 0);
	}
	ReleaseSRWLockExclusive(&_dispatch_file_handles_lock);
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
_dispatch_event_loop_timer_arm(dispatch_timer_heap_t dth DISPATCH_UNUSED,
		uint32_t tidx, dispatch_timer_delay_s range,
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
_dispatch_event_loop_timer_delete(dispatch_timer_heap_t dth DISPATCH_UNUSED,
		uint32_t tidx)
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

		case DISPATCH_PORT_FILE_HANDLE:
			_dispatch_event_merge_file_handle();
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
