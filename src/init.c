/*
 * Copyright (c) 2008-2013 Apple Inc. All rights reserved.
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

#pragma mark -
#pragma mark dispatch_globals

DISPATCH_HIDE_SYMBOL(dispatch_assert_queue, 10.12, 10.0, 10.0, 3.0);
DISPATCH_HIDE_SYMBOL(dispatch_assert_queue_not, 10.12, 10.0, 10.0, 3.0);
DISPATCH_HIDE_SYMBOL(dispatch_queue_create_with_target, 10.12, 10.0, 10.0, 3.0);

#if DISPATCH_COCOA_COMPAT
void *(*_dispatch_begin_NSAutoReleasePool)(void);
void (*_dispatch_end_NSAutoReleasePool)(void *);
#endif

#if DISPATCH_USE_THREAD_LOCAL_STORAGE
__thread struct dispatch_tsd __dispatch_tsd;
pthread_key_t __dispatch_tsd_key;
#elif !DISPATCH_USE_DIRECT_TSD
pthread_key_t dispatch_queue_key;
pthread_key_t dispatch_frame_key;
pthread_key_t dispatch_cache_key;
pthread_key_t dispatch_context_key;
pthread_key_t dispatch_pthread_root_queue_observer_hooks_key;
pthread_key_t dispatch_defaultpriority_key;
#if DISPATCH_INTROSPECTION
pthread_key_t dispatch_introspection_key;
#elif DISPATCH_PERF_MON
pthread_key_t dispatch_bcounter_key;
#endif
pthread_key_t dispatch_sema4_key;
pthread_key_t dispatch_voucher_key;
pthread_key_t dispatch_deferred_items_key;
#endif // !DISPATCH_USE_DIRECT_TSD && !DISPATCH_USE_THREAD_LOCAL_STORAGE

#if VOUCHER_USE_MACH_VOUCHER
dispatch_once_t _voucher_task_mach_voucher_pred;
mach_voucher_t _voucher_task_mach_voucher;
#if !VOUCHER_USE_EMPTY_MACH_BASE_VOUCHER
mach_voucher_t _voucher_default_task_mach_voucher;
#endif
dispatch_once_t _firehose_task_buffer_pred;
firehose_buffer_t _firehose_task_buffer;
const uint32_t _firehose_spi_version = OS_FIREHOSE_SPI_VERSION;
uint64_t _voucher_unique_pid;
voucher_activity_hooks_t _voucher_libtrace_hooks;
dispatch_mach_t _voucher_activity_debug_channel;
#endif
#if HAVE_PTHREAD_WORKQUEUE_QOS && DISPATCH_DEBUG
int _dispatch_set_qos_class_enabled;
#endif
#if DISPATCH_USE_KEVENT_WORKQUEUE && DISPATCH_USE_MGR_THREAD
int _dispatch_kevent_workqueue_enabled;
#endif
#if DISPATCH_USE_EVFILT_MACHPORT_DIRECT && \
		DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK
int _dispatch_evfilt_machport_direct_enabled;
#endif

DISPATCH_HW_CONFIG();
uint8_t _dispatch_unsafe_fork;
bool _dispatch_child_of_unsafe_fork;
#if DISPATCH_USE_MEMORYPRESSURE_SOURCE
bool _dispatch_memory_warn;
int _dispatch_continuation_cache_limit = DISPATCH_CONTINUATION_CACHE_LIMIT;
#endif

DISPATCH_NOINLINE
bool
_dispatch_is_multithreaded(void)
{
	return _dispatch_is_multithreaded_inline();
}

DISPATCH_NOINLINE
bool
_dispatch_is_fork_of_multithreaded_parent(void)
{
	return _dispatch_child_of_unsafe_fork;
}

DISPATCH_NOINLINE
void
_dispatch_fork_becomes_unsafe_slow(void)
{
	uint8_t value = os_atomic_or(&_dispatch_unsafe_fork,
			_DISPATCH_UNSAFE_FORK_MULTITHREADED, relaxed);
	if (value & _DISPATCH_UNSAFE_FORK_PROHIBIT) {
		DISPATCH_CLIENT_CRASH(0, "Transition to multithreaded is prohibited");
	}
}

DISPATCH_NOINLINE
void
_dispatch_prohibit_transition_to_multithreaded(bool prohibit)
{
	if (prohibit) {
		uint8_t value = os_atomic_or(&_dispatch_unsafe_fork,
				_DISPATCH_UNSAFE_FORK_PROHIBIT, relaxed);
		if (value & _DISPATCH_UNSAFE_FORK_MULTITHREADED) {
			DISPATCH_CLIENT_CRASH(0, "The executable is already multithreaded");
		}
	} else {
		os_atomic_and(&_dispatch_unsafe_fork,
				(uint8_t)~_DISPATCH_UNSAFE_FORK_PROHIBIT, relaxed);
	}
}

const struct dispatch_queue_offsets_s dispatch_queue_offsets = {
	.dqo_version = 6,
	.dqo_label = offsetof(struct dispatch_queue_s, dq_label),
	.dqo_label_size = sizeof(((dispatch_queue_t)NULL)->dq_label),
	.dqo_flags = 0,
	.dqo_flags_size = 0,
	.dqo_serialnum = offsetof(struct dispatch_queue_s, dq_serialnum),
	.dqo_serialnum_size = sizeof(((dispatch_queue_t)NULL)->dq_serialnum),
	.dqo_width = offsetof(struct dispatch_queue_s, dq_width),
	.dqo_width_size = sizeof(((dispatch_queue_t)NULL)->dq_width),
	.dqo_running = 0,
	.dqo_running_size = 0,
	.dqo_suspend_cnt = 0,
	.dqo_suspend_cnt_size = 0,
	.dqo_target_queue = offsetof(struct dispatch_queue_s, do_targetq),
	.dqo_target_queue_size = sizeof(((dispatch_queue_t)NULL)->do_targetq),
	.dqo_priority = offsetof(struct dispatch_queue_s, dq_priority),
	.dqo_priority_size = sizeof(((dispatch_queue_t)NULL)->dq_priority),
};

#if DISPATCH_USE_DIRECT_TSD
const struct dispatch_tsd_indexes_s dispatch_tsd_indexes = {
	.dti_version = 2,
	.dti_queue_index = dispatch_queue_key,
	.dti_voucher_index = dispatch_voucher_key,
	.dti_qos_class_index = dispatch_priority_key,
};
#endif // DISPATCH_USE_DIRECT_TSD

// 6618342 Contact the team that owns the Instrument DTrace probe before
//         renaming this symbol
DISPATCH_CACHELINE_ALIGN
struct dispatch_queue_s _dispatch_main_q = {
	DISPATCH_GLOBAL_OBJECT_HEADER(queue_main),
#if !DISPATCH_USE_RESOLVERS
	.do_targetq = &_dispatch_root_queues[
			DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS_OVERCOMMIT],
#endif
	.dq_state = DISPATCH_QUEUE_STATE_INIT_VALUE(1),
	.dq_label = "com.apple.main-thread",
	.dq_width = 1,
	.dq_atomic_bits = DQF_THREAD_BOUND | DQF_CANNOT_TRYSYNC,
	.dq_override_voucher = DISPATCH_NO_VOUCHER,
	.dq_serialnum = 1,
};

#pragma mark -
#pragma mark dispatch_queue_attr_t

#define DISPATCH_QUEUE_ATTR_INIT(qos, prio, overcommit, freq, concurrent, inactive) \
		{ \
			DISPATCH_GLOBAL_OBJECT_HEADER(queue_attr), \
			.dqa_qos_class = (qos), \
			.dqa_relative_priority = (qos) ? (prio) : 0, \
			.dqa_overcommit = _dispatch_queue_attr_overcommit_##overcommit, \
			.dqa_autorelease_frequency = DISPATCH_AUTORELEASE_FREQUENCY_##freq, \
			.dqa_concurrent = (concurrent), \
			.dqa_inactive = (inactive), \
		}

#define DISPATCH_QUEUE_ATTR_ACTIVE_INIT(qos, prio, overcommit, freq, concurrent) \
		{ \
			[DQA_INDEX_ACTIVE] = DISPATCH_QUEUE_ATTR_INIT(\
					qos, prio, overcommit, freq, concurrent, false), \
			[DQA_INDEX_INACTIVE] = DISPATCH_QUEUE_ATTR_INIT(\
					qos, prio, overcommit, freq, concurrent, true), \
		}

#define DISPATCH_QUEUE_ATTR_OVERCOMMIT_INIT(qos, prio, overcommit) \
		{ \
			[DQA_INDEX_AUTORELEASE_FREQUENCY_INHERIT][DQA_INDEX_CONCURRENT] = \
					DISPATCH_QUEUE_ATTR_ACTIVE_INIT(qos, prio, overcommit, INHERIT, 1), \
			[DQA_INDEX_AUTORELEASE_FREQUENCY_INHERIT][DQA_INDEX_SERIAL] = \
					DISPATCH_QUEUE_ATTR_ACTIVE_INIT(qos, prio, overcommit, INHERIT, 0), \
			[DQA_INDEX_AUTORELEASE_FREQUENCY_WORK_ITEM][DQA_INDEX_CONCURRENT] = \
					DISPATCH_QUEUE_ATTR_ACTIVE_INIT(qos, prio, overcommit, WORK_ITEM, 1), \
			[DQA_INDEX_AUTORELEASE_FREQUENCY_WORK_ITEM][DQA_INDEX_SERIAL] = \
					DISPATCH_QUEUE_ATTR_ACTIVE_INIT(qos, prio, overcommit, WORK_ITEM, 0), \
			[DQA_INDEX_AUTORELEASE_FREQUENCY_NEVER][DQA_INDEX_CONCURRENT] = \
					DISPATCH_QUEUE_ATTR_ACTIVE_INIT(qos, prio, overcommit, NEVER, 1), \
			[DQA_INDEX_AUTORELEASE_FREQUENCY_NEVER][DQA_INDEX_SERIAL] = \
					DISPATCH_QUEUE_ATTR_ACTIVE_INIT(qos, prio, overcommit, NEVER, 0), \
		}

#define DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, prio) \
		[prio] = { \
			[DQA_INDEX_UNSPECIFIED_OVERCOMMIT] = \
					DISPATCH_QUEUE_ATTR_OVERCOMMIT_INIT(qos, -(prio), unspecified), \
			[DQA_INDEX_NON_OVERCOMMIT] = \
					DISPATCH_QUEUE_ATTR_OVERCOMMIT_INIT(qos, -(prio), disabled), \
			[DQA_INDEX_OVERCOMMIT] = \
					DISPATCH_QUEUE_ATTR_OVERCOMMIT_INIT(qos, -(prio), enabled), \
		}

#define DISPATCH_QUEUE_ATTR_PRIO_INIT(qos) \
		{ \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 0), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 1), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 2), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 3), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 4), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 5), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 6), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 7), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 8), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 9), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 10), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 11), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 12), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 13), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 14), \
			DISPATCH_QUEUE_ATTR_PRIO_INITIALIZER(qos, 15), \
		}

#define DISPATCH_QUEUE_ATTR_QOS_INITIALIZER(qos) \
		[DQA_INDEX_QOS_CLASS_##qos] = \
				DISPATCH_QUEUE_ATTR_PRIO_INIT(_DISPATCH_QOS_CLASS_##qos)

// DISPATCH_QUEUE_CONCURRENT resp. _dispatch_queue_attr_concurrent is aliased
// to array member [0][0][0][0][0][0] and their properties must match!
const struct dispatch_queue_attr_s _dispatch_queue_attrs[]
		[DISPATCH_QUEUE_ATTR_PRIO_COUNT]
		[DISPATCH_QUEUE_ATTR_OVERCOMMIT_COUNT]
		[DISPATCH_QUEUE_ATTR_AUTORELEASE_FREQUENCY_COUNT]
		[DISPATCH_QUEUE_ATTR_CONCURRENCY_COUNT]
		[DISPATCH_QUEUE_ATTR_INACTIVE_COUNT] = {
	DISPATCH_QUEUE_ATTR_QOS_INITIALIZER(UNSPECIFIED),
	DISPATCH_QUEUE_ATTR_QOS_INITIALIZER(MAINTENANCE),
	DISPATCH_QUEUE_ATTR_QOS_INITIALIZER(BACKGROUND),
	DISPATCH_QUEUE_ATTR_QOS_INITIALIZER(UTILITY),
	DISPATCH_QUEUE_ATTR_QOS_INITIALIZER(DEFAULT),
	DISPATCH_QUEUE_ATTR_QOS_INITIALIZER(USER_INITIATED),
	DISPATCH_QUEUE_ATTR_QOS_INITIALIZER(USER_INTERACTIVE),
};

#if DISPATCH_VARIANT_STATIC
// <rdar://problem/16778703>
struct dispatch_queue_attr_s _dispatch_queue_attr_concurrent =
	DISPATCH_QUEUE_ATTR_INIT(_DISPATCH_QOS_CLASS_UNSPECIFIED, 0,
			unspecified, INHERIT, 1, false);
#endif // DISPATCH_VARIANT_STATIC

// _dispatch_queue_attr_concurrent is aliased using libdispatch.aliases
// and the -alias_list linker option on Darwin but needs to be done manually
// for other platforms.
#ifndef __APPLE__
extern struct dispatch_queue_attr_s _dispatch_queue_attr_concurrent
	__attribute__((__alias__("_dispatch_queue_attrs")));
#endif

#pragma mark -
#pragma mark dispatch_vtables

DISPATCH_VTABLE_INSTANCE(semaphore,
	.do_type = DISPATCH_SEMAPHORE_TYPE,
	.do_kind = "semaphore",
	.do_dispose = _dispatch_semaphore_dispose,
	.do_debug = _dispatch_semaphore_debug,
);

DISPATCH_VTABLE_INSTANCE(group,
	.do_type = DISPATCH_GROUP_TYPE,
	.do_kind = "group",
	.do_dispose = _dispatch_group_dispose,
	.do_debug = _dispatch_group_debug,
);

DISPATCH_VTABLE_INSTANCE(queue,
	.do_type = DISPATCH_QUEUE_LEGACY_TYPE,
	.do_kind = "queue",
	.do_dispose = _dispatch_queue_dispose,
	.do_suspend = _dispatch_queue_suspend,
	.do_resume = _dispatch_queue_resume,
	.do_invoke = _dispatch_queue_invoke,
	.do_wakeup = _dispatch_queue_wakeup,
	.do_debug = dispatch_queue_debug,
	.do_set_targetq = _dispatch_queue_set_target_queue,
);

DISPATCH_VTABLE_SUBCLASS_INSTANCE(queue_serial, queue,
	.do_type = DISPATCH_QUEUE_SERIAL_TYPE,
	.do_kind = "serial-queue",
	.do_dispose = _dispatch_queue_dispose,
	.do_suspend = _dispatch_queue_suspend,
	.do_resume = _dispatch_queue_resume,
	.do_finalize_activation = _dispatch_queue_finalize_activation,
	.do_invoke = _dispatch_queue_invoke,
	.do_wakeup = _dispatch_queue_wakeup,
	.do_debug = dispatch_queue_debug,
	.do_set_targetq = _dispatch_queue_set_target_queue,
);

DISPATCH_VTABLE_SUBCLASS_INSTANCE(queue_concurrent, queue,
	.do_type = DISPATCH_QUEUE_CONCURRENT_TYPE,
	.do_kind = "concurrent-queue",
	.do_dispose = _dispatch_queue_dispose,
	.do_suspend = _dispatch_queue_suspend,
	.do_resume = _dispatch_queue_resume,
	.do_finalize_activation = _dispatch_queue_finalize_activation,
	.do_invoke = _dispatch_queue_invoke,
	.do_wakeup = _dispatch_queue_wakeup,
	.do_debug = dispatch_queue_debug,
	.do_set_targetq = _dispatch_queue_set_target_queue,
);


DISPATCH_VTABLE_SUBCLASS_INSTANCE(queue_root, queue,
	.do_type = DISPATCH_QUEUE_GLOBAL_ROOT_TYPE,
	.do_kind = "global-queue",
	.do_dispose = _dispatch_pthread_root_queue_dispose,
	.do_wakeup = _dispatch_root_queue_wakeup,
	.do_debug = dispatch_queue_debug,
);

DISPATCH_VTABLE_SUBCLASS_INSTANCE(queue_main, queue,
	.do_type = DISPATCH_QUEUE_SERIAL_TYPE,
	.do_kind = "main-queue",
	.do_dispose = _dispatch_queue_dispose,
	.do_invoke = _dispatch_queue_invoke,
	.do_wakeup = _dispatch_main_queue_wakeup,
	.do_debug = dispatch_queue_debug,
);

DISPATCH_VTABLE_SUBCLASS_INSTANCE(queue_runloop, queue,
	.do_type = DISPATCH_QUEUE_RUNLOOP_TYPE,
	.do_kind = "runloop-queue",
	.do_dispose = _dispatch_runloop_queue_dispose,
	.do_invoke = _dispatch_queue_invoke,
	.do_wakeup = _dispatch_runloop_queue_wakeup,
	.do_debug = dispatch_queue_debug,
);

DISPATCH_VTABLE_SUBCLASS_INSTANCE(queue_mgr, queue,
	.do_type = DISPATCH_QUEUE_MGR_TYPE,
	.do_kind = "mgr-queue",
	.do_invoke = _dispatch_mgr_thread,
	.do_wakeup = _dispatch_mgr_queue_wakeup,
	.do_debug = dispatch_queue_debug,
);

DISPATCH_VTABLE_INSTANCE(queue_specific_queue,
	.do_type = DISPATCH_QUEUE_SPECIFIC_TYPE,
	.do_kind = "queue-context",
	.do_dispose = _dispatch_queue_specific_queue_dispose,
	.do_invoke = (void *)_dispatch_queue_invoke,
	.do_wakeup = (void *)_dispatch_queue_wakeup,
	.do_debug = (void *)dispatch_queue_debug,
);

DISPATCH_VTABLE_INSTANCE(queue_attr,
	.do_type = DISPATCH_QUEUE_ATTR_TYPE,
	.do_kind = "queue-attr",
);

DISPATCH_VTABLE_INSTANCE(source,
	.do_type = DISPATCH_SOURCE_KEVENT_TYPE,
	.do_kind = "kevent-source",
	.do_dispose = _dispatch_source_dispose,
	.do_suspend = (void *)_dispatch_queue_suspend,
	.do_resume = (void *)_dispatch_queue_resume,
	.do_finalize_activation = _dispatch_source_finalize_activation,
	.do_invoke = _dispatch_source_invoke,
	.do_wakeup = _dispatch_source_wakeup,
	.do_debug = _dispatch_source_debug,
	.do_set_targetq = (void *)_dispatch_queue_set_target_queue,
);

#if HAVE_MACH
DISPATCH_VTABLE_INSTANCE(mach,
	.do_type = DISPATCH_MACH_CHANNEL_TYPE,
	.do_kind = "mach-channel",
	.do_dispose = _dispatch_mach_dispose,
	.do_suspend = (void *)_dispatch_queue_suspend,
	.do_resume = (void *)_dispatch_queue_resume,
	.do_finalize_activation = _dispatch_mach_finalize_activation,
	.do_invoke = _dispatch_mach_invoke,
	.do_wakeup = _dispatch_mach_wakeup,
	.do_debug = _dispatch_mach_debug,
	.do_set_targetq = (void *)_dispatch_queue_set_target_queue,
);

DISPATCH_VTABLE_INSTANCE(mach_msg,
	.do_type = DISPATCH_MACH_MSG_TYPE,
	.do_kind = "mach-msg",
	.do_dispose = _dispatch_mach_msg_dispose,
	.do_invoke = _dispatch_mach_msg_invoke,
	.do_debug = _dispatch_mach_msg_debug,
);
#endif // HAVE_MACH

#if !DISPATCH_DATA_IS_BRIDGED_TO_NSDATA
DISPATCH_VTABLE_INSTANCE(data,
	.do_type = DISPATCH_DATA_TYPE,
	.do_kind = "data",
	.do_dispose = _dispatch_data_dispose,
	.do_debug = _dispatch_data_debug,
);
#endif

DISPATCH_VTABLE_INSTANCE(io,
	.do_type = DISPATCH_IO_TYPE,
	.do_kind = "channel",
	.do_dispose = _dispatch_io_dispose,
	.do_debug = _dispatch_io_debug,
	.do_set_targetq = _dispatch_io_set_target_queue,
);

DISPATCH_VTABLE_INSTANCE(operation,
	.do_type = DISPATCH_OPERATION_TYPE,
	.do_kind = "operation",
	.do_dispose = _dispatch_operation_dispose,
	.do_debug = _dispatch_operation_debug,
);

DISPATCH_VTABLE_INSTANCE(disk,
	.do_type = DISPATCH_DISK_TYPE,
	.do_kind = "disk",
	.do_dispose = _dispatch_disk_dispose,
);


const struct dispatch_continuation_vtable_s _dispatch_continuation_vtables[] = {
	DC_VTABLE_ENTRY(ASYNC_REDIRECT,
		.do_kind = "dc-redirect",
		.do_invoke = _dispatch_async_redirect_invoke),
#if HAVE_MACH
	DC_VTABLE_ENTRY(MACH_SEND_BARRRIER_DRAIN,
		.do_kind = "dc-mach-send-drain",
		.do_invoke = _dispatch_mach_send_barrier_drain_invoke),
	DC_VTABLE_ENTRY(MACH_SEND_BARRIER,
		.do_kind = "dc-mach-send-barrier",
		.do_invoke = _dispatch_mach_barrier_invoke),
	DC_VTABLE_ENTRY(MACH_RECV_BARRIER,
		.do_kind = "dc-mach-recv-barrier",
		.do_invoke = _dispatch_mach_barrier_invoke),
#endif
#if HAVE_PTHREAD_WORKQUEUE_QOS
	DC_VTABLE_ENTRY(OVERRIDE_STEALING,
		.do_kind = "dc-override-stealing",
		.do_invoke = _dispatch_queue_override_invoke),
	DC_VTABLE_ENTRY(OVERRIDE_OWNING,
		.do_kind = "dc-override-owning",
		.do_invoke = _dispatch_queue_override_invoke),
#endif
};

void
_dispatch_vtable_init(void)
{
#if OS_OBJECT_HAVE_OBJC2
	// ObjC classes and dispatch vtables are co-located via linker order and
	// alias files, verify correct layout during initialization rdar://10640168
	dispatch_assert((char*)&DISPATCH_CONCAT(_,DISPATCH_CLASS(semaphore_vtable))
			- (char*)DISPATCH_VTABLE(semaphore) ==
			offsetof(struct dispatch_semaphore_vtable_s, _os_obj_vtable));
#endif // USE_OBJC
}

#pragma mark -
#pragma mark dispatch_bug

static char _dispatch_build[16];

static void
_dispatch_build_init(void *context DISPATCH_UNUSED)
{
#ifdef __APPLE__
	int mib[] = { CTL_KERN, KERN_OSVERSION };
	size_t bufsz = sizeof(_dispatch_build);

	sysctl(mib, 2, _dispatch_build, &bufsz, NULL, 0);
#if TARGET_IPHONE_SIMULATOR
	char *sim_version = getenv("SIMULATOR_RUNTIME_BUILD_VERSION");
	if (sim_version) {
		(void)strlcat(_dispatch_build, " ", sizeof(_dispatch_build));
		(void)strlcat(_dispatch_build, sim_version, sizeof(_dispatch_build));
	}
#endif // TARGET_IPHONE_SIMULATOR

#else
	/*
	 * XXXRW: What to do here for !Mac OS X?
	 */
	memset(_dispatch_build, 0, sizeof(_dispatch_build));
