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
#include "protocol.h"
#include "protocolServer.h"
#include <sys/mount.h>

#define DISPATCH_EVFILT_TIMER	(-EVFILT_SYSCOUNT - 1)
#define DISPATCH_EVFILT_CUSTOM_ADD	(-EVFILT_SYSCOUNT - 2)
#define DISPATCH_EVFILT_CUSTOM_OR	(-EVFILT_SYSCOUNT - 3)
#define DISPATCH_EVFILT_SYSCOUNT	(EVFILT_SYSCOUNT + 3)

#define DISPATCH_TIMER_INDEX_WALL 0
#define DISPATCH_TIMER_INDEX_MACH 1
static struct dispatch_kevent_s _dispatch_kevent_timer[] = {
	{
		.dk_kevent = {
			.ident = DISPATCH_TIMER_INDEX_WALL,
			.filter = DISPATCH_EVFILT_TIMER,
			.udata = &_dispatch_kevent_timer[0],
		},
		.dk_sources = TAILQ_HEAD_INITIALIZER(_dispatch_kevent_timer[0].dk_sources),
	},
	{
		.dk_kevent = {
			.ident = DISPATCH_TIMER_INDEX_MACH,
			.filter = DISPATCH_EVFILT_TIMER,
			.udata = &_dispatch_kevent_timer[1],
		},
		.dk_sources = TAILQ_HEAD_INITIALIZER(_dispatch_kevent_timer[1].dk_sources),
	},
};
#define DISPATCH_TIMER_COUNT (sizeof _dispatch_kevent_timer / sizeof _dispatch_kevent_timer[0])

static struct dispatch_kevent_s _dispatch_kevent_data_or = {
	.dk_kevent = {
		.filter = DISPATCH_EVFILT_CUSTOM_OR,
		.flags = EV_CLEAR,
		.udata = &_dispatch_kevent_data_or,
	},
	.dk_sources = TAILQ_HEAD_INITIALIZER(_dispatch_kevent_data_or.dk_sources),
};
static struct dispatch_kevent_s _dispatch_kevent_data_add = {
	.dk_kevent = {
		.filter = DISPATCH_EVFILT_CUSTOM_ADD,
		.udata = &_dispatch_kevent_data_add,
	},
	.dk_sources = TAILQ_HEAD_INITIALIZER(_dispatch_kevent_data_add.dk_sources),
};

#ifndef DISPATCH_NO_LEGACY
struct dispatch_source_attr_vtable_s {
	DISPATCH_VTABLE_HEADER(dispatch_source_attr_s);
};

struct dispatch_source_attr_s {
	DISPATCH_STRUCT_HEADER(dispatch_source_attr_s, dispatch_source_attr_vtable_s);
	void* finalizer_ctxt;
	dispatch_source_finalizer_function_t finalizer_func;
	void* context;
};
#endif /* DISPATCH_NO_LEGACY */

#define _dispatch_source_call_block ((void *)-1)
static void _dispatch_source_latch_and_call(dispatch_source_t ds);
static void _dispatch_source_cancel_callout(dispatch_source_t ds);
static bool _dispatch_source_probe(dispatch_source_t ds);
static void _dispatch_source_dispose(dispatch_source_t ds);
static void _dispatch_source_merge_kevent(dispatch_source_t ds, const struct kevent *ke);
static size_t _dispatch_source_debug(dispatch_source_t ds, char* buf, size_t bufsiz);
static size_t dispatch_source_debug_attr(dispatch_source_t ds, char* buf, size_t bufsiz);
static dispatch_queue_t _dispatch_source_invoke(dispatch_source_t ds);

static void _dispatch_kevent_merge(dispatch_source_t ds);
static void _dispatch_kevent_release(dispatch_source_t ds);
static void _dispatch_kevent_resume(dispatch_kevent_t dk, uint32_t new_flags, uint32_t del_flags);
static void _dispatch_kevent_machport_resume(dispatch_kevent_t dk, uint32_t new_flags, uint32_t del_flags);
static void _dispatch_kevent_machport_enable(dispatch_kevent_t dk);
static void _dispatch_kevent_machport_disable(dispatch_kevent_t dk);

static void _dispatch_drain_mach_messages(struct kevent *ke);
static void _dispatch_timer_list_update(dispatch_source_t ds);

static void
_dispatch_mach_notify_source_init(void *context __attribute__((unused)));

static const char *
_evfiltstr(short filt)
{
	switch (filt) {
#define _evfilt2(f) case (f): return #f
	_evfilt2(EVFILT_READ);
	_evfilt2(EVFILT_WRITE);
	_evfilt2(EVFILT_AIO);
	_evfilt2(EVFILT_VNODE);
	_evfilt2(EVFILT_PROC);
	_evfilt2(EVFILT_SIGNAL);
	_evfilt2(EVFILT_TIMER);
	_evfilt2(EVFILT_MACHPORT);
	_evfilt2(EVFILT_FS);
	_evfilt2(EVFILT_USER);
	_evfilt2(EVFILT_SESSION);

	_evfilt2(DISPATCH_EVFILT_TIMER);
	_evfilt2(DISPATCH_EVFILT_CUSTOM_ADD);
	_evfilt2(DISPATCH_EVFILT_CUSTOM_OR);
	default:
		return "EVFILT_missing";
	}
}

#define DSL_HASH_SIZE 256u	// must be a power of two
#define DSL_HASH(x)	((x) & (DSL_HASH_SIZE - 1))

static TAILQ_HEAD(, dispatch_kevent_s) _dispatch_sources[DSL_HASH_SIZE];

static dispatch_kevent_t
_dispatch_kevent_find(uintptr_t ident, short filter)
{
	uintptr_t hash = DSL_HASH(filter == EVFILT_MACHPORT ? MACH_PORT_INDEX(ident) : ident);
	dispatch_kevent_t dki;

	TAILQ_FOREACH(dki, &_dispatch_sources[hash], dk_list) {
		if (dki->dk_kevent.ident == ident && dki->dk_kevent.filter == filter) {
			break;
		}
	}
	return dki;
}

static void
_dispatch_kevent_insert(dispatch_kevent_t dk)
{
	uintptr_t ident = dk->dk_kevent.ident;
	uintptr_t hash = DSL_HASH(dk->dk_kevent.filter == EVFILT_MACHPORT ? MACH_PORT_INDEX(ident) : ident);

	TAILQ_INSERT_TAIL(&_dispatch_sources[hash], dk, dk_list);
}

void
dispatch_source_cancel(dispatch_source_t ds)
{
#if DISPATCH_DEBUG
	dispatch_debug(ds, __FUNCTION__);
#endif
	dispatch_atomic_or(&ds->ds_atomic_flags, DSF_CANCELED);
	_dispatch_wakeup(ds);
}

#ifndef DISPATCH_NO_LEGACY
void
_dispatch_source_legacy_xref_release(dispatch_source_t ds)
{
	if (ds->ds_is_legacy) {
		if (!(ds->ds_timer.flags & DISPATCH_TIMER_ONESHOT)) {
			dispatch_source_cancel(ds);
		}

		// Clients often leave sources suspended at the last release
		dispatch_atomic_and(&ds->do_suspend_cnt, DISPATCH_OBJECT_SUSPEND_LOCK);
	} else if (slowpath(DISPATCH_OBJECT_SUSPENDED(ds))) {
		// Arguments for and against this assert are within 6705399
		DISPATCH_CLIENT_CRASH("Release of a suspended object");
	}
	_dispatch_wakeup(ds);
	_dispatch_release(ds);
}
#endif /* DISPATCH_NO_LEGACY */

long
dispatch_source_testcancel(dispatch_source_t ds)
{
	return (bool)(ds->ds_atomic_flags & DSF_CANCELED);
}


unsigned long
dispatch_source_get_mask(dispatch_source_t ds)
{
	return ds->ds_pending_data_mask;
}

uintptr_t
dispatch_source_get_handle(dispatch_source_t ds)
{
	return (int)ds->ds_ident_hack;
}

unsigned long
dispatch_source_get_data(dispatch_source_t ds)
{
	return ds->ds_data;
}

#if DISPATCH_DEBUG
void
dispatch_debug_kevents(struct kevent* kev, size_t count, const char* str)
{
	size_t i;
	for (i = 0; i < count; ++i) {
		_dispatch_log("kevent[%lu] = { ident = %p, filter = %s, flags = 0x%x, fflags = 0x%x, data = %p, udata = %p }: %s",
			i, (void*)kev[i].ident, _evfiltstr(kev[i].filter), kev[i].flags, kev[i].fflags, (void*)kev[i].data, (void*)kev[i].udata, str);
	}
}
#endif

static size_t
_dispatch_source_kevent_debug(dispatch_source_t ds, char* buf, size_t bufsiz)
{
	size_t offset = _dispatch_source_debug(ds, buf, bufsiz);
	offset += snprintf(&buf[offset], bufsiz - offset, "filter = %s }",
			ds->ds_dkev ? _evfiltstr(ds->ds_dkev->dk_kevent.filter) : "????");
	return offset;
}

