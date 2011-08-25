/*
 * Copyright (c) 2008-2011 Apple Inc. All rights reserved.
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

// Contains exported global data and initialization & other routines that must
// only exist once in the shared library even when resolvers are used.

#include "internal.h"

#if HAVE_MACH
#include "protocolServer.h"
#endif

#pragma mark -
#pragma mark dispatch_init

#if USE_LIBDISPATCH_INIT_CONSTRUCTOR
DISPATCH_NOTHROW __attribute__((constructor))
void
_libdispatch_init(void);

DISPATCH_EXPORT DISPATCH_NOTHROW
void
_libdispatch_init(void)
{
	libdispatch_init();
}
#endif

DISPATCH_EXPORT DISPATCH_NOTHROW
void
dispatch_atfork_prepare(void)
{
}

DISPATCH_EXPORT DISPATCH_NOTHROW
void
dispatch_atfork_parent(void)
{
}

void
dummy_function(void)
{
}

long
dummy_function_r0(void)
{
	return 0;
}

#pragma mark -
#pragma mark dispatch_globals

#if DISPATCH_COCOA_COMPAT
// dispatch_begin_thread_4GC having non-default value triggers GC-only slow
// paths and is checked frequently, testing against NULL is faster than
// comparing for equality with "dummy_function"
void (*dispatch_begin_thread_4GC)(void) = NULL;
void (*dispatch_end_thread_4GC)(void) = dummy_function;
void (*dispatch_no_worker_threads_4GC)(void) = NULL;
void *(*_dispatch_begin_NSAutoReleasePool)(void) = (void *)dummy_function;
void (*_dispatch_end_NSAutoReleasePool)(void *) = (void *)dummy_function;
#endif

#if !DISPATCH_USE_DIRECT_TSD
pthread_key_t dispatch_queue_key;
pthread_key_t dispatch_sema4_key;
pthread_key_t dispatch_cache_key;
pthread_key_t dispatch_io_key;
pthread_key_t dispatch_apply_key;
#if DISPATCH_PERF_MON
pthread_key_t dispatch_bcounter_key;
#endif
#endif // !DISPATCH_USE_DIRECT_TSD

struct _dispatch_hw_config_s _dispatch_hw_config;
bool _dispatch_safe_fork = true;

const struct dispatch_queue_offsets_s dispatch_queue_offsets = {
	.dqo_version = 3,
	.dqo_label = offsetof(struct dispatch_queue_s, dq_label),
	.dqo_label_size = sizeof(((dispatch_queue_t)NULL)->dq_label),
	.dqo_flags = 0,
	.dqo_flags_size = 0,
	.dqo_width = offsetof(struct dispatch_queue_s, dq_width),
	.dqo_width_size = sizeof(((dispatch_queue_t)NULL)->dq_width),
	.dqo_serialnum = offsetof(struct dispatch_queue_s, dq_serialnum),
	.dqo_serialnum_size = sizeof(((dispatch_queue_t)NULL)->dq_serialnum),
	.dqo_running = offsetof(struct dispatch_queue_s, dq_running),
	.dqo_running_size = sizeof(((dispatch_queue_t)NULL)->dq_running),
};

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
DISPATCH_CACHELINE_ALIGN
struct dispatch_queue_s _dispatch_main_q = {
#if !DISPATCH_USE_RESOLVERS
	.do_vtable = &_dispatch_queue_vtable,
	.do_targetq = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_DEFAULT_OVERCOMMIT_PRIORITY],
#endif
	.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_suspend_cnt = DISPATCH_OBJECT_SUSPEND_LOCK,
	.dq_label = "com.apple.main-thread",
	.dq_running = 1,
	.dq_width = 1,
	.dq_serialnum = 1,
};

const struct dispatch_queue_attr_vtable_s dispatch_queue_attr_vtable = {
	.do_type = DISPATCH_QUEUE_ATTR_TYPE,
	.do_kind = "queue-attr",
};

struct dispatch_queue_attr_s _dispatch_queue_attr_concurrent = {
	.do_vtable = &dispatch_queue_attr_vtable,
	.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_next = DISPATCH_OBJECT_LISTLESS,
};

struct dispatch_data_s _dispatch_data_empty = {
#if !DISPATCH_USE_RESOLVERS
	.do_vtable = &_dispatch_data_vtable,
#endif
	.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_next = DISPATCH_OBJECT_LISTLESS,
};

const dispatch_block_t _dispatch_data_destructor_free = ^{
	DISPATCH_CRASH("free destructor called");
};

#pragma mark -
#pragma mark dispatch_log

static char _dispatch_build[16];

static void
_dispatch_bug_init(void *context DISPATCH_UNUSED)
{
#ifdef __APPLE__
	int mib[] = { CTL_KERN, KERN_OSVERSION };
	size_t bufsz = sizeof(_dispatch_build);

	sysctl(mib, 2, _dispatch_build, &bufsz, NULL, 0);
#else
	/*
	 * XXXRW: What to do here for !Mac OS X?
	 */
	memset(_dispatch_build, 0, sizeof(_dispatch_build));