#endif // __APPLE__
}

static dispatch_once_t _dispatch_build_pred;

char*
_dispatch_get_build(void)
{
	dispatch_once_f(&_dispatch_build_pred, NULL, _dispatch_build_init);
	return _dispatch_build;
}

#define _dispatch_bug_log(msg, ...) do { \
	static void *last_seen; \
	void *ra = __builtin_return_address(0); \
	if (last_seen != ra) { \
		last_seen = ra; \
		_dispatch_log(msg, ##__VA_ARGS__); \
	} \
} while(0)

void
_dispatch_bug(size_t line, long val)
{
	dispatch_once_f(&_dispatch_build_pred, NULL, _dispatch_build_init);
	_dispatch_bug_log("BUG in libdispatch: %s - %lu - 0x%lx",
			_dispatch_build, (unsigned long)line, val);
}

void
_dispatch_bug_client(const char* msg)
{
	_dispatch_bug_log("BUG in libdispatch client: %s", msg);
}

#if HAVE_MACH
void
_dispatch_bug_mach_client(const char* msg, mach_msg_return_t kr)
{
	_dispatch_bug_log("BUG in libdispatch client: %s %s - 0x%x", msg,
			mach_error_string(kr), kr);
}
#endif

void
_dispatch_bug_kevent_client(const char* msg, const char* filter,
		const char *operation, int err)
{
	if (operation && err) {
		_dispatch_bug_log("BUG in libdispatch client: %s[%s] %s: \"%s\" - 0x%x",
				msg, filter, operation, strerror(err), err);
	} else if (operation) {
		_dispatch_bug_log("BUG in libdispatch client: %s[%s] %s",
				msg, filter, operation);
	} else {
		_dispatch_bug_log("BUG in libdispatch: %s[%s]: \"%s\" - 0x%x",
				msg, filter, strerror(err), err);
	}
}

