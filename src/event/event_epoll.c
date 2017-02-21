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
	int     dmn_ident;
	int16_t dmn_filter;
	int16_t dmn_events;
	bool    dmn_socket_listener;
} *dispatch_muxnote_t;

typedef struct dispatch_epoll_timeout_s {
	int       det_fd;
	uint16_t  det_ident;
	bool      det_registered;
	bool      det_armed;
} *dispatch_epoll_timeout_t;

static int _dispatch_epfd, _dispatch_eventfd;
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
_dispatch_muxnote_bucket(int ident)
{
	return &_dispatch_sources[DSL_HASH((uint32_t)ident)];
}
#define _dispatch_unote_muxnote_bucket(du) \
	_dispatch_muxnote_bucket(du._du->du_ident)

DISPATCH_ALWAYS_INLINE
static inline dispatch_muxnote_t
_dispatch_muxnote_find(struct dispatch_muxnote_bucket_s *dmb,
		uint64_t ident, int16_t filter)
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
	if (dmn->dmn_filter != EVFILT_READ || dmn->dmn_fd != dmn->dmn_ident) {
		close(dmn->dmn_fd);
	}
	free(dmn);
}

static dispatch_muxnote_t
_dispatch_muxnote_create(dispatch_unote_t du, int16_t events)
{
	dispatch_muxnote_t dmn;
	struct stat sb;
	int fd = du._du->du_ident;
	int16_t filter = du._du->du_filter;
	bool socket_listener = false;
	sigset_t sigmask;

	switch (filter) {
	case EVFILT_SIGNAL:
		sigemptyset(&sigmask);
		sigaddset(&sigmask, du._du->du_ident);
		fd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
		if (fd < 0) {
			return NULL;
		}
		sigprocmask(SIG_BLOCK, &sigmask, NULL);
		break;

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
		} else if (S_ISSOCK(sb.st_mode)) {
			socklen_t vlen = sizeof(int);
			int v;
			if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &v, &vlen) == 0) {
				socket_listener = (bool)v;
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
	dmn->dmn_socket_listener = socket_listener;
	return dmn;
}

#pragma mark dispatch_unote_t

static int
_dispatch_epoll_update(dispatch_muxnote_t dmn, int op)
{
	struct epoll_event ev = {
		.events = dmn->dmn_events,
		.data = { .ptr = dmn },
	};
	return epoll_ctl(_dispatch_epfd, op, dmn->dmn_fd, &ev);
}

bool
_dispatch_unote_register(dispatch_unote_t du, dispatch_priority_t pri)
{
	struct dispatch_muxnote_bucket_s *dmb;
	dispatch_muxnote_t dmn;
	int16_t events = EPOLLFREE;

	dispatch_assert(!_dispatch_unote_registered(du));
	du._du->du_priority = pri;

	switch (du._du->du_filter) {
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
	case DISPATCH_EVFILT_CUSTOM_REPLACE:
		du._du->du_wlh = DISPATCH_WLH_GLOBAL;
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
		du._du->du_wlh = DISPATCH_WLH_GLOBAL;
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
_dispatch_unote_unregister(dispatch_unote_t du, uint32_t flags)
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
		int16_t events = dmn->dmn_events;

		if (du._du->du_filter == EVFILT_WRITE) {
			TAILQ_REMOVE(&dmn->dmn_writers_head, dul, du_link);
		} else {
			TAILQ_REMOVE(&dmn->dmn_readers_head, dul, du_link);
		}
		_TAILQ_TRASH_ENTRY(dul, du_link);
		dul->du_muxnote = NULL;

		if (TAILQ_EMPTY(&dmn->dmn_readers_head)) {
			events &= ~EPOLLIN;
		}
		if (TAILQ_EMPTY(&dmn->dmn_writers_head)) {
			events &= ~EPOLLOUT;
		}

		if (events == dmn->dmn_events) {
			// nothing to do
		} else if (events & (EPOLLIN | EPOLLOUT)) {
			_dispatch_epoll_update(dmn, EPOLL_CTL_MOD);
		} else {
			epoll_ctl(_dispatch_epfd, EPOLL_CTL_DEL, dmn->dmn_fd, NULL);
			TAILQ_REMOVE(_dispatch_unote_muxnote_bucket(du), dmn, dmn_list);
			_dispatch_muxnote_dispose(dmn);
		}
		du._du->du_wlh = NULL;
	}
	return true;
}

#pragma mark timers