static void
_dispatch_source_init_tail_queue_array(void *context __attribute__((unused)))
{
	unsigned int i;
	for (i = 0; i < DSL_HASH_SIZE; i++) {
		TAILQ_INIT(&_dispatch_sources[i]);
	}

	TAILQ_INSERT_TAIL(&_dispatch_sources[DSL_HASH(DISPATCH_TIMER_INDEX_WALL)], &_dispatch_kevent_timer[DISPATCH_TIMER_INDEX_WALL], dk_list);
	TAILQ_INSERT_TAIL(&_dispatch_sources[DSL_HASH(DISPATCH_TIMER_INDEX_MACH)], &_dispatch_kevent_timer[DISPATCH_TIMER_INDEX_MACH], dk_list);
	TAILQ_INSERT_TAIL(&_dispatch_sources[0], &_dispatch_kevent_data_or, dk_list);
	TAILQ_INSERT_TAIL(&_dispatch_sources[0], &_dispatch_kevent_data_add, dk_list);
}

// Find existing kevents, and merge any new flags if necessary
void
_dispatch_kevent_merge(dispatch_source_t ds)
{
	static dispatch_once_t pred;
	dispatch_kevent_t dk;
	typeof(dk->dk_kevent.fflags) new_flags;
	bool do_resume = false;

	if (ds->ds_is_installed) {
		return;
	}
	ds->ds_is_installed = true;

	dispatch_once_f(&pred, NULL, _dispatch_source_init_tail_queue_array);

	dk = _dispatch_kevent_find(ds->ds_dkev->dk_kevent.ident, ds->ds_dkev->dk_kevent.filter);
	
	if (dk) {
		// If an existing dispatch kevent is found, check to see if new flags
		// need to be added to the existing kevent
		new_flags = ~dk->dk_kevent.fflags & ds->ds_dkev->dk_kevent.fflags;
		dk->dk_kevent.fflags |= ds->ds_dkev->dk_kevent.fflags;
		free(ds->ds_dkev);
		ds->ds_dkev = dk;
		do_resume = new_flags;
	} else {
		dk = ds->ds_dkev;
		_dispatch_kevent_insert(dk);
		new_flags = dk->dk_kevent.fflags;
		do_resume = true;
	}

	TAILQ_INSERT_TAIL(&dk->dk_sources, ds, ds_list);

	// Re-register the kevent with the kernel if new flags were added
	// by the dispatch kevent
	if (do_resume) {
		dk->dk_kevent.flags |= EV_ADD;
		_dispatch_kevent_resume(ds->ds_dkev, new_flags, 0);
		ds->ds_is_armed = true;
	}
}


void
_dispatch_kevent_resume(dispatch_kevent_t dk, uint32_t new_flags, uint32_t del_flags)
{
	switch (dk->dk_kevent.filter) {
	case DISPATCH_EVFILT_TIMER:
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
		// these types not registered with kevent
		return;
	case EVFILT_MACHPORT:
		_dispatch_kevent_machport_resume(dk, new_flags, del_flags);
		break;
	case EVFILT_PROC:
		if (dk->dk_kevent.flags & EV_ONESHOT) {
			return;
		}
		// fall through
	default:
		_dispatch_update_kq(&dk->dk_kevent);
		if (dk->dk_kevent.flags & EV_DISPATCH) {
			dk->dk_kevent.flags &= ~EV_ADD;
		}
		break;
	}
}

dispatch_queue_t
_dispatch_source_invoke(dispatch_source_t ds)
{
	// This function performs all source actions. Each action is responsible
	// for verifying that it takes place on the appropriate queue. If the
	// current queue is not the correct queue for this action, the correct queue
	// will be returned and the invoke will be re-driven on that queue.

	// The order of tests here in invoke and in probe should be consistent.
	
	dispatch_queue_t dq = _dispatch_queue_get_current();

	if (!ds->ds_is_installed) {
		// The source needs to be installed on the manager queue.
		if (dq != &_dispatch_mgr_q) {
			return &_dispatch_mgr_q;
		}
		_dispatch_kevent_merge(ds);
	} else if ((ds->ds_atomic_flags & DSF_CANCELED) || (ds->do_xref_cnt == 0)) {
		// The source has been cancelled and needs to be uninstalled from the
		// manager queue. After uninstallation, the cancellation handler needs
		// to be delivered to the target queue.
		if (ds->ds_dkev) {
			if (dq != &_dispatch_mgr_q) {
				return &_dispatch_mgr_q;
			}
			_dispatch_kevent_release(ds);
			return ds->do_targetq;
		} else if (ds->ds_cancel_handler) {
			if (dq != ds->do_targetq) {
				return ds->do_targetq;
			}
		}	
		_dispatch_source_cancel_callout(ds);
	} else if (ds->ds_pending_data) {
		// The source has pending data to deliver via the event handler callback
		// on the target queue. Some sources need to be rearmed on the manager
		// queue after event delivery.
		if (dq != ds->do_targetq) {
			return ds->do_targetq;
		}
		_dispatch_source_latch_and_call(ds);
		if (ds->ds_needs_rearm) {
			return &_dispatch_mgr_q;
		}
	} else if (ds->ds_needs_rearm && !ds->ds_is_armed) {
		// The source needs to be rearmed on the manager queue.
		if (dq != &_dispatch_mgr_q) {
			return &_dispatch_mgr_q;
		}
		_dispatch_kevent_resume(ds->ds_dkev, 0, 0);
		ds->ds_is_armed = true;
	}

	return NULL;
}

bool
_dispatch_source_probe(dispatch_source_t ds)
{
	// This function determines whether the source needs to be invoked.
	// The order of tests here in probe and in invoke should be consistent.

	if (!ds->ds_is_installed) {
		// The source needs to be installed on the manager queue.
		return true;
	} else if ((ds->ds_atomic_flags & DSF_CANCELED) || (ds->do_xref_cnt == 0)) {
		// The source needs to be uninstalled from the manager queue, or the
		// cancellation handler needs to be delivered to the target queue.
		// Note: cancellation assumes installation.
		if (ds->ds_dkev || ds->ds_cancel_handler) {
			return true;
		}
	} else if (ds->ds_pending_data) {
		// The source has pending data to deliver to the target queue.
		return true;
	} else if (ds->ds_needs_rearm && !ds->ds_is_armed) {
		// The source needs to be rearmed on the manager queue.
		return true;
	}
	// Nothing to do.
	return false;
}

void
_dispatch_source_dispose(dispatch_source_t ds)
{
	_dispatch_queue_dispose((dispatch_queue_t)ds);
}

static void
_dispatch_kevent_debugger2(void *context, dispatch_source_t unused __attribute__((unused)))
{
	struct sockaddr sa;
	socklen_t sa_len = sizeof(sa);
	int c, fd = (int)(long)context;
	unsigned int i;
	dispatch_kevent_t dk;
	dispatch_source_t ds;
	FILE *debug_stream;

	c = accept(fd, &sa, &sa_len);
	if (c == -1) {
		if (errno != EAGAIN) {
			dispatch_assume_zero(errno);
		}
		return;
	}
#if 0
	int r = fcntl(c, F_SETFL, 0);	// disable non-blocking IO
	if (r == -1) {
		dispatch_assume_zero(errno);
	}
#endif
	debug_stream = fdopen(c, "a");
	if (!dispatch_assume(debug_stream)) {
		close(c);
		return;
	}

	fprintf(debug_stream, "HTTP/1.0 200 OK\r\n");
	fprintf(debug_stream, "Content-type: text/html\r\n");
	fprintf(debug_stream, "Pragma: nocache\r\n");
	fprintf(debug_stream, "\r\n");
	fprintf(debug_stream, "<html>\n<head><title>PID %u</title></head>\n<body>\n<ul>\n", getpid());

	//fprintf(debug_stream, "<tr><td>DK</td><td>DK</td><td>DK</td><td>DK</td><td>DK</td><td>DK</td><td>DK</td></tr>\n");

	for (i = 0; i < DSL_HASH_SIZE; i++) {
		if (TAILQ_EMPTY(&_dispatch_sources[i])) {
			continue;
		}
		TAILQ_FOREACH(dk, &_dispatch_sources[i], dk_list) {
			fprintf(debug_stream, "\t<br><li>DK %p ident %lu filter %s flags 0x%hx fflags 0x%x data 0x%lx udata %p\n",
					dk, dk->dk_kevent.ident, _evfiltstr(dk->dk_kevent.filter), dk->dk_kevent.flags,
					dk->dk_kevent.fflags, dk->dk_kevent.data, dk->dk_kevent.udata);
			fprintf(debug_stream, "\t\t<ul>\n");
			TAILQ_FOREACH(ds, &dk->dk_sources, ds_list) {
				fprintf(debug_stream, "\t\t\t<li>DS %p refcnt 0x%x suspend 0x%x data 0x%lx mask 0x%lx flags 0x%x</li>\n",
						ds, ds->do_ref_cnt, ds->do_suspend_cnt, ds->ds_pending_data, ds->ds_pending_data_mask,
						ds->ds_atomic_flags);
				if (ds->do_suspend_cnt == DISPATCH_OBJECT_SUSPEND_LOCK) {
					dispatch_queue_t dq = ds->do_targetq;
					fprintf(debug_stream, "\t\t<br>DQ: %p refcnt 0x%x suspend 0x%x label: %s\n", dq, dq->do_ref_cnt, dq->do_suspend_cnt, dq->dq_label);
				}
			}
			fprintf(debug_stream, "\t\t</ul>\n");
			fprintf(debug_stream, "\t</li>\n");
		}
	}
	fprintf(debug_stream, "</ul>\n</body>\n</html>\n");
	fflush(debug_stream);
	fclose(debug_stream);
}