void
_dispatch_bug_deprecated(const char *msg)
{
	_dispatch_bug_log("DEPRECATED USE in libdispatch client: %s", msg);
}

void
_dispatch_abort(size_t line, long val)
{
	_dispatch_bug(line, val);
	abort();
}

#if !DISPATCH_USE_OS_DEBUG_LOG

#pragma mark -
#pragma mark dispatch_log

static int dispatch_logfile = -1;
static bool dispatch_log_disabled;
#if DISPATCH_DEBUG
static uint64_t dispatch_log_basetime;
#endif
static dispatch_once_t _dispatch_logv_pred;

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
			dispatch_logfile = STDERR_FILENO;
		}
	}
	if (!dispatch_log_disabled) {
		if (log_to_file && dispatch_logfile == -1) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "/var/tmp/libdispatch.%d.log",
					getpid());
			dispatch_logfile = open(path, O_WRONLY | O_APPEND | O_CREAT |
					O_NOFOLLOW | O_CLOEXEC, 0666);
		}
		if (dispatch_logfile != -1) {
			struct timeval tv;
			gettimeofday(&tv, NULL);
#if DISPATCH_DEBUG
			dispatch_log_basetime = _dispatch_absolute_time();
#endif
			dprintf(dispatch_logfile, "=== log file opened for %s[%u] at "
					"%ld.%06u ===\n", getprogname() ?: "", getpid(),
					tv.tv_sec, (int)tv.tv_usec);
		}
	}
}