#endif
}

void
_dispatch_bug(size_t line, long val)
{
	static dispatch_once_t pred;
	static void *last_seen;
	void *ra = __builtin_return_address(0);

	dispatch_once_f(&pred, NULL, _dispatch_bug_init);
	if (last_seen != ra) {
		last_seen = ra;
		_dispatch_log("BUG in libdispatch: %s - %lu - 0x%lx",
				_dispatch_build, (unsigned long)line, val);
	}
}

void
_dispatch_bug_mach_client(const char* msg, mach_msg_return_t kr)
{
	static void *last_seen;
	void *ra = __builtin_return_address(0);
	if (last_seen != ra) {
		last_seen = ra;
		_dispatch_log("BUG in libdispatch client: %s %s - 0x%x", msg,
				mach_error_string(kr), kr);
	}
}

void
_dispatch_abort(size_t line, long val)
{
	_dispatch_bug(line, val);
	abort();
}

void
_dispatch_log(const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	_dispatch_logv(msg, ap);
	va_end(ap);
}

static FILE *dispatch_logfile;
static bool dispatch_log_disabled;

static void
_dispatch_logv_init(void *context DISPATCH_UNUSED)
{
#if DISPATCH_DEBUG
	bool log_to_file = true;
#else
	bool log_to_file = false;
#endif
	char *e = getenv("LIBDISPATCH_LOG");
	if (e) {
		if (strcmp(e, "YES") == 0) {
			// default
		} else if (strcmp(e, "NO") == 0) {
			dispatch_log_disabled = true;
		} else if (strcmp(e, "syslog") == 0) {
			log_to_file = false;
		} else if (strcmp(e, "file") == 0) {
			log_to_file = true;
		} else if (strcmp(e, "stderr") == 0) {
			log_to_file = true;
			dispatch_logfile = stderr;
		}
	}
	if (!dispatch_log_disabled) {
		if (log_to_file && !dispatch_logfile) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "/var/tmp/libdispatch.%d.log",
					getpid());
			dispatch_logfile = fopen(path, "a");
		}
		if (dispatch_logfile) {
			struct timeval tv;
			gettimeofday(&tv, NULL);
			fprintf(dispatch_logfile, "=== log file opened for %s[%u] at "
					"%ld.%06u ===\n", getprogname() ?: "", getpid(),
					tv.tv_sec, tv.tv_usec);
			fflush(dispatch_logfile);
		}
	}
}

void
_dispatch_logv(const char *msg, va_list ap)
{
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_logv_init);

	if (slowpath(dispatch_log_disabled)) {
		return;
	}
	if (slowpath(dispatch_logfile)) {
		vfprintf(dispatch_logfile, msg, ap);
		// TODO: May cause interleaving with another thread's log
		fputc('\n', dispatch_logfile);
		fflush(dispatch_logfile);
		return;
	}
	vsyslog(LOG_NOTICE, msg, ap);
}

#pragma mark -
#pragma mark dispatch_debug

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

#pragma mark -
#pragma mark dispatch_block_t

#ifdef __BLOCKS__

#undef _dispatch_Block_copy
dispatch_block_t
_dispatch_Block_copy(dispatch_block_t db)
{
	dispatch_block_t rval;

	while (!(rval = Block_copy(db))) {
		sleep(1);
	}
	return rval;
}