static void
_dispatch_kevent_debugger(void *context __attribute__((unused)))
{
	union {
		struct sockaddr_in sa_in;
		struct sockaddr sa;
	} sa_u = {
		.sa_in = {
			.sin_family = AF_INET,
			.sin_addr = { htonl(INADDR_LOOPBACK), },
		},
	};
	dispatch_source_t ds;
	const char *valstr;
	int val, r, fd, sock_opt = 1;
	socklen_t slen = sizeof(sa_u);

	if (issetugid()) {
		return;
	}
	valstr = getenv("LIBDISPATCH_DEBUGGER");
	if (!valstr) {
		return;
	}
	val = atoi(valstr);
	if (val == 2) {
		sa_u.sa_in.sin_addr.s_addr = 0;
	}
	fd = socket(PF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		dispatch_assume_zero(errno);
		return;
	}
	r = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&sock_opt, (socklen_t) sizeof sock_opt);
	if (r == -1) {
		dispatch_assume_zero(errno);
		goto out_bad;
	}
#if 0
	r = fcntl(fd, F_SETFL, O_NONBLOCK);
	if (r == -1) {
		dispatch_assume_zero(errno);
		goto out_bad;
	}
#endif
	r = bind(fd, &sa_u.sa, sizeof(sa_u));
	if (r == -1) {
		dispatch_assume_zero(errno);
		goto out_bad;
	}
	r = listen(fd, SOMAXCONN);
	if (r == -1) {
		dispatch_assume_zero(errno);
		goto out_bad;
	}
	r = getsockname(fd, &sa_u.sa, &slen);
	if (r == -1) {
		dispatch_assume_zero(errno);
		goto out_bad;
	}
	ds = dispatch_source_read_create_f(fd, NULL, &_dispatch_mgr_q, (void *)(long)fd, _dispatch_kevent_debugger2);
	if (dispatch_assume(ds)) {
		_dispatch_log("LIBDISPATCH: debug port: %hu", ntohs(sa_u.sa_in.sin_port));
		return;
	}
out_bad:
	close(fd);
}

void
_dispatch_source_drain_kevent(struct kevent *ke)
{
	static dispatch_once_t pred;
	dispatch_kevent_t dk = ke->udata;
	dispatch_source_t dsi;

	dispatch_once_f(&pred, NULL, _dispatch_kevent_debugger);

	dispatch_debug_kevents(ke, 1, __func__);

	if (ke->filter == EVFILT_MACHPORT) {
		return _dispatch_drain_mach_messages(ke);
	}
	dispatch_assert(dk);

	if (ke->flags & EV_ONESHOT) {
		dk->dk_kevent.flags |= EV_ONESHOT;
	}

	TAILQ_FOREACH(dsi, &dk->dk_sources, ds_list) {
		_dispatch_source_merge_kevent(dsi, ke);
	}
}

static void
_dispatch_kevent_dispose(dispatch_kevent_t dk)
{
	uintptr_t key;

	switch (dk->dk_kevent.filter) {
	case DISPATCH_EVFILT_TIMER:
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
		// these sources live on statically allocated lists
		return;
	case EVFILT_MACHPORT:
		_dispatch_kevent_machport_resume(dk, 0, dk->dk_kevent.fflags);
		break;
	case EVFILT_PROC:
		if (dk->dk_kevent.flags & EV_ONESHOT) {
			break;	// implicitly deleted
		}
		// fall through
	default:
		if (~dk->dk_kevent.flags & EV_DELETE) {
			dk->dk_kevent.flags |= EV_DELETE;
			_dispatch_update_kq(&dk->dk_kevent);
		}
		break;
	}

	if (dk->dk_kevent.filter == EVFILT_MACHPORT) {
		key = MACH_PORT_INDEX(dk->dk_kevent.ident);
	} else {
		key = dk->dk_kevent.ident;
	}

	TAILQ_REMOVE(&_dispatch_sources[DSL_HASH(key)], dk, dk_list);
	free(dk);
}

void
_dispatch_kevent_release(dispatch_source_t ds)
{
	dispatch_kevent_t dk = ds->ds_dkev;
	dispatch_source_t dsi;
	uint32_t del_flags, fflags = 0;

	ds->ds_dkev = NULL;

	TAILQ_REMOVE(&dk->dk_sources, ds, ds_list);

	if (TAILQ_EMPTY(&dk->dk_sources)) {
		_dispatch_kevent_dispose(dk);
	} else {
		TAILQ_FOREACH(dsi, &dk->dk_sources, ds_list) {
			fflags |= (uint32_t)dsi->ds_pending_data_mask;
		}
		del_flags = (uint32_t)ds->ds_pending_data_mask & ~fflags;
		if (del_flags) {
			dk->dk_kevent.flags |= EV_ADD;
			dk->dk_kevent.fflags = fflags;
			_dispatch_kevent_resume(dk, 0, del_flags);
		}
	}

	ds->ds_is_armed = false;
	ds->ds_needs_rearm = false;	// re-arm is pointless and bad now
	_dispatch_release(ds);	// the retain is done at creation time
}

void
_dispatch_source_merge_kevent(dispatch_source_t ds, const struct kevent *ke)
{
	struct kevent fake;

	if ((ds->ds_atomic_flags & DSF_CANCELED) || (ds->do_xref_cnt == 0)) {
		return;
	}

	// EVFILT_PROC may fail with ESRCH when the process exists but is a zombie.
	// We simulate an exit event in this case. <rdar://problem/5067725>
	if (ke->flags & EV_ERROR) {
		if (ke->filter == EVFILT_PROC && ke->data == ESRCH) {
			fake = *ke;
			fake.flags &= ~EV_ERROR;
			fake.fflags = NOTE_EXIT;
			fake.data = 0;
			ke = &fake;
		} else {
			// log the unexpected error
			dispatch_assume_zero(ke->data);
			return;
		}
	}

	if (ds->ds_is_level) {
		// ke->data is signed and "negative available data" makes no sense
		// zero bytes happens when EV_EOF is set
		// 10A268 does not fail this assert with EVFILT_READ and a 10 GB file
		dispatch_assert(ke->data >= 0l);
		ds->ds_pending_data = ~ke->data;
	} else if (ds->ds_is_adder) {
		dispatch_atomic_add(&ds->ds_pending_data, ke->data);
	} else {
		dispatch_atomic_or(&ds->ds_pending_data, ke->fflags & ds->ds_pending_data_mask);
	}

	// EV_DISPATCH and EV_ONESHOT sources are no longer armed after delivery
	if (ds->ds_needs_rearm) {
		ds->ds_is_armed = false;
	}

	_dispatch_wakeup(ds);
}

void
_dispatch_source_latch_and_call(dispatch_source_t ds)
{
	unsigned long prev;

	if ((ds->ds_atomic_flags & DSF_CANCELED) || (ds->do_xref_cnt == 0)) {
		return;
	}
	prev = dispatch_atomic_xchg(&ds->ds_pending_data, 0);
	if (ds->ds_is_level) {
		ds->ds_data = ~prev;
	} else {
		ds->ds_data = prev;
	}
	if (dispatch_assume(prev)) {
		if (ds->ds_handler_func) {
			ds->ds_handler_func(ds->ds_handler_ctxt, ds);
		}
	}
}

void
_dispatch_source_cancel_callout(dispatch_source_t ds)
{
	ds->ds_pending_data_mask = 0;
	ds->ds_pending_data = 0;
	ds->ds_data = 0;

#ifdef __BLOCKS__
	if (ds->ds_handler_is_block) {
		Block_release(ds->ds_handler_ctxt);
		ds->ds_handler_is_block = false;
		ds->ds_handler_func = NULL;
		ds->ds_handler_ctxt = NULL;
	}
#endif

	if (!ds->ds_cancel_handler) {
		return;
	}
	if (ds->ds_cancel_is_block) {
#ifdef __BLOCKS__
		dispatch_block_t b = ds->ds_cancel_handler;
		if (ds->ds_atomic_flags & DSF_CANCELED) {
			b();
		}
		Block_release(ds->ds_cancel_handler);
		ds->ds_cancel_is_block = false;
#endif
	} else {
		dispatch_function_t f = ds->ds_cancel_handler;
		if (ds->ds_atomic_flags & DSF_CANCELED) {
			f(ds->do_ctxt);
		}
	}
	ds->ds_cancel_handler = NULL;
}