static inline void
_dispatch_log_file(char *buf, size_t len)
{
	ssize_t r;

	buf[len++] = '\n';
retry:
	r = write(dispatch_logfile, buf, len);
	if (slowpath(r == -1) && errno == EINTR) {
		goto retry;
	}
}

DISPATCH_NOINLINE
static void
_dispatch_logv_file(const char *msg, va_list ap)
{
	char buf[2048];
	size_t bufsiz = sizeof(buf), offset = 0;
	int r;

#if DISPATCH_DEBUG
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%llu\t",
			_dispatch_absolute_time() - dispatch_log_basetime);
#endif
	r = vsnprintf(&buf[offset], bufsiz - offset, msg, ap);
	if (r < 0) return;
	offset += (size_t)r;
	if (offset > bufsiz - 1) {
		offset = bufsiz - 1;
	}
	_dispatch_log_file(buf, offset);
}

#if DISPATCH_USE_SIMPLE_ASL
static inline void
_dispatch_syslog(const char *msg)
{
	_simple_asl_log(ASL_LEVEL_NOTICE, "com.apple.libsystem.libdispatch", msg);
}

static inline void
_dispatch_vsyslog(const char *msg, va_list ap)
{
	char *str;
    vasprintf(&str, msg, ap);
	if (str) {
		_dispatch_syslog(str);
		free(str);
	}
}
#else // DISPATCH_USE_SIMPLE_ASL
static inline void
_dispatch_syslog(const char *msg)
{
	syslog(LOG_NOTICE, "%s", msg);
}

