/*
 * Copyright (c) 2016 Apple Inc. All rights reserved.
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
#if DISPATCH_EVENT_BACKEND_EPOLL
#include <linux/sockios.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#ifndef EPOLLFREE
#define EPOLLFREE 0x4000
#endif

#if !DISPATCH_USE_MGR_THREAD
#error unsupported configuration
#endif

#define DISPATCH_EPOLL_MAX_EVENT_COUNT 16

enum {
	DISPATCH_EPOLL_EVENTFD    = 0x0001,
	DISPATCH_EPOLL_CLOCK_WALL = 0x0002,
	DISPATCH_EPOLL_CLOCK_MACH = 0x0003,
};

typedef struct dispatch_muxnote_s {
	TAILQ_ENTRY(dispatch_muxnote_s) dmn_list;
	TAILQ_HEAD(, dispatch_unote_linkage_s) dmn_readers_head;
	TAILQ_HEAD(, dispatch_unote_linkage_s) dmn_writers_head;
	int     dmn_fd;
	uint32_t dmn_ident;
	uint32_t dmn_events;
	int16_t dmn_filter;
	bool    dmn_skip_outq_ioctl;
	bool    dmn_skip_inq_ioctl;
} *dispatch_muxnote_t;

typedef struct dispatch_epoll_timeout_s {
	int       det_fd;
	uint16_t  det_ident;
	bool      det_registered;
	bool      det_armed;
} *dispatch_epoll_timeout_t;

static int _dispatch_epfd, _dispatch_eventfd;

static dispatch_once_t epoll_init_pred;
static void _dispatch_epoll_init(void *);

DISPATCH_CACHELINE_ALIGN
static TAILQ_HEAD(dispatch_muxnote_bucket_s, dispatch_muxnote_s)
_dispatch_sources[DSL_HASH_SIZE];

#define DISPATCH_EPOLL_TIMEOUT_INITIALIZER(clock) \
	[DISPATCH_CLOCK_##clock] = { \
		.det_fd = -1, \
		.det_ident = DISPATCH_EPOLL_CLOCK_##clock, \
	}
static struct dispatch_epoll_timeout_s _dispatch_epoll_timeout[] = {
	DISPATCH_EPOLL_TIMEOUT_INITIALIZER(WALL),
	DISPATCH_EPOLL_TIMEOUT_INITIALIZER(MACH),
};

#pragma mark dispatch_muxnote_t

DISPATCH_ALWAYS_INLINE
static inline struct dispatch_muxnote_bucket_s *
_dispatch_muxnote_bucket(uint32_t ident)
{
	return &_dispatch_sources[DSL_HASH(ident)];
}
#define _dispatch_unote_muxnote_bucket(du) \
	_dispatch_muxnote_bucket(du._du->du_ident)

DISPATCH_ALWAYS_INLINE
static inline dispatch_muxnote_t
_dispatch_muxnote_find(struct dispatch_muxnote_bucket_s *dmb,
		uint32_t ident, int16_t filter)
{
	dispatch_muxnote_t dmn;
	if (filter == EVFILT_WRITE) filter = EVFILT_READ;
	TAILQ_FOREACH(dmn, dmb, dmn_list) {
		if (dmn->dmn_ident == ident && dmn->dmn_filter == filter) {
			break;
		}
	}
	return dmn;
}
#define _dispatch_unote_muxnote_find(dmb, du) \
		_dispatch_muxnote_find(dmb, du._du->du_ident, du._du->du_filter)

static void
_dispatch_muxnote_dispose(dispatch_muxnote_t dmn)
{
	if (dmn->dmn_filter != EVFILT_READ || (uint32_t)dmn->dmn_fd != dmn->dmn_ident) {
		close(dmn->dmn_fd);
	}
	free(dmn);
}

static pthread_t manager_thread;

static void
_dispatch_muxnote_signal_block_and_raise(int signo)
{
	// On linux, for signals to be delivered to the signalfd, signals
	// must be blocked, else any thread that hasn't them blocked may
	// receive them.  Fix that by lazily noticing, blocking said signal,
	// and raising the signal again when it happens
	_dispatch_sigmask();
	pthread_kill(manager_thread, signo);
}

static dispatch_muxnote_t
_dispatch_muxnote_create(dispatch_unote_t du, uint32_t events)
{
	static sigset_t signals_with_unotes;
	static struct sigaction sa = {
		.sa_handler = _dispatch_muxnote_signal_block_and_raise,
		.sa_flags = SA_RESTART,
	};

	dispatch_muxnote_t dmn;
	struct stat sb;
	int fd = (int)du._du->du_ident;
	int16_t filter = du._du->du_filter;
	bool skip_outq_ioctl = false, skip_inq_ioctl = false;
	sigset_t sigmask;

	switch (filter) {
	case EVFILT_SIGNAL: {
		int signo = (int)du._du->du_ident;
		if (!sigismember(&signals_with_unotes, signo)) {
			manager_thread = pthread_self();
			sigaddset(&signals_with_unotes, signo);
			sigaction(signo, &sa, NULL);
		}
		sigemptyset(&sigmask);
		sigaddset(&sigmask, signo);
		fd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
		if (fd < 0) {
			return NULL;
		}
		break;
	}
	case EVFILT_WRITE:
		filter = EVFILT_READ;
	case EVFILT_READ:
		if (fstat(fd, &sb) < 0) {
			return NULL;
		}
		if (S_ISREG(sb.st_mode)) {
			// make a dummy fd that is both readable & writeable
			fd = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
			if (fd < 0) {
				return NULL;
			}
			// Linux doesn't support output queue size ioctls for regular files
			skip_outq_ioctl = true;
		} else if (S_ISSOCK(sb.st_mode)) {
			socklen_t vlen = sizeof(int);
			int v;
			// Linux doesn't support saying how many clients are ready to be
			// accept()ed for sockets
			if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &v, &vlen) == 0) {
				skip_inq_ioctl = (bool)v;
			}
		}
		break;

	default:
		DISPATCH_INTERNAL_CRASH(0, "Unexpected filter");
	}

	dmn = _dispatch_calloc(1, sizeof(struct dispatch_muxnote_s));
	TAILQ_INIT(&dmn->dmn_readers_head);
	TAILQ_INIT(&dmn->dmn_writers_head);
	dmn->dmn_fd = fd;
	dmn->dmn_ident = du._du->du_ident;
	dmn->dmn_filter = filter;
	dmn->dmn_events = events;
	dmn->dmn_skip_outq_ioctl = skip_outq_ioctl;
	dmn->dmn_skip_inq_ioctl = skip_inq_ioctl;
	return dmn;
}

#pragma mark dispatch_unote_t

static int
_dispatch_epoll_update(dispatch_muxnote_t dmn, int op)
{
	dispatch_once_f(&epoll_init_pred, NULL, _dispatch_epoll_init);
	struct epoll_event ev = {
		.events = dmn->dmn_events,
		.data = { .ptr = dmn },
	};
	return epoll_ctl(_dispatch_epfd, op, dmn->dmn_fd, &ev);
}

bool
_dispatch_unote_register(dispatch_unote_t du,
		DISPATCH_UNUSED dispatch_wlh_t wlh, dispatch_priority_t pri)
{
	struct dispatch_muxnote_bucket_s *dmb;
	dispatch_muxnote_t dmn;
	uint32_t events = EPOLLFREE;

	dispatch_assert(!_dispatch_unote_registered(du));
	du._du->du_priority = pri;

	switch (du._du->du_filter) {
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
	case DISPATCH_EVFILT_CUSTOM_REPLACE:
		du._du->du_wlh = DISPATCH_WLH_ANON;
		return true;
	case EVFILT_WRITE:
		events |= EPOLLOUT;
		break;
	default:
		events |= EPOLLIN;
		break;
	}

	if (du._du->du_type->dst_flags & EV_DISPATCH) {
		events |= EPOLLONESHOT;
	}

	dmb = _dispatch_unote_muxnote_bucket(du);
	dmn = _dispatch_unote_muxnote_find(dmb, du);
	if (dmn) {
		events &= ~dmn->dmn_events;
		if (events) {
			dmn->dmn_events |= events;
			if (_dispatch_epoll_update(dmn, EPOLL_CTL_MOD) < 0) {
				dmn->dmn_events &= ~events;
				dmn = NULL;
			}
		}
	} else {
		dmn = _dispatch_muxnote_create(du, events);
		if (_dispatch_epoll_update(dmn, EPOLL_CTL_ADD) < 0) {
			_dispatch_muxnote_dispose(dmn);
			dmn = NULL;
		} else {
			TAILQ_INSERT_TAIL(dmb, dmn, dmn_list);
		}
	}

	if (dmn) {
		dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);
		if (events & EPOLLOUT) {
			TAILQ_INSERT_TAIL(&dmn->dmn_writers_head, dul, du_link);
		} else {
			TAILQ_INSERT_TAIL(&dmn->dmn_readers_head, dul, du_link);
		}
		dul->du_muxnote = dmn;
		dispatch_assert(du._du->du_wlh == NULL);
		du._du->du_wlh = DISPATCH_WLH_ANON;
	}
	return dmn != NULL;
}

void
_dispatch_unote_resume(dispatch_unote_t du)
{
	dispatch_muxnote_t dmn = _dispatch_unote_get_linkage(du)->du_muxnote;
	dispatch_assert(_dispatch_unote_registered(du));

	_dispatch_epoll_update(dmn, EPOLL_CTL_MOD);
}

bool
_dispatch_unote_unregister(dispatch_unote_t du, DISPATCH_UNUSED uint32_t flags)
{
	switch (du._du->du_filter) {
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
	case DISPATCH_EVFILT_CUSTOM_REPLACE:
		du._du->du_wlh = NULL;
		return true;
	}
	if (_dispatch_unote_registered(du)) {
		dispatch_unote_linkage_t dul = _dispatch_unote_get_linkage(du);
		dispatch_muxnote_t dmn = dul->du_muxnote;
		uint32_t events = dmn->dmn_events;

		if (du._du->du_filter == EVFILT_WRITE) {
			TAILQ_REMOVE(&dmn->dmn_writers_head, dul, du_link);
		} else {
			TAILQ_REMOVE(&dmn->dmn_readers_head, dul, du_link);
		}
		_TAILQ_TRASH_ENTRY(dul, du_link);
		dul->du_muxnote = NULL;

		if (TAILQ_EMPTY(&dmn->dmn_readers_head)) {
			events &= (uint32_t)(~EPOLLIN);
		}
		if (TAILQ_EMPTY(&dmn->dmn_writers_head)) {
			events &= (uint32_t)(~EPOLLOUT);
		}

		if (events == dmn->dmn_events) {
			// nothing to do
		} else if (events & (EPOLLIN | EPOLLOUT)) {
			dmn->dmn_events = events;
			_dispatch_epoll_update(dmn, EPOLL_CTL_MOD);
		} else {
			epoll_ctl(_dispatch_epfd, EPOLL_CTL_DEL, dmn->dmn_fd, NULL);
			TAILQ_REMOVE(_dispatch_unote_muxnote_bucket(du), dmn, dmn_list);
			_dispatch_muxnote_dispose(dmn);
		}
		dispatch_assert(du._du->du_wlh == DISPATCH_WLH_ANON);
		du._du->du_wlh = NULL;
	}
	return true;
}

#pragma mark timers

static void
_dispatch_event_merge_timer(dispatch_clock_t clock)
{
	_dispatch_timers_expired = true;
	_dispatch_timers_processing_mask |= 1 << DISPATCH_TIMER_INDEX(clock, 0);
#if DISPATCH_USE_DTRACE
	_dispatch_timers_will_wake |= 1 << 0;
#endif
	_dispatch_epoll_timeout[clock].det_armed = false;
	_dispatch_timers_heap[clock].dth_flags &= ~DTH_ARMED;
}

static void
_dispatch_timeout_program(uint32_t tidx, uint64_t target,
		DISPATCH_UNUSED uint64_t leeway)
{
	dispatch_clock_t clock = DISPATCH_TIMER_CLOCK(tidx);
	dispatch_epoll_timeout_t timer = &_dispatch_epoll_timeout[clock];
	struct epoll_event ev = {
		.events = EPOLLONESHOT | EPOLLIN,
		.data = { .u32 = timer->det_ident },
	};
	int op;

	if (target >= INT64_MAX && !timer->det_registered) {
		return;
	}

	if (unlikely(timer->det_fd < 0)) {
		clockid_t clockid;
		int fd;
		switch (DISPATCH_TIMER_CLOCK(tidx)) {
		case DISPATCH_CLOCK_MACH:
			clockid = CLOCK_MONOTONIC;
			break;
		case DISPATCH_CLOCK_WALL:
			clockid = CLOCK_REALTIME;
			break;
		}
		fd = timerfd_create(clockid, TFD_NONBLOCK | TFD_CLOEXEC);
		if (!dispatch_assume(fd >= 0)) {
			return;
		}
		timer->det_fd = fd;
	}

	if (target < INT64_MAX) {
		struct itimerspec its = { .it_value = {
			.tv_sec  = target / NSEC_PER_SEC,
			.tv_nsec = target % NSEC_PER_SEC,
		} };
		dispatch_assume_zero(timerfd_settime(timer->det_fd, TFD_TIMER_ABSTIME,
				&its, NULL));
		if (!timer->det_registered) {
			op = EPOLL_CTL_ADD;
		} else if (!timer->det_armed) {
			op = EPOLL_CTL_MOD;
		} else {
			return;
		}
	} else {
		op = EPOLL_CTL_DEL;
	}
	dispatch_assume_zero(epoll_ctl(_dispatch_epfd, op, timer->det_fd, &ev));
	timer->det_armed = timer->det_registered = (op != EPOLL_CTL_DEL);;
}

void
_dispatch_event_loop_timer_arm(uint32_t tidx, dispatch_timer_delay_s range,
		dispatch_clock_now_cache_t nows)
{
	uint64_t target = range.delay;
	target += _dispatch_time_now_cached(DISPATCH_TIMER_CLOCK(tidx), nows);
	_dispatch_timers_heap[tidx].dth_flags |= DTH_ARMED;
	_dispatch_timeout_program(tidx, target, range.leeway);
}

void
_dispatch_event_loop_timer_delete(uint32_t tidx)
{
	_dispatch_timers_heap[tidx].dth_flags &= ~DTH_ARMED;
	_dispatch_timeout_program(tidx, UINT64_MAX, UINT64_MAX);
}

#pragma mark dispatch_loop

void
_dispatch_event_loop_atfork_child(void)
{
}

static void
_dispatch_epoll_init(void *context DISPATCH_UNUSED)
{
	_dispatch_fork_becomes_unsafe();

	unsigned int i;
	for (i = 0; i < DSL_HASH_SIZE; i++) {
		TAILQ_INIT(&_dispatch_sources[i]);
	}

	_dispatch_epfd = epoll_create1(EPOLL_CLOEXEC);
	if (_dispatch_epfd < 0) {
		DISPATCH_INTERNAL_CRASH(errno, "epoll_create1() failed");
	}

	_dispatch_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (_dispatch_eventfd < 0) {
		DISPATCH_INTERNAL_CRASH(errno, "epoll_eventfd() failed");
	}

	struct epoll_event ev = {
		.events = EPOLLIN | EPOLLFREE,
		.data = { .u32 = DISPATCH_EPOLL_EVENTFD, },
	};
	int op = EPOLL_CTL_ADD;
	if (epoll_ctl(_dispatch_epfd, op, _dispatch_eventfd, &ev) < 0) {
		DISPATCH_INTERNAL_CRASH(errno, "epoll_ctl() failed");
	}

#if DISPATCH_USE_MGR_THREAD
	dx_push(_dispatch_mgr_q.do_targetq, &_dispatch_mgr_q, 0);
#endif
}

void
_dispatch_event_loop_poke(dispatch_wlh_t wlh DISPATCH_UNUSED,
		uint64_t dq_state DISPATCH_UNUSED, uint32_t flags DISPATCH_UNUSED)
{
	dispatch_once_f(&epoll_init_pred, NULL, _dispatch_epoll_init);
	dispatch_assume_zero(eventfd_write(_dispatch_eventfd, 1));
}

static void
_dispatch_event_merge_signal(dispatch_muxnote_t dmn)
{
	dispatch_unote_linkage_t dul, dul_next;
	struct signalfd_siginfo si;
	ssize_t rc;

	// Linux has the weirdest semantics around signals: if it finds a thread
	// that has not masked a process wide-signal, it may deliver it to this
	// thread, meaning that the signalfd may have been made readable, but the
	// signal consumed through the legacy delivery mechanism.
	//
	// Because of this we can get a misfire of the signalfd yielding EAGAIN the
	// first time around. The _dispatch_muxnote_signal_block_and_raise() hack
	// will kick in, the thread with the wrong mask will be fixed up, and the
	// signal delivered to us again properly.
	if ((rc = read(dmn->dmn_fd, &si, sizeof(si))) == sizeof(si)) {
		TAILQ_FOREACH_SAFE(dul, &dmn->dmn_readers_head, du_link, dul_next) {
			dispatch_unote_t du = _dispatch_unote_linkage_get_unote(dul);
			dux_merge_evt(du._du, EV_ADD|EV_ENABLE|EV_CLEAR, 1, 0, 0);
		}
	} else {
		dispatch_assume(rc == -1 && errno == EAGAIN);
	}
}

static uintptr_t
_dispatch_get_buffer_size(dispatch_muxnote_t dmn, bool writer)
{
	int n;

	if (writer ? dmn->dmn_skip_outq_ioctl : dmn->dmn_skip_inq_ioctl) {
		return 1;
	}

	if (ioctl((int)dmn->dmn_ident, writer ? SIOCOUTQ : SIOCINQ, &n) != 0) {
		switch (errno) {
		case EINVAL:
		case ENOTTY:
			// this file descriptor actually doesn't support the buffer
			// size ioctl, remember that for next time to avoid the syscall.
			break;
		default:
			dispatch_assume_zero(errno);
			break;
		}
		if (writer) {
			dmn->dmn_skip_outq_ioctl = true;
		} else {
			dmn->dmn_skip_inq_ioctl = true;
		}
		return 1;
	}
	return (uintptr_t)n;
}

static void
_dispatch_event_merge_fd(dispatch_muxnote_t dmn, uint32_t events)
{
	dispatch_unote_linkage_t dul, dul_next;
	uintptr_t data;

	if (events & EPOLLIN) {
		data = _dispatch_get_buffer_size(dmn, false);
		TAILQ_FOREACH_SAFE(dul, &dmn->dmn_readers_head, du_link, dul_next) {
			dispatch_unote_t du = _dispatch_unote_linkage_get_unote(dul);
			dux_merge_evt(du._du, EV_ADD|EV_ENABLE|EV_DISPATCH, ~data, 0, 0);
		}
	}

	if (events & EPOLLOUT) {
		data = _dispatch_get_buffer_size(dmn, true);
		TAILQ_FOREACH_SAFE(dul, &dmn->dmn_writers_head, du_link, dul_next) {
			dispatch_unote_t du = _dispatch_unote_linkage_get_unote(dul);
			dux_merge_evt(du._du, EV_ADD|EV_ENABLE|EV_DISPATCH, ~data, 0, 0);
		}
	}
}

DISPATCH_NOINLINE
void
_dispatch_event_loop_drain(uint32_t flags)
{
	struct epoll_event ev[DISPATCH_EPOLL_MAX_EVENT_COUNT];
	int i, r;
	int timeout = (flags & KEVENT_FLAG_IMMEDIATE) ? 0 : -1;

retry:
	r = epoll_wait(_dispatch_epfd, ev, countof(ev), timeout);
	if (unlikely(r == -1)) {
		int err = errno;
		switch (err) {
		case EINTR:
			goto retry;
		case EBADF:
			DISPATCH_CLIENT_CRASH(err, "Do not close random Unix descriptors");
			break;
		default:
			(void)dispatch_assume_zero(err);
			break;
		}
		return;
	}

	for (i = 0; i < r; i++) {
		dispatch_muxnote_t dmn;
		eventfd_t value;

		if (ev[i].events & EPOLLFREE) {
			DISPATCH_CLIENT_CRASH(0, "Do not close random Unix descriptors");
		}

		switch (ev[i].data.u32) {
		case DISPATCH_EPOLL_EVENTFD:
			dispatch_assume_zero(eventfd_read(_dispatch_eventfd, &value));
			break;

		case DISPATCH_EPOLL_CLOCK_WALL:
			_dispatch_event_merge_timer(DISPATCH_CLOCK_WALL);
			break;

		case DISPATCH_EPOLL_CLOCK_MACH:
			_dispatch_event_merge_timer(DISPATCH_CLOCK_MACH);
			break;

		default:
			dmn = ev[i].data.ptr;
			switch (dmn->dmn_filter) {
			case EVFILT_SIGNAL:
				_dispatch_event_merge_signal(dmn);
				break;

			case EVFILT_READ:
				_dispatch_event_merge_fd(dmn, ev[i].events);
				break;
			}
		}
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

#endif // DISPATCH_EVENT_BACKEND_EPOLL