const struct dispatch_source_vtable_s _dispatch_source_kevent_vtable = {
	.do_type = DISPATCH_SOURCE_KEVENT_TYPE,
	.do_kind = "kevent-source",
	.do_invoke = _dispatch_source_invoke,
	.do_dispose = _dispatch_source_dispose,
	.do_probe = _dispatch_source_probe,
	.do_debug = _dispatch_source_kevent_debug,
};

void
dispatch_source_merge_data(dispatch_source_t ds, unsigned long val)
{	
	struct kevent kev = {
		.fflags = (typeof(kev.fflags))val,
		.data = val,
	};

	dispatch_assert(ds->ds_dkev->dk_kevent.filter == DISPATCH_EVFILT_CUSTOM_ADD ||
					ds->ds_dkev->dk_kevent.filter == DISPATCH_EVFILT_CUSTOM_OR);

	_dispatch_source_merge_kevent(ds, &kev);
}

size_t
dispatch_source_debug_attr(dispatch_source_t ds, char* buf, size_t bufsiz)
{
	dispatch_queue_t target = ds->do_targetq;
	return snprintf(buf, bufsiz,
			"target = %s[%p], pending_data = 0x%lx, pending_data_mask = 0x%lx, ",
			target ? target->dq_label : "", target,
			ds->ds_pending_data, ds->ds_pending_data_mask);
}

size_t
_dispatch_source_debug(dispatch_source_t ds, char* buf, size_t bufsiz)
{
        size_t offset = 0;
        offset += snprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ", dx_kind(ds), ds);
        offset += dispatch_object_debug_attr(ds, &buf[offset], bufsiz - offset);
        offset += dispatch_source_debug_attr(ds, &buf[offset], bufsiz - offset);
        return offset;
}

#ifndef DISPATCH_NO_LEGACY
static void
dispatch_source_attr_dispose(dispatch_source_attr_t attr)
{
	// release the finalizer block if necessary
	dispatch_source_attr_set_finalizer(attr, NULL);
	_dispatch_dispose(attr);
}

static const struct dispatch_source_attr_vtable_s dispatch_source_attr_vtable = {
	.do_type = DISPATCH_SOURCE_ATTR_TYPE,
	.do_kind = "source-attr",
	.do_dispose = dispatch_source_attr_dispose,
};

dispatch_source_attr_t
dispatch_source_attr_create(void)
{
	dispatch_source_attr_t rval = calloc(1, sizeof(struct dispatch_source_attr_s));

	if (rval) {
		rval->do_vtable = &dispatch_source_attr_vtable;
		rval->do_next = DISPATCH_OBJECT_LISTLESS;
		rval->do_targetq = dispatch_get_global_queue(0, 0);
		rval->do_ref_cnt = 1;
		rval->do_xref_cnt = 1;
	}

	return rval;
}

void
dispatch_source_attr_set_finalizer_f(dispatch_source_attr_t attr,
	void *context, dispatch_source_finalizer_function_t finalizer)
{
#ifdef __BLOCKS__
	if (attr->finalizer_func == (void*)_dispatch_call_block_and_release2) {
		Block_release(attr->finalizer_ctxt);
	}
#endif

	attr->finalizer_ctxt = context;
	attr->finalizer_func = finalizer;
}

#ifdef __BLOCKS__
long
dispatch_source_attr_set_finalizer(dispatch_source_attr_t attr,
	dispatch_source_finalizer_t finalizer)
{
	void *ctxt;
	dispatch_source_finalizer_function_t func;

	if (finalizer) {
		if (!(ctxt = Block_copy(finalizer))) {
			return 1;
		}
		func = (void *)_dispatch_call_block_and_release2;
	} else {
		ctxt = NULL;
		func = NULL;
	}

	dispatch_source_attr_set_finalizer_f(attr, ctxt, func);

	return 0;
}

dispatch_source_finalizer_t
dispatch_source_attr_get_finalizer(dispatch_source_attr_t attr)
{
	if (attr->finalizer_func == (void*)_dispatch_call_block_and_release2) {
		return (dispatch_source_finalizer_t)attr->finalizer_ctxt;
	} else if (attr->finalizer_func == NULL) {
		return NULL;
	} else {
		abort(); // finalizer is not a block...
	}
}
#endif

void
dispatch_source_attr_set_context(dispatch_source_attr_t attr, void *context)
{
	attr->context = context;
}

dispatch_source_attr_t
dispatch_source_attr_copy(dispatch_source_attr_t proto)
{
	dispatch_source_attr_t rval = NULL;

	if (proto && (rval = malloc(sizeof(struct dispatch_source_attr_s)))) {
		memcpy(rval, proto, sizeof(struct dispatch_source_attr_s));
#ifdef __BLOCKS__
		if (rval->finalizer_func == (void*)_dispatch_call_block_and_release2) {
			rval->finalizer_ctxt = Block_copy(rval->finalizer_ctxt);
		}
#endif
	} else if (!proto) {
		rval = dispatch_source_attr_create();
	}
	return rval;
}
#endif /* DISPATCH_NO_LEGACY */


struct dispatch_source_type_s {
	struct kevent ke;
	uint64_t mask;
};

const struct dispatch_source_type_s _dispatch_source_type_timer = {
	.ke = {
		.filter = DISPATCH_EVFILT_TIMER,
	},
	.mask = DISPATCH_TIMER_INTERVAL|DISPATCH_TIMER_ONESHOT|DISPATCH_TIMER_ABSOLUTE|DISPATCH_TIMER_WALL_CLOCK,
};

const struct dispatch_source_type_s _dispatch_source_type_read = {
	.ke = {
		.filter = EVFILT_READ,
		.flags = EV_DISPATCH,
	},
};

const struct dispatch_source_type_s _dispatch_source_type_write = {
	.ke = {
		.filter = EVFILT_WRITE,
		.flags = EV_DISPATCH,
	},
};

const struct dispatch_source_type_s _dispatch_source_type_proc = {
	.ke = {
		.filter = EVFILT_PROC,
		.flags = EV_CLEAR,
	},
	.mask = NOTE_EXIT|NOTE_FORK|NOTE_EXEC|NOTE_SIGNAL|NOTE_REAP,
};

const struct dispatch_source_type_s _dispatch_source_type_signal = {
	.ke = {
		.filter = EVFILT_SIGNAL,
	},
};

const struct dispatch_source_type_s _dispatch_source_type_vnode = {
	.ke = {
		.filter = EVFILT_VNODE,
		.flags = EV_CLEAR,
	},
	.mask = NOTE_DELETE|NOTE_WRITE|NOTE_EXTEND|NOTE_ATTRIB|NOTE_LINK|NOTE_RENAME|NOTE_REVOKE|NOTE_NONE,
};

const struct dispatch_source_type_s _dispatch_source_type_vfs = {
	.ke = {
		.filter = EVFILT_FS,
		.flags = EV_CLEAR,
	},
	.mask = VQ_NOTRESP|VQ_NEEDAUTH|VQ_LOWDISK|VQ_MOUNT|VQ_UNMOUNT|VQ_DEAD|VQ_ASSIST|VQ_NOTRESPLOCK|VQ_UPDATE|VQ_VERYLOWDISK,
};

const struct dispatch_source_type_s _dispatch_source_type_mach_send = {
	.ke = {
		.filter = EVFILT_MACHPORT,
		.flags = EV_DISPATCH,
		.fflags = DISPATCH_MACHPORT_DEAD,
	},
	.mask = DISPATCH_MACH_SEND_DEAD,
};

const struct dispatch_source_type_s _dispatch_source_type_mach_recv = {
	.ke = {
		.filter = EVFILT_MACHPORT,
		.flags = EV_DISPATCH,
		.fflags = DISPATCH_MACHPORT_RECV,
	},
};

const struct dispatch_source_type_s _dispatch_source_type_data_add = {
	.ke = {
		.filter = DISPATCH_EVFILT_CUSTOM_ADD,
	},
};

const struct dispatch_source_type_s _dispatch_source_type_data_or = {
	.ke = {
		.filter = DISPATCH_EVFILT_CUSTOM_OR,
		.flags = EV_CLEAR,
		.fflags = ~0,
	},
};