void
_dispatch_call_block_and_release(void *block)
{
	void (^b)(void) = block;
	b();
	Block_release(b);
}

#endif // __BLOCKS__

#pragma mark -
#pragma mark dispatch_client_callout

#if DISPATCH_USE_CLIENT_CALLOUT

#undef _dispatch_client_callout
#undef _dispatch_client_callout2

DISPATCH_NOINLINE
void
_dispatch_client_callout(void *ctxt, dispatch_function_t f)
{
	return f(ctxt);
}

DISPATCH_NOINLINE
void
_dispatch_client_callout2(void *ctxt, size_t i, void (*f)(void *, size_t))
{
	return f(ctxt, i);
}

#endif

#pragma mark -
#pragma mark dispatch_source_types

static void
dispatch_source_type_timer_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED,
	unsigned long mask,
	dispatch_queue_t q DISPATCH_UNUSED)
{
	ds->ds_refs = calloc(1ul, sizeof(struct dispatch_timer_source_refs_s));
	if (slowpath(!ds->ds_refs)) return;
	ds->ds_needs_rearm = true;
	ds->ds_is_timer = true;
	ds_timer(ds->ds_refs).flags = mask;
}

const struct dispatch_source_type_s _dispatch_source_type_timer = {
	.ke = {
		.filter = DISPATCH_EVFILT_TIMER,
	},
	.mask = DISPATCH_TIMER_WALL_CLOCK,
	.init = dispatch_source_type_timer_init,
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

#if DISPATCH_USE_VM_PRESSURE
#if TARGET_IPHONE_SIMULATOR // rdar://problem/9219483
static int _dispatch_ios_simulator_memory_warnings_fd = -1;
static void
_dispatch_ios_simulator_vm_source_init(void *context DISPATCH_UNUSED)
{
	char *e = getenv("IPHONE_SIMULATOR_MEMORY_WARNINGS");
	if (!e) return;
	_dispatch_ios_simulator_memory_warnings_fd = open(e, O_EVTONLY);
	if (_dispatch_ios_simulator_memory_warnings_fd == -1) {
		(void)dispatch_assume_zero(errno);
	}
}
static void
dispatch_source_type_vm_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED,
	unsigned long mask,
	dispatch_queue_t q DISPATCH_UNUSED)
{
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_ios_simulator_vm_source_init);
	ds->ds_dkev->dk_kevent.ident = (mask & DISPATCH_VM_PRESSURE ?
			_dispatch_ios_simulator_memory_warnings_fd : -1);
}

const struct dispatch_source_type_s _dispatch_source_type_vm = {
	.ke = {
		.filter = EVFILT_VNODE,
		.flags = EV_CLEAR,
	},
	.mask = NOTE_ATTRIB,
	.init = dispatch_source_type_vm_init,
};
#else
static void
dispatch_source_type_vm_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED,
	unsigned long mask DISPATCH_UNUSED,
	dispatch_queue_t q DISPATCH_UNUSED)
{
	ds->ds_is_level = false;
}

const struct dispatch_source_type_s _dispatch_source_type_vm = {
	.ke = {
		.filter = EVFILT_VM,
		.flags = EV_DISPATCH,
	},
	.mask = NOTE_VM_PRESSURE,
	.init = dispatch_source_type_vm_init,
};
#endif
#endif

const struct dispatch_source_type_s _dispatch_source_type_proc = {
	.ke = {
		.filter = EVFILT_PROC,
		.flags = EV_CLEAR,
	},
	.mask = NOTE_EXIT|NOTE_FORK|NOTE_EXEC
#if HAVE_DECL_NOTE_SIGNAL
			|NOTE_SIGNAL
#endif
#if HAVE_DECL_NOTE_REAP
			|NOTE_REAP
#endif
			,
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
	.mask = NOTE_DELETE|NOTE_WRITE|NOTE_EXTEND|NOTE_ATTRIB|NOTE_LINK|
			NOTE_RENAME|NOTE_REVOKE
#if HAVE_DECL_NOTE_NONE
			|NOTE_NONE
#endif
			,
};