static inline void
_dispatch_vsyslog(const char *msg, va_list ap)
{
	vsyslog(LOG_NOTICE, msg, ap);
}
#endif // DISPATCH_USE_SIMPLE_ASL

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_logv(const char *msg, size_t len, va_list *ap_ptr)
{
	dispatch_once_f(&_dispatch_logv_pred, NULL, _dispatch_logv_init);
	if (slowpath(dispatch_log_disabled)) {
		return;
	}
	if (slowpath(dispatch_logfile != -1)) {
		if (!ap_ptr) {
			return _dispatch_log_file((char*)msg, len);
		}
		return _dispatch_logv_file(msg, *ap_ptr);
	}
	if (!ap_ptr) {
		return _dispatch_syslog(msg);
	}
	return _dispatch_vsyslog(msg, *ap_ptr);
}

DISPATCH_NOINLINE
void
_dispatch_log(const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	_dispatch_logv(msg, 0, &ap);
	va_end(ap);
}

#endif // DISPATCH_USE_OS_DEBUG_LOG

#pragma mark -
#pragma mark dispatch_debug

static size_t
_dispatch_object_debug2(dispatch_object_t dou, char* buf, size_t bufsiz)
{
	DISPATCH_OBJECT_TFB(_dispatch_objc_debug, dou, buf, bufsiz);
	if (dx_vtable(dou._do)->do_debug) {
		return dx_debug(dou._do, buf, bufsiz);
	}
	return strlcpy(buf, "NULL vtable slot: ", bufsiz);
}

DISPATCH_NOINLINE
static void
_dispatch_debugv(dispatch_object_t dou, const char *msg, va_list ap)
{
	char buf[2048];
	size_t bufsiz = sizeof(buf), offset = 0;
	int r;
#if DISPATCH_DEBUG && !DISPATCH_USE_OS_DEBUG_LOG
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%llu\t\t%p\t",
			_dispatch_absolute_time() - dispatch_log_basetime,
			(void *)_dispatch_thread_self());
#endif
	if (dou._do) {
		offset += _dispatch_object_debug2(dou, &buf[offset], bufsiz - offset);
		dispatch_assert(offset + 2 < bufsiz);
		buf[offset++] = ':';
		buf[offset++] = ' ';
		buf[offset]   = '\0';
	} else {
		offset += strlcpy(&buf[offset], "NULL: ", bufsiz - offset);
	}
	r = vsnprintf(&buf[offset], bufsiz - offset, msg, ap);
#if !DISPATCH_USE_OS_DEBUG_LOG
	size_t len = offset + (r < 0 ? 0 : (size_t)r);
	if (len > bufsiz - 1) {
		len = bufsiz - 1;
	}
	_dispatch_logv(buf, len, NULL);
#else
	_dispatch_log("%s", buf);
#endif
}

DISPATCH_NOINLINE
void
dispatch_debugv(dispatch_object_t dou, const char *msg, va_list ap)
{
	_dispatch_debugv(dou, msg, ap);
}

DISPATCH_NOINLINE
void
dispatch_debug(dispatch_object_t dou, const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	_dispatch_debugv(dou, msg, ap);
	va_end(ap);
}

#if DISPATCH_DEBUG
DISPATCH_NOINLINE
void
_dispatch_object_debug(dispatch_object_t dou, const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	_dispatch_debugv(dou._do, msg, ap);
	va_end(ap);
}
#endif

#pragma mark -
#pragma mark dispatch_calloc

DISPATCH_NOINLINE
void
_dispatch_temporary_resource_shortage(void)
{
	sleep(1);
}

void *
_dispatch_calloc(size_t num_items, size_t size)
{
	void *buf;
	while (!fastpath(buf = calloc(num_items, size))) {
		_dispatch_temporary_resource_shortage();
	}
	return buf;
}

/**
 * If the source string is mutable, allocates memory and copies the contents.
 * Otherwise returns the source string.
 */
const char *
_dispatch_strdup_if_mutable(const char *str)
{
#if HAVE_DYLD_IS_MEMORY_IMMUTABLE
	size_t size = strlen(str) + 1;
	if (slowpath(!_dyld_is_memory_immutable(str, size))) {
		char *clone = (char *) malloc(size);
		if (dispatch_assume(clone)) {
			memcpy(clone, str, size);
		}
		return clone;
	}
	return str;
#else
	return strdup(str);
#endif
}

#pragma mark -
#pragma mark dispatch_block_t

#ifdef __BLOCKS__

void *
(_dispatch_Block_copy)(void *db)
{
	dispatch_block_t rval;

	if (fastpath(db)) {
		while (!fastpath(rval = Block_copy(db))) {
			_dispatch_temporary_resource_shortage();
		}
		return rval;
	}
	DISPATCH_CLIENT_CRASH(0, "NULL was passed where a block should have been");
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

// Abort on uncaught exceptions thrown from client callouts rdar://8577499
#if DISPATCH_USE_CLIENT_CALLOUT && (__USING_SJLJ_EXCEPTIONS__ || !USE_OBJC || \
		OS_OBJECT_HAVE_OBJC1)