dispatch_source_t
dispatch_source_create(dispatch_source_type_t type,
	uintptr_t handle,
	unsigned long mask,
	dispatch_queue_t q)
{
	const struct kevent *proto_kev = &type->ke;
	dispatch_source_t ds = NULL;
	dispatch_kevent_t dk = NULL;

	// input validation
	if (type == NULL || (mask & ~type->mask)) {
		goto out_bad;
	}

	switch (type->ke.filter) {
	case EVFILT_SIGNAL:
		if (handle >= NSIG) {
			goto out_bad;
		}
		break;
	case EVFILT_FS:
	case DISPATCH_EVFILT_CUSTOM_ADD:
	case DISPATCH_EVFILT_CUSTOM_OR:
	case DISPATCH_EVFILT_TIMER:
		if (handle) {
			goto out_bad;
		}
		break;
	default:
		break;
	}
	
	ds = calloc(1ul, sizeof(struct dispatch_source_s));
	if (slowpath(!ds)) {
		goto out_bad;
	}
	dk = calloc(1ul, sizeof(struct dispatch_kevent_s));
	if (slowpath(!dk)) {
		goto out_bad;
	}

	dk->dk_kevent = *proto_kev;
	dk->dk_kevent.ident = handle;
	dk->dk_kevent.flags |= EV_ADD|EV_ENABLE;
	dk->dk_kevent.fflags |= (uint32_t)mask;
	dk->dk_kevent.udata = dk;
	TAILQ_INIT(&dk->dk_sources);

	// Initialize as a queue first, then override some settings below.
	_dispatch_queue_init((dispatch_queue_t)ds);
	strlcpy(ds->dq_label, "source", sizeof(ds->dq_label));

	// Dispatch Object
	ds->do_vtable = &_dispatch_source_kevent_vtable;
	ds->do_ref_cnt++; // the reference the manger queue holds
	ds->do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_INTERVAL;
	// do_targetq will be retained below, past point of no-return
	ds->do_targetq = q;

	// Dispatch Source
	ds->ds_ident_hack = dk->dk_kevent.ident;
	ds->ds_dkev = dk;
	ds->ds_pending_data_mask = dk->dk_kevent.fflags;
	if ((EV_DISPATCH|EV_ONESHOT) & proto_kev->flags) {
		if (proto_kev->filter != EVFILT_MACHPORT) {
			ds->ds_is_level = true;
		}
		ds->ds_needs_rearm = true;
	} else if (!(EV_CLEAR & proto_kev->flags)) {
		// we cheat and use EV_CLEAR to mean a "flag thingy"
		ds->ds_is_adder = true;
	}
	
	// If its a timer source, it needs to be re-armed
	if (type->ke.filter == DISPATCH_EVFILT_TIMER) {
		ds->ds_needs_rearm = true;
	}
	
	dispatch_assert(!(ds->ds_is_level && ds->ds_is_adder));
#if DISPATCH_DEBUG
	dispatch_debug(ds, __FUNCTION__);
#endif

	// Some sources require special processing
	if (type == DISPATCH_SOURCE_TYPE_MACH_SEND) {
		static dispatch_once_t pred;
		dispatch_once_f(&pred, NULL, _dispatch_mach_notify_source_init);
	} else if (type == DISPATCH_SOURCE_TYPE_TIMER) {
		ds->ds_timer.flags = mask;
	}

	_dispatch_retain(ds->do_targetq);
	return ds;
	
out_bad:
	free(ds);
	free(dk);
	return NULL;
}

// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
static void
_dispatch_source_set_event_handler2(void *context)
{
	struct Block_layout *bl = context;

	dispatch_source_t ds = (dispatch_source_t)_dispatch_queue_get_current();
	dispatch_assert(ds->do_vtable == &_dispatch_source_kevent_vtable);
	
	if (ds->ds_handler_is_block && ds->ds_handler_ctxt) {
		Block_release(ds->ds_handler_ctxt);
	}
	ds->ds_handler_func = bl ? (void *)bl->invoke : NULL;
	ds->ds_handler_ctxt = bl;
	ds->ds_handler_is_block = true;
}

void
dispatch_source_set_event_handler(dispatch_source_t ds, dispatch_block_t handler)
{
	dispatch_assert(!ds->ds_is_legacy);
	handler = _dispatch_Block_copy(handler);
	dispatch_barrier_async_f((dispatch_queue_t)ds,
		handler, _dispatch_source_set_event_handler2);
}

static void
_dispatch_source_set_event_handler_f(void *context)
{
	dispatch_source_t ds = (dispatch_source_t)_dispatch_queue_get_current();
	dispatch_assert(ds->do_vtable == &_dispatch_source_kevent_vtable);
	
	if (ds->ds_handler_is_block && ds->ds_handler_ctxt) {
		Block_release(ds->ds_handler_ctxt);
	}
	ds->ds_handler_func = context;
	ds->ds_handler_ctxt = ds->do_ctxt;
	ds->ds_handler_is_block = false;
}

void
dispatch_source_set_event_handler_f(dispatch_source_t ds,
	dispatch_function_t handler)
{
	dispatch_assert(!ds->ds_is_legacy);
	dispatch_barrier_async_f((dispatch_queue_t)ds,
		handler, _dispatch_source_set_event_handler_f);
}

// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
static void
_dispatch_source_set_cancel_handler2(void *context)
{
	dispatch_source_t ds = (dispatch_source_t)_dispatch_queue_get_current();
	dispatch_assert(ds->do_vtable == &_dispatch_source_kevent_vtable);
	
	if (ds->ds_cancel_is_block && ds->ds_cancel_handler) {
		Block_release(ds->ds_cancel_handler);
	}
	ds->ds_cancel_handler = context;
	ds->ds_cancel_is_block = true;
}

void
dispatch_source_set_cancel_handler(dispatch_source_t ds,
	dispatch_block_t handler)
{
	dispatch_assert(!ds->ds_is_legacy);
	handler = _dispatch_Block_copy(handler);
	dispatch_barrier_async_f((dispatch_queue_t)ds,
							 handler, _dispatch_source_set_cancel_handler2);
}

static void
_dispatch_source_set_cancel_handler_f(void *context)
{
	dispatch_source_t ds = (dispatch_source_t)_dispatch_queue_get_current();
	dispatch_assert(ds->do_vtable == &_dispatch_source_kevent_vtable);
	
	if (ds->ds_cancel_is_block && ds->ds_cancel_handler) {
		Block_release(ds->ds_cancel_handler);
	}
	ds->ds_cancel_handler = context;
	ds->ds_cancel_is_block = false;
}

void
dispatch_source_set_cancel_handler_f(dispatch_source_t ds,
	dispatch_function_t handler)
{
	dispatch_assert(!ds->ds_is_legacy);
	dispatch_barrier_async_f((dispatch_queue_t)ds,
							 handler, _dispatch_source_set_cancel_handler_f);
}

#ifndef DISPATCH_NO_LEGACY
// 6618342 Contact the team that owns the Instrument DTrace probe before renaming this symbol
dispatch_source_t
_dispatch_source_create2(dispatch_source_t ds,
	dispatch_source_attr_t attr,
	void *context,
	dispatch_source_handler_function_t handler)
{
	if (ds == NULL || handler == NULL) {
		return NULL;
	}

	ds->ds_is_legacy = true;

	ds->ds_handler_func = handler;
	ds->ds_handler_ctxt = context;
		
	if (attr && attr != DISPATCH_SOURCE_CREATE_SUSPENDED) {
		ds->dq_finalizer_ctxt = attr->finalizer_ctxt;
		ds->dq_finalizer_func = (typeof(ds->dq_finalizer_func))attr->finalizer_func;
		ds->do_ctxt = attr->context;
	}
#ifdef __BLOCKS__
	if (ds->dq_finalizer_func == (void*)_dispatch_call_block_and_release2) {
		ds->dq_finalizer_ctxt = Block_copy(ds->dq_finalizer_ctxt);
		if (!ds->dq_finalizer_ctxt) {
			goto out_bad;
		}
	}
	if (handler == _dispatch_source_call_block) {
		struct Block_layout *bl = ds->ds_handler_ctxt = Block_copy(context);
		if (!ds->ds_handler_ctxt) {
			if (ds->dq_finalizer_func == (void*)_dispatch_call_block_and_release2) {
				Block_release(ds->dq_finalizer_ctxt);
			}
			goto out_bad;
		}
		ds->ds_handler_func = (void *)bl->invoke;
		ds->ds_handler_is_block = true;
	}

	// all legacy sources get a cancellation event on the normal event handler.
	dispatch_source_handler_function_t func = ds->ds_handler_func;
	dispatch_source_handler_t block = ds->ds_handler_ctxt;
	void *ctxt = ds->ds_handler_ctxt;
	bool handler_is_block = ds->ds_handler_is_block;
	
	ds->ds_cancel_is_block = true;
	if (handler_is_block) {
		ds->ds_cancel_handler = _dispatch_Block_copy(^{
			block(ds);
		});
	} else {
		ds->ds_cancel_handler = _dispatch_Block_copy(^{
			func(ctxt, ds);
		});
	}
#endif
	if (attr != DISPATCH_SOURCE_CREATE_SUSPENDED) {
		dispatch_resume(ds);
	}

	return ds;

out_bad:
	free(ds);
	return NULL;
}