const struct dispatch_source_type_s _dispatch_source_type_vfs = {
	.ke = {
		.filter = EVFILT_FS,
		.flags = EV_CLEAR,
	},
	.mask = VQ_NOTRESP|VQ_NEEDAUTH|VQ_LOWDISK|VQ_MOUNT|VQ_UNMOUNT|VQ_DEAD|
			VQ_ASSIST|VQ_NOTRESPLOCK
#if HAVE_DECL_VQ_UPDATE
			|VQ_UPDATE
#endif
#if HAVE_DECL_VQ_VERYLOWDISK
			|VQ_VERYLOWDISK
#endif
			,
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

#if HAVE_MACH

static void
dispatch_source_type_mach_send_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED, unsigned long mask,
	dispatch_queue_t q DISPATCH_UNUSED)
{
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_mach_notify_source_init);
	if (!mask) {
		// Preserve legacy behavior that (mask == 0) => DISPATCH_MACH_SEND_DEAD
		ds->ds_dkev->dk_kevent.fflags = DISPATCH_MACH_SEND_DEAD;
		ds->ds_pending_data_mask = DISPATCH_MACH_SEND_DEAD;
	}
}

const struct dispatch_source_type_s _dispatch_source_type_mach_send = {
	.ke = {
		.filter = EVFILT_MACHPORT,
		.flags = EV_CLEAR,
	},
	.mask = DISPATCH_MACH_SEND_DEAD|DISPATCH_MACH_SEND_POSSIBLE,
	.init = dispatch_source_type_mach_send_init,
};

static void
dispatch_source_type_mach_recv_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED,
	unsigned long mask DISPATCH_UNUSED,
	dispatch_queue_t q DISPATCH_UNUSED)
{
	ds->ds_is_level = false;
}

const struct dispatch_source_type_s _dispatch_source_type_mach_recv = {
	.ke = {
		.filter = EVFILT_MACHPORT,
		.flags = EV_DISPATCH,
		.fflags = DISPATCH_MACH_RECV_MESSAGE,
	},
	.init = dispatch_source_type_mach_recv_init,
};

#pragma mark -
#pragma mark dispatch_mig

void *
dispatch_mach_msg_get_context(mach_msg_header_t *msg)
{
	mach_msg_context_trailer_t *tp;
	void *context = NULL;

	tp = (mach_msg_context_trailer_t *)((uint8_t *)msg +
			round_msg(msg->msgh_size));
	if (tp->msgh_trailer_size >=
			(mach_msg_size_t)sizeof(mach_msg_context_trailer_t)) {
		context = (void *)(uintptr_t)tp->msgh_context;
	}
	return context;
}

kern_return_t
_dispatch_wakeup_main_thread(mach_port_t mp DISPATCH_UNUSED)
{
	// dummy function just to pop out the main thread out of mach_msg()
	return 0;
}

kern_return_t
_dispatch_consume_send_once_right(mach_port_t mp DISPATCH_UNUSED)
{
	// dummy function to consume a send-once right
	return 0;
}

kern_return_t
_dispatch_mach_notify_port_destroyed(mach_port_t notify DISPATCH_UNUSED,
		mach_port_t name)
{
	kern_return_t kr;
	// this function should never be called
	(void)dispatch_assume_zero(name);
	kr = mach_port_mod_refs(mach_task_self(), name, MACH_PORT_RIGHT_RECEIVE,-1);
	DISPATCH_VERIFY_MIG(kr);
	(void)dispatch_assume_zero(kr);
	return KERN_SUCCESS;
}

kern_return_t
_dispatch_mach_notify_no_senders(mach_port_t notify,
		mach_port_mscount_t mscnt DISPATCH_UNUSED)
{
	// this function should never be called
	(void)dispatch_assume_zero(notify);
	return KERN_SUCCESS;
}

kern_return_t
_dispatch_mach_notify_send_once(mach_port_t notify DISPATCH_UNUSED)
{
	// we only register for dead-name notifications
	// some code deallocated our send-once right without consuming it
#if DISPATCH_DEBUG
	_dispatch_log("Corruption: An app/library deleted a libdispatch "
			"dead-name notification");
#endif
	return KERN_SUCCESS;
}


#endif // HAVE_MACH