// On platforms with SjLj exceptions, avoid the SjLj overhead on every callout
// by clearing the unwinder's TSD pointer to the handler stack around callouts

#define _dispatch_get_tsd_base()
#define _dispatch_get_unwind_tsd() (NULL)
#define _dispatch_set_unwind_tsd(u) do {(void)(u);} while (0)
#define _dispatch_free_unwind_tsd()

#undef _dispatch_client_callout
DISPATCH_NOINLINE
void
_dispatch_client_callout(void *ctxt, dispatch_function_t f)
{
	_dispatch_get_tsd_base();
	void *u = _dispatch_get_unwind_tsd();
	if (fastpath(!u)) return f(ctxt);
	_dispatch_set_unwind_tsd(NULL);
	f(ctxt);
	_dispatch_free_unwind_tsd();
	_dispatch_set_unwind_tsd(u);
}

#undef _dispatch_client_callout2
DISPATCH_NOINLINE
void
_dispatch_client_callout2(void *ctxt, size_t i, void (*f)(void *, size_t))
{
	_dispatch_get_tsd_base();
	void *u = _dispatch_get_unwind_tsd();
	if (fastpath(!u)) return f(ctxt, i);
	_dispatch_set_unwind_tsd(NULL);
	f(ctxt, i);
	_dispatch_free_unwind_tsd();
	_dispatch_set_unwind_tsd(u);
}

#if HAVE_MACH
#undef _dispatch_client_callout4
void
_dispatch_client_callout4(void *ctxt, dispatch_mach_reason_t reason,
		dispatch_mach_msg_t dmsg, mach_error_t error,
		dispatch_mach_handler_function_t f)
{
	_dispatch_get_tsd_base();
	void *u = _dispatch_get_unwind_tsd();
	if (fastpath(!u)) return f(ctxt, reason, dmsg, error);
	_dispatch_set_unwind_tsd(NULL);
	f(ctxt, reason, dmsg, error);
	_dispatch_free_unwind_tsd();
	_dispatch_set_unwind_tsd(u);
}
#endif // HAVE_MACH

#endif // DISPATCH_USE_CLIENT_CALLOUT

#pragma mark -
#pragma mark _os_object_t no_objc

#if !USE_OBJC

static const _os_object_vtable_s _os_object_vtable;

void
_os_object_init(void)
{
	return;
}

inline _os_object_t
_os_object_alloc_realized(const void *cls, size_t size)
{
	_os_object_t obj;
	dispatch_assert(size >= sizeof(struct _os_object_s));
	while (!fastpath(obj = calloc(1u, size))) {
		_dispatch_temporary_resource_shortage();
	}
	obj->os_obj_isa = cls;
	return obj;
}

_os_object_t
_os_object_alloc(const void *cls, size_t size)
{
	if (!cls) cls = &_os_object_vtable;
	return _os_object_alloc_realized(cls, size);
}

void
_os_object_dealloc(_os_object_t obj)
{
	*((void *volatile*)&obj->os_obj_isa) = (void *)0x200;
	return free(obj);
}

void
_os_object_xref_dispose(_os_object_t obj)
{
	_os_object_xrefcnt_dispose_barrier(obj);
	if (fastpath(obj->os_obj_isa->_os_obj_xref_dispose)) {
		return obj->os_obj_isa->_os_obj_xref_dispose(obj);
	}
	return _os_object_release_internal(obj);
}

void
_os_object_dispose(_os_object_t obj)
{
	_os_object_refcnt_dispose_barrier(obj);
	if (fastpath(obj->os_obj_isa->_os_obj_dispose)) {
		return obj->os_obj_isa->_os_obj_dispose(obj);
	}
	return _os_object_dealloc(obj);
}

void*
os_retain(void *obj)
{
	if (fastpath(obj)) {
		return _os_object_retain(obj);
	}
	return obj;
}

#undef os_release
void
os_release(void *obj)
{
	if (fastpath(obj)) {
		return _os_object_release(obj);
	}
}

#pragma mark -
#pragma mark dispatch_autorelease_pool no_objc

#if DISPATCH_COCOA_COMPAT

void*
_dispatch_autorelease_pool_push(void)
{
	void *pool = NULL;
	if (_dispatch_begin_NSAutoReleasePool) {
		pool = _dispatch_begin_NSAutoReleasePool();
	}
	return pool;
}

void
_dispatch_autorelease_pool_pop(void *pool)
{
	if (_dispatch_end_NSAutoReleasePool) {
		_dispatch_end_NSAutoReleasePool(pool);
	}
}

void*
_dispatch_last_resort_autorelease_pool_push(void)
{
	return _dispatch_autorelease_pool_push();
}

void
_dispatch_last_resort_autorelease_pool_pop(void *pool)
{
	_dispatch_autorelease_pool_pop(pool);
}

#endif // DISPATCH_COCOA_COMPAT
#endif // !USE_OBJC

#pragma mark -
#pragma mark dispatch_source_types

static void
dispatch_source_type_timer_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED,
	unsigned long mask,
	dispatch_queue_t q)
{
	if (fastpath(!ds->ds_refs)) {
		ds->ds_refs = _dispatch_calloc(1ul,
				sizeof(struct dispatch_timer_source_refs_s));
	}
	ds->ds_needs_rearm = true;
	ds->ds_is_timer = true;
	if (q == dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_BACKGROUND, 0)
			|| q == dispatch_get_global_queue(
			DISPATCH_QUEUE_PRIORITY_BACKGROUND, DISPATCH_QUEUE_OVERCOMMIT)){
		mask |= DISPATCH_TIMER_BACKGROUND; // <rdar://problem/12200216>
	}
	ds_timer(ds->ds_refs).flags = mask;
}

const struct dispatch_source_type_s _dispatch_source_type_timer = {
	.ke = {
		.filter = DISPATCH_EVFILT_TIMER,
	},
	.mask = DISPATCH_TIMER_STRICT|DISPATCH_TIMER_BACKGROUND|
			DISPATCH_TIMER_WALL_CLOCK,
	.init = dispatch_source_type_timer_init,
};

static void
dispatch_source_type_after_init(dispatch_source_t ds,
	dispatch_source_type_t type, uintptr_t handle, unsigned long mask,
	dispatch_queue_t q)
{
	dispatch_source_type_timer_init(ds, type, handle, mask, q);
	ds->ds_needs_rearm = false;
	ds_timer(ds->ds_refs).flags |= DISPATCH_TIMER_AFTER;
}

const struct dispatch_source_type_s _dispatch_source_type_after = {
	.ke = {
		.filter = DISPATCH_EVFILT_TIMER,
	},
	.init = dispatch_source_type_after_init,
};