long
dispatch_source_get_error(dispatch_source_t ds, long *err_out)
{
	// 6863892 don't report ECANCELED until kevent is unregistered
	if ((ds->ds_atomic_flags & DSF_CANCELED) && !ds->ds_dkev) {
		if (err_out) {
			*err_out = ECANCELED;
		}
		return DISPATCH_ERROR_DOMAIN_POSIX;
	} else {
		return DISPATCH_ERROR_DOMAIN_NO_ERROR;
	}
}
#endif /* DISPATCH_NO_LEGACY */

// Updates the ordered list of timers based on next fire date for changes to ds.
// Should only be called from the context of _dispatch_mgr_q.
void
_dispatch_timer_list_update(dispatch_source_t ds)
{
	dispatch_source_t dsi = NULL;
	int idx;
	
	dispatch_assert(_dispatch_queue_get_current() == &_dispatch_mgr_q);

	// do not reschedule timers unregistered with _dispatch_kevent_release()
	if (!ds->ds_dkev) {
		return;
	}

	// Ensure the source is on the global kevent lists before it is removed and
	// readded below.
	_dispatch_kevent_merge(ds);
	
	TAILQ_REMOVE(&ds->ds_dkev->dk_sources, ds, ds_list);

	// change the list if the clock type has changed
	if (ds->ds_timer.flags & DISPATCH_TIMER_WALL_CLOCK) {
		idx = DISPATCH_TIMER_INDEX_WALL;
	} else {
		idx = DISPATCH_TIMER_INDEX_MACH;
	}
	ds->ds_dkev = &_dispatch_kevent_timer[idx];

	if (ds->ds_timer.target) {
		TAILQ_FOREACH(dsi, &ds->ds_dkev->dk_sources, ds_list) {
			if (dsi->ds_timer.target == 0 || ds->ds_timer.target < dsi->ds_timer.target) {
				break;
			}
		}
	}
	
	if (dsi) {
		TAILQ_INSERT_BEFORE(dsi, ds, ds_list);
	} else {
		TAILQ_INSERT_TAIL(&ds->ds_dkev->dk_sources, ds, ds_list);
	}
}

static void
_dispatch_run_timers2(unsigned int timer)
{
	dispatch_source_t ds;
	uint64_t now, missed;

	if (timer == DISPATCH_TIMER_INDEX_MACH) {
		now = mach_absolute_time();
	} else {
		now = _dispatch_get_nanoseconds();
	}

	while ((ds = TAILQ_FIRST(&_dispatch_kevent_timer[timer].dk_sources))) {
		// We may find timers on the wrong list due to a pending update from
		// dispatch_source_set_timer. Force an update of the list in that case.
		if (timer != ds->ds_ident_hack) {
			_dispatch_timer_list_update(ds);
			continue;
		}
		if (!ds->ds_timer.target) {
			// no configured timers on the list
			break;
		}
		if (ds->ds_timer.target > now) {
			// Done running timers for now.
			break;
		}

		if (ds->ds_timer.flags & (DISPATCH_TIMER_ONESHOT|DISPATCH_TIMER_ABSOLUTE)) {
			dispatch_atomic_inc(&ds->ds_pending_data);
			ds->ds_timer.target = 0;
		} else {
			// Calculate number of missed intervals.
			missed = (now - ds->ds_timer.target) / ds->ds_timer.interval;
			dispatch_atomic_add(&ds->ds_pending_data, missed + 1);
			ds->ds_timer.target += (missed + 1) * ds->ds_timer.interval;
		}

		_dispatch_timer_list_update(ds);
		_dispatch_wakeup(ds);
	}
}

void
_dispatch_run_timers(void)
{
	unsigned int i;
	for (i = 0; i < DISPATCH_TIMER_COUNT; i++) {
		_dispatch_run_timers2(i);
	}
}

#if defined(__i386__) || defined(__x86_64__)
// these architectures always return mach_absolute_time() in nanoseconds
#define _dispatch_convert_mach2nano(x) (x)
#define _dispatch_convert_nano2mach(x) (x)
#else
static mach_timebase_info_data_t tbi;
static dispatch_once_t tbi_pred;

static void
_dispatch_convert_init(void *context __attribute__((unused)))
{
	dispatch_assume_zero(mach_timebase_info(&tbi));
}

static uint64_t
_dispatch_convert_mach2nano(uint64_t val)
{
#ifdef __LP64__
	__uint128_t tmp;
#else
	long double tmp;
#endif

	dispatch_once_f(&tbi_pred, NULL, _dispatch_convert_init);

	tmp = val;
	tmp *= tbi.numer;
	tmp /= tbi.denom;

	return tmp;
}

static uint64_t
_dispatch_convert_nano2mach(uint64_t val)
{
#ifdef __LP64__
	__uint128_t tmp;
#else
	long double tmp;
#endif

	dispatch_once_f(&tbi_pred, NULL, _dispatch_convert_init);

	tmp = val;
	tmp *= tbi.denom;
	tmp /= tbi.numer;

	return tmp;
}
#endif

// approx 1 year (60s * 60m * 24h * 365d)
#define FOREVER_SEC 3153600l
#define FOREVER_NSEC 31536000000000000ull

struct timespec *
_dispatch_get_next_timer_fire(struct timespec *howsoon)
{
	// <rdar://problem/6459649>
	// kevent(2) does not allow large timeouts, so we use a long timeout
	// instead (approximately 1 year).
	dispatch_source_t ds = NULL;
	unsigned int timer;
	uint64_t now, delta_tmp, delta = UINT64_MAX;

	// We are looking for the first unsuspended timer which has its target
	// time set. Given timers are kept in order, if we hit an timer that's
	// unset there's no point in continuing down the list.
	for (timer = 0; timer < DISPATCH_TIMER_COUNT; timer++) {
		TAILQ_FOREACH(ds, &_dispatch_kevent_timer[timer].dk_sources, ds_list) {
			if (!ds->ds_timer.target) {
				break;
			}
			if (DISPATCH_OBJECT_SUSPENDED(ds)) {
				ds->ds_is_armed = false;
			} else {
				break;
			}
		}

		if (!ds || !ds->ds_timer.target) {
			continue;
		}
				
		if (ds->ds_timer.flags & DISPATCH_TIMER_WALL_CLOCK) {
			now = _dispatch_get_nanoseconds();
		} else {
			now = mach_absolute_time();
		}
		if (ds->ds_timer.target <= now) {
			howsoon->tv_sec = 0;
			howsoon->tv_nsec = 0;
			return howsoon;
		}

		// the subtraction cannot go negative because the previous "if"
		// verified that the target is greater than now.
		delta_tmp = ds->ds_timer.target - now;
		if (!(ds->ds_timer.flags & DISPATCH_TIMER_WALL_CLOCK)) {
			delta_tmp = _dispatch_convert_mach2nano(delta_tmp);
		}
		if (delta_tmp < delta) {
			delta = delta_tmp;
		}
	}
	if (slowpath(delta > FOREVER_NSEC)) {
		return NULL;
	} else {
		howsoon->tv_sec = (time_t)(delta / NSEC_PER_SEC);
		howsoon->tv_nsec = (long)(delta % NSEC_PER_SEC);
	}
	return howsoon;
}

struct dispatch_set_timer_params {
	dispatch_source_t ds;
	uintptr_t ident;
	struct dispatch_timer_source_s values;
};

// To be called from the context of the _dispatch_mgr_q
static void
_dispatch_source_set_timer2(void *context)
{
	struct dispatch_set_timer_params *params = context;
	dispatch_source_t ds = params->ds;
	ds->ds_ident_hack = params->ident;
	ds->ds_timer = params->values;
	_dispatch_timer_list_update(ds);
	dispatch_resume(ds);
	dispatch_release(ds);
	free(params);
}

void
dispatch_source_set_timer(dispatch_source_t ds,
	dispatch_time_t start,
	uint64_t interval,
	uint64_t leeway)
{
	struct dispatch_set_timer_params *params;
	
	// we use zero internally to mean disabled
	if (interval == 0) {
		interval = 1;
	} else if ((int64_t)interval < 0) {
		// 6866347 - make sure nanoseconds won't overflow
		interval = INT64_MAX;
	}

	// Suspend the source so that it doesn't fire with pending changes
	// The use of suspend/resume requires the external retain/release
	dispatch_retain(ds);
	dispatch_suspend(ds);
	
	if (start == DISPATCH_TIME_NOW) {
		start = mach_absolute_time();
	} else if (start == DISPATCH_TIME_FOREVER) {
		start = INT64_MAX;
	}

	while (!(params = malloc(sizeof(struct dispatch_set_timer_params)))) {
		sleep(1);
	}

	params->ds = ds;
	params->values.flags = ds->ds_timer.flags;

	if ((int64_t)start < 0) {
		// wall clock
		params->ident = DISPATCH_TIMER_INDEX_WALL;
		params->values.start = -((int64_t)start);
		params->values.target = -((int64_t)start);
		params->values.interval = interval;
		params->values.leeway = leeway;
		params->values.flags |= DISPATCH_TIMER_WALL_CLOCK;
	} else {
		// mach clock
		params->ident = DISPATCH_TIMER_INDEX_MACH;
		params->values.start = start;
		params->values.target = start;
		params->values.interval = _dispatch_convert_nano2mach(interval);
		params->values.leeway = _dispatch_convert_nano2mach(leeway);
		params->values.flags &= ~DISPATCH_TIMER_WALL_CLOCK;
	}

	dispatch_barrier_async_f(&_dispatch_mgr_q, params, _dispatch_source_set_timer2);
}