static void
_dispatch_event_merge_timer(dispatch_clock_t clock)
{
	int qos;

	_dispatch_timers_expired = true;
	_dispatch_timers_processing_mask |= 1 << DISPATCH_TIMER_INDEX(clock, 0);
#if DISPATCH_USE_DTRACE
	_dispatch_timers_will_wake |= 1 << 0;
#endif
	_dispatch_epoll_timeout[clock].det_armed = false;
	_dispatch_timers_heap[clock].dth_flags &= ~DTH_ARMED;
}

static void
_dispatch_timeout_program(uint32_t tidx, uint64_t target, uint64_t leeway)
{
	uint32_t qos = DISPATCH_TIMER_QOS(tidx);
	dispatch_clock_t clock = DISPATCH_TIMER_CLOCK(tidx);
	dispatch_epoll_timeout_t timer = &_dispatch_epoll_timeout[clock];
	struct epoll_event ev = {
		.events = EPOLLONESHOT | EPOLLIN,
		.data = { .u32 = timer->det_ident },
	};
	unsigned long op;

	if (target >= INT64_MAX && !timer->det_registered) {
		return;
	}

	if (unlikely(timer->det_fd < 0)) {
		clockid_t clock;
		int fd;
		switch (DISPATCH_TIMER_CLOCK(tidx)) {
		case DISPATCH_CLOCK_MACH:
			clock = CLOCK_MONOTONIC;
			break;
		case DISPATCH_CLOCK_WALL:
			clock = CLOCK_REALTIME;
			break;
		}
		fd = timerfd_create(clock, TFD_NONBLOCK | TFD_CLOEXEC);
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
	target += _dispatch_time_cached_now(nows, DISPATCH_TIMER_CLOCK(tidx));
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

void
_dispatch_event_loop_init(void)
{
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
	unsigned long op = EPOLL_CTL_ADD;
	if (epoll_ctl(_dispatch_eventfd, op, _dispatch_eventfd, &ev) < 0) {
		DISPATCH_INTERNAL_CRASH(errno, "epoll_ctl() failed");
	}
}

void
_dispatch_event_loop_poke(dispatch_wlh_t wlh DISPATCH_UNUSED,
		dispatch_priority_t pri DISPATCH_UNUSED, uint32_t flags DISPATCH_UNUSED)
{
	dispatch_assume_zero(eventfd_write(_dispatch_eventfd, 1));
}

static void
_dispatch_event_merge_signal(dispatch_muxnote_t dmn)
{
	dispatch_unote_linkage_t dul, dul_next;
	struct signalfd_siginfo si;

	dispatch_assume(read(dmn->dmn_fd, &si, sizeof(si)) == sizeof(si));

	TAILQ_FOREACH_SAFE(dul, &dmn->dmn_readers_head, du_link, dul_next) {
		dispatch_unote_t du = _dispatch_unote_linkage_get_unote(dul);
		dux_merge_evt(du._du, EV_ADD|EV_ENABLE|EV_CLEAR, 1, 0);
	}
}

static uintptr_t
_dispatch_get_buffer_size(dispatch_muxnote_t dmn, bool writer)
{
	unsigned long op = writer ? SIOCOUTQ : SIOCINQ;
	int n;

	if (!writer && dmn->dmn_socket_listener) {
		// Linux doesn't support saying how many clients are ready to be
		// accept()ed
		return 1;
	}

	if (dispatch_assume_zero(ioctl(dmn->dmn_ident, op, &n))) {
		return 1;
	}
	return (uintptr_t)n;
}

static void
_dispatch_event_merge_fd(dispatch_muxnote_t dmn, int16_t events)
{
	dispatch_unote_linkage_t dul, dul_next;
	uintptr_t data;

	if (events & EPOLLIN) {
		data = _dispatch_get_buffer_size(dmn, false);
		TAILQ_FOREACH_SAFE(dul, &dmn->dmn_readers_head, du_link, dul_next) {
			dispatch_unote_t du = _dispatch_unote_linkage_get_unote(dul);
			dux_merge_evt(du._du, EV_ADD|EV_ENABLE|EV_DISPATCH, ~data, 0);
		}
	}

	if (events & EPOLLOUT) {
		data = _dispatch_get_buffer_size(dmn, true);
		TAILQ_FOREACH_SAFE(dul, &dmn->dmn_writers_head, du_link, dul_next) {
			dispatch_unote_t du = _dispatch_unote_linkage_get_unote(dul);
			dux_merge_evt(du._du, EV_ADD|EV_ENABLE|EV_DISPATCH, ~data, 0);
		}
	}
}

DISPATCH_NOINLINE
void
_dispatch_event_loop_drain(uint32_t flags)
{
	struct epoll_event ev[DISPATCH_DEFERRED_ITEMS_EVENT_COUNT];
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

#endif // DISPATCH_EVENT_BACKEND_EPOLL