static void
dispatch_source_type_timer_with_aggregate_init(dispatch_source_t ds,
	dispatch_source_type_t type, uintptr_t handle, unsigned long mask,
	dispatch_queue_t q)
{
	ds->ds_refs = _dispatch_calloc(1ul,
			sizeof(struct dispatch_timer_source_aggregate_refs_s));
	dispatch_source_type_timer_init(ds, type, handle, mask, q);
	ds_timer(ds->ds_refs).flags |= DISPATCH_TIMER_WITH_AGGREGATE;
	ds->dq_specific_q = (void*)handle;
	_dispatch_retain(ds->dq_specific_q);
}

const struct dispatch_source_type_s _dispatch_source_type_timer_with_aggregate={
	.ke = {
		.filter = DISPATCH_EVFILT_TIMER,
		.ident = ~0ull,
	},
	.mask = DISPATCH_TIMER_STRICT|DISPATCH_TIMER_BACKGROUND,
	.init = dispatch_source_type_timer_with_aggregate_init,
};

static void
dispatch_source_type_interval_init(dispatch_source_t ds,
	dispatch_source_type_t type, uintptr_t handle, unsigned long mask,
	dispatch_queue_t q)
{
	dispatch_source_type_timer_init(ds, type, handle, mask, q);
	ds_timer(ds->ds_refs).flags |= DISPATCH_TIMER_INTERVAL;
	unsigned long ident = _dispatch_source_timer_idx(ds->ds_refs);
	ds->ds_dkev->dk_kevent.ident = ds->ds_ident_hack = ident;
	_dispatch_source_set_interval(ds, handle);
}

const struct dispatch_source_type_s _dispatch_source_type_interval = {
	.ke = {
		.filter = DISPATCH_EVFILT_TIMER,
		.ident = ~0ull,
	},
	.mask = DISPATCH_TIMER_STRICT|DISPATCH_TIMER_BACKGROUND|
			DISPATCH_INTERVAL_UI_ANIMATION,
	.init = dispatch_source_type_interval_init,
};

static void
dispatch_source_type_readwrite_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED,
	unsigned long mask DISPATCH_UNUSED,
	dispatch_queue_t q DISPATCH_UNUSED)
{
	ds->ds_is_level = true;
#ifdef HAVE_DECL_NOTE_LOWAT
	// bypass kernel check for device kqueue support rdar://19004921
	ds->ds_dkev->dk_kevent.fflags = NOTE_LOWAT;
#endif
	ds->ds_dkev->dk_kevent.data = 1;
}

const struct dispatch_source_type_s _dispatch_source_type_read = {
	.ke = {
		.filter = EVFILT_READ,
		.flags = EV_VANISHED|EV_DISPATCH|EV_UDATA_SPECIFIC,
	},
	.init = dispatch_source_type_readwrite_init,
};

const struct dispatch_source_type_s _dispatch_source_type_write = {
	.ke = {
		.filter = EVFILT_WRITE,
		.flags = EV_VANISHED|EV_DISPATCH|EV_UDATA_SPECIFIC,
	},
	.init = dispatch_source_type_readwrite_init,
};

#if DISPATCH_USE_MEMORYSTATUS

#if TARGET_IPHONE_SIMULATOR // rdar://problem/9219483
static int _dispatch_ios_simulator_memory_warnings_fd = -1;
static void
_dispatch_ios_simulator_memorypressure_init(void *context DISPATCH_UNUSED)
{
	char *e = getenv("SIMULATOR_MEMORY_WARNINGS");
	if (!e) return;
	_dispatch_ios_simulator_memory_warnings_fd = open(e, O_EVTONLY);
	if (_dispatch_ios_simulator_memory_warnings_fd == -1) {
		(void)dispatch_assume_zero(errno);
	}
}
#endif

#if TARGET_IPHONE_SIMULATOR
static void
dispatch_source_type_memorypressure_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED,
	unsigned long mask DISPATCH_UNUSED,
	dispatch_queue_t q DISPATCH_UNUSED)
{
	static dispatch_once_t pred;
	dispatch_once_f(&pred, NULL, _dispatch_ios_simulator_memorypressure_init);
	handle = (uintptr_t)_dispatch_ios_simulator_memory_warnings_fd;
	mask = NOTE_ATTRIB;
	ds->ds_dkev->dk_kevent.filter = EVFILT_VNODE;
	ds->ds_dkev->dk_kevent.ident = handle;
	ds->ds_dkev->dk_kevent.flags |= EV_CLEAR;
	ds->ds_dkev->dk_kevent.fflags = (uint32_t)mask;
	ds->ds_ident_hack = handle;
	ds->ds_pending_data_mask = mask;
	ds->ds_memorypressure_override = 1;
}
#else
#define dispatch_source_type_memorypressure_init NULL
#endif

#ifndef NOTE_MEMORYSTATUS_LOW_SWAP
#define NOTE_MEMORYSTATUS_LOW_SWAP 0x8
#endif

const struct dispatch_source_type_s _dispatch_source_type_memorypressure = {
	.ke = {
		.filter = EVFILT_MEMORYSTATUS,
		.flags = EV_DISPATCH|EV_UDATA_SPECIFIC,
	},
	.mask = NOTE_MEMORYSTATUS_PRESSURE_NORMAL|NOTE_MEMORYSTATUS_PRESSURE_WARN
			|NOTE_MEMORYSTATUS_PRESSURE_CRITICAL|NOTE_MEMORYSTATUS_LOW_SWAP
			|NOTE_MEMORYSTATUS_PROC_LIMIT_WARN|NOTE_MEMORYSTATUS_PROC_LIMIT_CRITICAL,
	.init = dispatch_source_type_memorypressure_init,
};

static void
dispatch_source_type_vm_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED,
	unsigned long mask DISPATCH_UNUSED,
	dispatch_queue_t q DISPATCH_UNUSED)
{
	// Map legacy vm pressure to memorypressure warning rdar://problem/15907505
	mask = NOTE_MEMORYSTATUS_PRESSURE_WARN;
	ds->ds_dkev->dk_kevent.fflags = (uint32_t)mask;
	ds->ds_pending_data_mask = mask;
	ds->ds_vmpressure_override = 1;
#if TARGET_IPHONE_SIMULATOR
	dispatch_source_type_memorypressure_init(ds, type, handle, mask, q);
#endif
}

const struct dispatch_source_type_s _dispatch_source_type_vm = {
	.ke = {
		.filter = EVFILT_MEMORYSTATUS,
		.flags = EV_DISPATCH|EV_UDATA_SPECIFIC,
	},
	.mask = NOTE_VM_PRESSURE,
	.init = dispatch_source_type_vm_init,
};