#ifndef DISPATCH_NO_LEGACY
// LEGACY
long
dispatch_source_timer_set_time(dispatch_source_t ds, uint64_t nanoseconds, uint64_t leeway)
{
	dispatch_time_t start;
	if (nanoseconds == 0) {
		nanoseconds = 1;
	}
	if (ds->ds_timer.flags == (DISPATCH_TIMER_ABSOLUTE|DISPATCH_TIMER_WALL_CLOCK)) {
		static const struct timespec t0;
		start = dispatch_walltime(&t0, nanoseconds);
	} else if (ds->ds_timer.flags & DISPATCH_TIMER_WALL_CLOCK) {
		start = dispatch_walltime(DISPATCH_TIME_NOW, nanoseconds);
	} else {
		start = dispatch_time(DISPATCH_TIME_NOW, nanoseconds);
	}
	if (ds->ds_timer.flags & (DISPATCH_TIMER_ABSOLUTE|DISPATCH_TIMER_ONESHOT)) {
		// 6866347 - make sure nanoseconds won't overflow
		nanoseconds = INT64_MAX; // non-repeating (~292 years)
	}
	dispatch_source_set_timer(ds, start, nanoseconds, leeway);
	return 0;
}

// LEGACY
uint64_t
dispatch_event_get_nanoseconds(dispatch_source_t ds)
{
	if (ds->ds_timer.flags & DISPATCH_TIMER_WALL_CLOCK) {
		return ds->ds_timer.interval;
	} else {
		return _dispatch_convert_mach2nano(ds->ds_timer.interval);
	}
}
#endif /* DISPATCH_NO_LEGACY */

static dispatch_source_t _dispatch_mach_notify_source;
static mach_port_t _dispatch_port_set;
static mach_port_t _dispatch_event_port;

#define _DISPATCH_IS_POWER_OF_TWO(v)	(!(v & (v - 1)) && v)
#define _DISPATCH_HASH(x, y)    (_DISPATCH_IS_POWER_OF_TWO(y) ? (MACH_PORT_INDEX(x) & ((y) - 1)) : (MACH_PORT_INDEX(x) % (y)))

#define _DISPATCH_MACHPORT_HASH_SIZE 32
#define _DISPATCH_MACHPORT_HASH(x)    _DISPATCH_HASH((x), _DISPATCH_MACHPORT_HASH_SIZE)

static void _dispatch_port_set_init(void *);
static mach_port_t _dispatch_get_port_set(void);

void
_dispatch_drain_mach_messages(struct kevent *ke)
{
	dispatch_source_t dsi;
	dispatch_kevent_t dk;
	struct kevent ke2;

	if (!dispatch_assume(ke->data)) {
		return;
	}
	dk = _dispatch_kevent_find(ke->data, EVFILT_MACHPORT);
	if (!dispatch_assume(dk)) {
		return;
	}
	_dispatch_kevent_machport_disable(dk);	// emulate EV_DISPATCH

	EV_SET(&ke2, ke->data, EVFILT_MACHPORT, EV_ADD|EV_ENABLE|EV_DISPATCH, DISPATCH_MACHPORT_RECV, 0, dk);

	TAILQ_FOREACH(dsi, &dk->dk_sources, ds_list) {
		_dispatch_source_merge_kevent(dsi, &ke2);
	}
}

void
_dispatch_port_set_init(void *context __attribute__((unused)))
{
	struct kevent kev = {
		.filter = EVFILT_MACHPORT,
		.flags = EV_ADD,
	};
	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &_dispatch_port_set);
	DISPATCH_VERIFY_MIG(kr);
	dispatch_assume_zero(kr);
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &_dispatch_event_port);
	DISPATCH_VERIFY_MIG(kr);
	dispatch_assume_zero(kr);
	kr = mach_port_move_member(mach_task_self(), _dispatch_event_port, _dispatch_port_set);
	DISPATCH_VERIFY_MIG(kr);
	dispatch_assume_zero(kr);

	kev.ident = _dispatch_port_set;

	_dispatch_update_kq(&kev);
}

mach_port_t
_dispatch_get_port_set(void)
{
	static dispatch_once_t pred;

	dispatch_once_f(&pred, NULL, _dispatch_port_set_init);

	return _dispatch_port_set;
}

void
_dispatch_kevent_machport_resume(dispatch_kevent_t dk, uint32_t new_flags, uint32_t del_flags)
{
	mach_port_t previous, port = (mach_port_t)dk->dk_kevent.ident;
	kern_return_t kr;

	if ((new_flags & DISPATCH_MACHPORT_RECV) || (!new_flags && !del_flags && dk->dk_kevent.fflags & DISPATCH_MACHPORT_RECV)) {
		_dispatch_kevent_machport_enable(dk);
	}
	if (new_flags & DISPATCH_MACHPORT_DEAD) {
		kr = mach_port_request_notification(mach_task_self(), port, MACH_NOTIFY_DEAD_NAME, 1,
				_dispatch_event_port, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
		DISPATCH_VERIFY_MIG(kr);

	
		switch(kr) {
			case KERN_INVALID_NAME:
			case KERN_INVALID_RIGHT:
				// Supress errors 
				break;
			default:
			// Else, we dont expect any errors from mach. Log any errors if we do
			if (dispatch_assume_zero(kr)) {
				// log the error
			} else if (dispatch_assume_zero(previous)) {
				// Another subsystem has beat libdispatch to requesting the Mach
				// dead-name notification on this port. We should technically cache the
				// previous port and message it when the kernel messages our port. Or
				// we can just say screw those subsystems and drop the previous port.
				// They should adopt libdispatch :-P
				kr = mach_port_deallocate(mach_task_self(), previous);
				DISPATCH_VERIFY_MIG(kr);
				dispatch_assume_zero(kr);
			}
		}
	}

	if (del_flags & DISPATCH_MACHPORT_RECV) {
		_dispatch_kevent_machport_disable(dk);
	}
	if (del_flags & DISPATCH_MACHPORT_DEAD) {
		kr = mach_port_request_notification(mach_task_self(), (mach_port_t)dk->dk_kevent.ident,
				MACH_NOTIFY_DEAD_NAME, 1, MACH_PORT_NULL, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
		DISPATCH_VERIFY_MIG(kr);

		switch (kr) {
			case KERN_INVALID_NAME:
			case KERN_INVALID_RIGHT:
			case KERN_INVALID_ARGUMENT:
				break;
			default:
			if (dispatch_assume_zero(kr)) {
				// log the error
			} else if (previous) {
				// the kernel has not consumed the right yet
				dispatch_assume_zero(_dispatch_send_consume_send_once_right(previous));
			}
		}
	}
}

void
_dispatch_kevent_machport_enable(dispatch_kevent_t dk)
{
	mach_port_t mp = (mach_port_t)dk->dk_kevent.ident;
	kern_return_t kr;

	kr = mach_port_move_member(mach_task_self(), mp, _dispatch_get_port_set());
	DISPATCH_VERIFY_MIG(kr);
	switch (kr) {
	case KERN_INVALID_NAME:
#if DISPATCH_DEBUG
		_dispatch_log("Corruption: Mach receive right 0x%x destroyed prematurely", mp);
#endif
		break;
	default:
	 	dispatch_assume_zero(kr);
	}
}

void
_dispatch_kevent_machport_disable(dispatch_kevent_t dk)
{
	mach_port_t mp = (mach_port_t)dk->dk_kevent.ident;
	kern_return_t kr;

	kr = mach_port_move_member(mach_task_self(), mp, 0);
	DISPATCH_VERIFY_MIG(kr);
	switch (kr) {
	case KERN_INVALID_RIGHT:
	case KERN_INVALID_NAME:
#if DISPATCH_DEBUG
		_dispatch_log("Corruption: Mach receive right 0x%x destroyed prematurely", mp);
#endif
		break;
	case 0:
		break;
	default:
		dispatch_assume_zero(kr);
		break;
	}
}

#define _DISPATCH_MIN_MSG_SZ (8ul * 1024ul - MAX_TRAILER_SIZE)
#ifndef DISPATCH_NO_LEGACY
dispatch_source_t
dispatch_source_mig_create(mach_port_t mport, size_t max_msg_size, dispatch_source_attr_t attr,
	dispatch_queue_t dq, dispatch_mig_callback_t mig_callback)
{
	if (max_msg_size < _DISPATCH_MIN_MSG_SZ) {
		max_msg_size = _DISPATCH_MIN_MSG_SZ;
	}
	return dispatch_source_machport_create(mport, DISPATCH_MACHPORT_RECV, attr, dq,
	^(dispatch_source_t ds) {
		if (!dispatch_source_get_error(ds, NULL)) {
			if (dq->dq_width != 1) {
				dispatch_retain(ds);	// this is a shim -- use the external retain
				dispatch_async(dq, ^{
					dispatch_mig_server(ds, max_msg_size, mig_callback);
					dispatch_release(ds);	// this is a shim -- use the external release
				});
			} else {
				dispatch_mig_server(ds, max_msg_size, mig_callback);
			}
		}
	});	
}
#endif /* DISPATCH_NO_LEGACY */

static void
_dispatch_mach_notify_source_init(void *context __attribute__((unused)))
{
	size_t maxsz = sizeof(union __RequestUnion___dispatch_send_libdispatch_internal_protocol_subsystem);

	if (sizeof(union __ReplyUnion___dispatch_libdispatch_internal_protocol_subsystem) > maxsz) {
		maxsz = sizeof(union __ReplyUnion___dispatch_libdispatch_internal_protocol_subsystem);
	}

	_dispatch_get_port_set();

	_dispatch_mach_notify_source = dispatch_source_mig_create(_dispatch_event_port,
			maxsz, NULL, &_dispatch_mgr_q, libdispatch_internal_protocol_server);

	dispatch_assert(_dispatch_mach_notify_source);
}

kern_return_t
_dispatch_mach_notify_port_deleted(mach_port_t notify __attribute__((unused)), mach_port_name_t name)
{
	dispatch_source_t dsi;
	dispatch_kevent_t dk;
	struct kevent kev;

#if DISPATCH_DEBUG
	_dispatch_log("Corruption: Mach send/send-once/dead-name right 0x%x deleted prematurely", name);
#endif

	dk = _dispatch_kevent_find(name, EVFILT_MACHPORT);
	if (!dk) {
		goto out;
	}

	EV_SET(&kev, name, EVFILT_MACHPORT, EV_ADD|EV_ENABLE|EV_DISPATCH|EV_EOF, DISPATCH_MACHPORT_DELETED, 0, dk);

	TAILQ_FOREACH(dsi, &dk->dk_sources, ds_list) {
		_dispatch_source_merge_kevent(dsi, &kev);
		// this can never happen again
		// this must happen after the merge
		// this may be racy in the future, but we don't provide a 'setter' API for the mask yet
		dsi->ds_pending_data_mask &= ~DISPATCH_MACHPORT_DELETED;
	}

	// no more sources have this flag
	dk->dk_kevent.fflags &= ~DISPATCH_MACHPORT_DELETED;

out:
	return KERN_SUCCESS;
}

kern_return_t
_dispatch_mach_notify_port_destroyed(mach_port_t notify __attribute__((unused)), mach_port_t name)
{
	kern_return_t kr;
	// this function should never be called
	dispatch_assume_zero(name);
	kr = mach_port_mod_refs(mach_task_self(), name, MACH_PORT_RIGHT_RECEIVE, -1);
	DISPATCH_VERIFY_MIG(kr);
	dispatch_assume_zero(kr);
	return KERN_SUCCESS;
}

kern_return_t
_dispatch_mach_notify_no_senders(mach_port_t notify, mach_port_mscount_t mscnt __attribute__((unused)))
{
	// this function should never be called
	dispatch_assume_zero(notify);
	return KERN_SUCCESS;
}

kern_return_t
_dispatch_mach_notify_send_once(mach_port_t notify __attribute__((unused)))
{
	// we only register for dead-name notifications
	// some code deallocated our send-once right without consuming it
#if DISPATCH_DEBUG
	_dispatch_log("Corruption: An app/library deleted a libdispatch dead-name notification");
#endif
	return KERN_SUCCESS;
}

kern_return_t
_dispatch_mach_notify_dead_name(mach_port_t notify __attribute__((unused)), mach_port_name_t name)
{
	dispatch_source_t dsi;
	dispatch_kevent_t dk;
	struct kevent kev;
	kern_return_t kr;

	dk = _dispatch_kevent_find(name, EVFILT_MACHPORT);
	if (!dk) {
		goto out;
	}

	EV_SET(&kev, name, EVFILT_MACHPORT, EV_ADD|EV_ENABLE|EV_DISPATCH|EV_EOF, DISPATCH_MACHPORT_DEAD, 0, dk);

	TAILQ_FOREACH(dsi, &dk->dk_sources, ds_list) {
		_dispatch_source_merge_kevent(dsi, &kev);
		// this can never happen again
		// this must happen after the merge
		// this may be racy in the future, but we don't provide a 'setter' API for the mask yet
		dsi->ds_pending_data_mask &= ~DISPATCH_MACHPORT_DEAD;
	}

	// no more sources have this flag
	dk->dk_kevent.fflags &= ~DISPATCH_MACHPORT_DEAD;

out:
	// the act of receiving a dead name notification allocates a dead-name right that must be deallocated
	kr = mach_port_deallocate(mach_task_self(), name);
	DISPATCH_VERIFY_MIG(kr);
	//dispatch_assume_zero(kr);

	return KERN_SUCCESS;
}

kern_return_t
_dispatch_wakeup_main_thread(mach_port_t mp __attribute__((unused)))
{
	// dummy function just to pop out the main thread out of mach_msg()
	return 0;
}

kern_return_t
_dispatch_consume_send_once_right(mach_port_t mp __attribute__((unused)))
{
	// dummy function to consume a send-once right
	return 0;
}

mach_msg_return_t
dispatch_mig_server(dispatch_source_t ds, size_t maxmsgsz, dispatch_mig_callback_t callback)
{
	mach_msg_options_t options = MACH_RCV_MSG | MACH_RCV_TIMEOUT
		| MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_CTX)
		| MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0);
	mach_msg_options_t tmp_options = options;
	mig_reply_error_t *bufTemp, *bufRequest, *bufReply;
	mach_msg_return_t kr = 0;
	unsigned int cnt = 1000;	// do not stall out serial queues
	int demux_success;

	maxmsgsz += MAX_TRAILER_SIZE;

	// XXX FIXME -- allocate these elsewhere
	bufRequest = alloca(maxmsgsz);
	bufReply = alloca(maxmsgsz);
	bufReply->Head.msgh_size = 0;	// make CLANG happy

	// XXX FIXME -- change this to not starve out the target queue
	for (;;) {
		if (DISPATCH_OBJECT_SUSPENDED(ds) || (--cnt == 0)) {
			options &= ~MACH_RCV_MSG;
			tmp_options &= ~MACH_RCV_MSG;

			if (!(tmp_options & MACH_SEND_MSG)) {
				break;
			}
		}

		kr = mach_msg(&bufReply->Head, tmp_options, bufReply->Head.msgh_size,
				(mach_msg_size_t)maxmsgsz, (mach_port_t)ds->ds_ident_hack, 0, 0);

		tmp_options = options;

		if (slowpath(kr)) {
			switch (kr) {
			case MACH_SEND_INVALID_DEST:
			case MACH_SEND_TIMED_OUT:
				if (bufReply->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) {
					mach_msg_destroy(&bufReply->Head);
				}
				break;
			case MACH_RCV_TIMED_OUT:
			case MACH_RCV_INVALID_NAME:
				break;
			default:
				dispatch_assume_zero(kr);
				break;
			}
			break;
		}

		if (!(tmp_options & MACH_RCV_MSG)) {
			break;
		}

		bufTemp = bufRequest;
		bufRequest = bufReply;
		bufReply = bufTemp;

		demux_success = callback(&bufRequest->Head, &bufReply->Head);

		if (!demux_success) {
			// destroy the request - but not the reply port
			bufRequest->Head.msgh_remote_port = 0;
			mach_msg_destroy(&bufRequest->Head);
		} else if (!(bufReply->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
			// if MACH_MSGH_BITS_COMPLEX is _not_ set, then bufReply->RetCode is present
			if (slowpath(bufReply->RetCode)) {
				if (bufReply->RetCode == MIG_NO_REPLY) {
					continue;
				}

				// destroy the request - but not the reply port
				bufRequest->Head.msgh_remote_port = 0;
				mach_msg_destroy(&bufRequest->Head);
			}
		}

		if (bufReply->Head.msgh_remote_port) {
			tmp_options |= MACH_SEND_MSG;
			if (MACH_MSGH_BITS_REMOTE(bufReply->Head.msgh_bits) != MACH_MSG_TYPE_MOVE_SEND_ONCE) {
				tmp_options |= MACH_SEND_TIMEOUT;
			}
		}
	}

	return kr;
}