#elif DISPATCH_USE_VM_PRESSURE

const struct dispatch_source_type_s _dispatch_source_type_vm = {
	.ke = {
		.filter = EVFILT_VM,
		.flags = EV_DISPATCH|EV_UDATA_SPECIFIC,
	},
	.mask = NOTE_VM_PRESSURE,
};

#endif // DISPATCH_USE_VM_PRESSURE

const struct dispatch_source_type_s _dispatch_source_type_signal = {
	.ke = {
		.filter = EVFILT_SIGNAL,
		.flags = EV_UDATA_SPECIFIC,
	},
};

#if !defined(__linux__)
static void
dispatch_source_type_proc_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED,
	unsigned long mask DISPATCH_UNUSED,
	dispatch_queue_t q DISPATCH_UNUSED)
{
	ds->ds_dkev->dk_kevent.fflags |= NOTE_EXIT; // rdar://16655831
}

const struct dispatch_source_type_s _dispatch_source_type_proc = {
	.ke = {
		.filter = EVFILT_PROC,
		.flags = EV_CLEAR|EV_UDATA_SPECIFIC,
	},
	.mask = NOTE_EXIT|NOTE_FORK|NOTE_EXEC
#if HAVE_DECL_NOTE_SIGNAL
			|NOTE_SIGNAL
#endif
#if HAVE_DECL_NOTE_REAP
			|NOTE_REAP
#endif
			,
	.init = dispatch_source_type_proc_init,
};

const struct dispatch_source_type_s _dispatch_source_type_vnode = {
	.ke = {
		.filter = EVFILT_VNODE,
		.flags = EV_CLEAR|EV_VANISHED|EV_UDATA_SPECIFIC,
	},
	.mask = NOTE_DELETE|NOTE_WRITE|NOTE_EXTEND|NOTE_ATTRIB|NOTE_LINK|
			NOTE_RENAME|NOTE_FUNLOCK
#if HAVE_DECL_NOTE_REVOKE
			|NOTE_REVOKE
#endif
#if HAVE_DECL_NOTE_NONE
			|NOTE_NONE
#endif
			,
};

const struct dispatch_source_type_s _dispatch_source_type_vfs = {
	.ke = {
		.filter = EVFILT_FS,
		.flags = EV_CLEAR|EV_UDATA_SPECIFIC,
	},
	.mask = VQ_NOTRESP|VQ_NEEDAUTH|VQ_LOWDISK|VQ_MOUNT|VQ_UNMOUNT|VQ_DEAD|
			VQ_ASSIST|VQ_NOTRESPLOCK
#if HAVE_DECL_VQ_UPDATE
			|VQ_UPDATE
#endif
#if HAVE_DECL_VQ_VERYLOWDISK
			|VQ_VERYLOWDISK
#endif
#if HAVE_DECL_VQ_QUOTA
			|VQ_QUOTA
#endif
			,
};

const struct dispatch_source_type_s _dispatch_source_type_sock = {
#ifdef EVFILT_SOCK
	.ke = {
		.filter = EVFILT_SOCK,
		.flags = EV_CLEAR|EV_VANISHED|EV_UDATA_SPECIFIC,
	},
	.mask = NOTE_CONNRESET |  NOTE_READCLOSED | NOTE_WRITECLOSED |
		NOTE_TIMEOUT | NOTE_NOSRCADDR |  NOTE_IFDENIED | NOTE_SUSPEND |
		NOTE_RESUME | NOTE_KEEPALIVE
#ifdef NOTE_ADAPTIVE_WTIMO
		| NOTE_ADAPTIVE_WTIMO | NOTE_ADAPTIVE_RTIMO
#endif
#ifdef NOTE_CONNECTED
		| NOTE_CONNECTED | NOTE_DISCONNECTED | NOTE_CONNINFO_UPDATED
#endif
#ifdef NOTE_NOTIFY_ACK
		| NOTE_NOTIFY_ACK
#endif
		,
#endif // EVFILT_SOCK
};
#endif // !defined(__linux__)

static void
dispatch_source_type_data_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED,
	unsigned long mask DISPATCH_UNUSED,
	dispatch_queue_t q DISPATCH_UNUSED)
{
	ds->ds_is_installed = true;
	ds->ds_is_custom_source = true;
	ds->ds_is_direct_kevent = true;
	ds->ds_pending_data_mask = ~0ul;
	ds->ds_needs_rearm = false; // not registered with kevent
}

const struct dispatch_source_type_s _dispatch_source_type_data_add = {
	.ke = {
		.filter = DISPATCH_EVFILT_CUSTOM_ADD,
		.flags = EV_UDATA_SPECIFIC,
	},
	.init = dispatch_source_type_data_init,
};

const struct dispatch_source_type_s _dispatch_source_type_data_or = {
	.ke = {
		.filter = DISPATCH_EVFILT_CUSTOM_OR,
		.flags = EV_CLEAR|EV_UDATA_SPECIFIC,
		.fflags = ~0u,
	},
	.init = dispatch_source_type_data_init,
};

#if HAVE_MACH

static void
dispatch_source_type_mach_send_init(dispatch_source_t ds,
	dispatch_source_type_t type DISPATCH_UNUSED,
	uintptr_t handle DISPATCH_UNUSED, unsigned long mask,
	dispatch_queue_t q DISPATCH_UNUSED)
{
	if (!mask) {
		// Preserve legacy behavior that (mask == 0) => DISPATCH_MACH_SEND_DEAD
		ds->ds_dkev->dk_kevent.fflags = DISPATCH_MACH_SEND_DEAD;
		ds->ds_pending_data_mask = DISPATCH_MACH_SEND_DEAD;
	}
}

const struct dispatch_source_type_s _dispatch_source_type_mach_send = {
	.ke = {
		.filter = DISPATCH_EVFILT_MACH_NOTIFICATION,
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
	ds->ds_pending_data_mask = DISPATCH_MACH_RECV_MESSAGE;
#if DISPATCH_EVFILT_MACHPORT_PORTSET_FALLBACK
	if (_dispatch_evfilt_machport_direct_enabled) return;
	ds->ds_dkev->dk_kevent.fflags = DISPATCH_MACH_RECV_MESSAGE;
	ds->ds_dkev->dk_kevent.flags &= ~(EV_UDATA_SPECIFIC|EV_VANISHED);
	ds->ds_is_direct_kevent = false;
#endif
}

const struct dispatch_source_type_s _dispatch_source_type_mach_recv = {
	.ke = {
		.filter = EVFILT_MACHPORT,
		.flags = EV_VANISHED|EV_DISPATCH|EV_UDATA_SPECIFIC,
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
_dispatch_wakeup_runloop_thread(mach_port_t mp DISPATCH_UNUSED)
{
	// dummy function just to pop a runloop thread out of mach_msg()
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
