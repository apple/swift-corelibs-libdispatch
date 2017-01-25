/*
 * Copyright (c) 2017-2017 Apple Inc. All rights reserved.
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

#if DISPATCH_USE_INTERNAL_WORKQUEUE

// forward looking typedef; not yet defined in dispatch
typedef pid_t dispatch_tid;

/*
 * dispatch_workq monitors the thread pool that is
 * executing the work enqueued on libdispatch's pthread
 * root queues and dynamically adjusts its size.
 *
 * The dynamic monitoring could be implemented using either
 *   (a) low-frequency user-level approximation of the number of runnable
 *       worker threads via reading the /proc file system
 *   (b) a Linux kernel extension that hooks the process change handler
 *       to accurately track the number of runnable normal worker threads
 * This file provides an implementation of option (a).
 *
 * Using either form of monitoring, if (i) there appears to be
 * work available in the monitored pthread root queue, (ii) the
 * number of runnable workers is below the target size for the pool,
 * and (iii) the total number of worker threads is below an upper limit,
 * then an additional worker thread will be added to the pool.
 */

#pragma mark static data for monitoring subsystem

/*
 * State for the user-level monitoring of a workqueue.
 */
typedef struct dispatch_workq_monitor_s {
	/* The dispatch_queue we are monitoring */
	dispatch_queue_t dq;

	/* The observed number of runnable worker threads */
	int32_t num_runnable;

	/* The desired number of runnable worker threads */
	int32_t target_runnable;

	/*
	 * Tracking of registered workers; all accesses must hold lock.
	 * Invariant: registered_tids[0]...registered_tids[num_registered_tids-1]
	 *   contain the dispatch_tids of the worker threads we are monitoring.
	 */
	dispatch_unfair_lock_s registered_tid_lock;
	dispatch_tid *registered_tids;
	int num_registered_tids;
} dispatch_workq_monitor_s, *dispatch_workq_monitor_t;

static dispatch_workq_monitor_s _dispatch_workq_monitors[WORKQ_NUM_PRIORITIES];

#pragma mark Implementation of the monitoring subsystem.

#define WORKQ_MAX_TRACKED_TIDS DISPATCH_WORKQ_MAX_PTHREAD_COUNT
#define WORKQ_OVERSUBSCRIBE_FACTOR 2

static void _dispatch_workq_init_once(void *context DISPATCH_UNUSED);
static dispatch_once_t _dispatch_workq_init_once_pred;

void
_dispatch_workq_worker_register(dispatch_queue_t root_q, int priority)
{
	dispatch_once_f(&_dispatch_workq_init_once_pred, NULL, &_dispatch_workq_init_once);

#if HAVE_DISPATCH_WORKQ_MONITORING
	dispatch_workq_monitor_t mon = &_dispatch_workq_monitors[priority];
	dispatch_assert(mon->dq == root_q);
	dispatch_tid tid = _dispatch_thread_getspecific(tid);
	_dispatch_unfair_lock_lock(&mon->registered_tid_lock);
	dispatch_assert(mon->num_registered_tids < WORKQ_MAX_TRACKED_TIDS-1);
	int worker_id = mon->num_registered_tids++;
	mon->registered_tids[worker_id] = tid;
	_dispatch_unfair_lock_unlock(&mon->registered_tid_lock);
#endif // HAVE_DISPATCH_WORKQ_MONITORING
}

void
_dispatch_workq_worker_unregister(dispatch_queue_t root_q, int priority)
{
#if HAVE_DISPATCH_WORKQ_MONITORING
	dispatch_workq_monitor_t mon = &_dispatch_workq_monitors[priority];
	dispatch_tid tid = _dispatch_thread_getspecific(tid);
	_dispatch_unfair_lock_lock(&mon->registered_tid_lock);
	for (int i = 0; i < mon->num_registered_tids; i++) {
		if (mon->registered_tids[i] == tid) {
			int last = mon->num_registered_tids - 1;
			mon->registered_tids[i] = mon->registered_tids[last];
			mon->registered_tids[last] = 0;
			mon->num_registered_tids--;
			break;
		}
	}
	_dispatch_unfair_lock_unlock(&mon->registered_tid_lock);
#endif // HAVE_DISPATCH_WORKQ_MONITORING
}


#if HAVE_DISPATCH_WORKQ_MONITORING
#if defined(__linux__)
/*
 * For each pid that is a registered worker, read /proc/[pid]/stat
 * to get a count of the number of them that are actually runnable.
 * See the proc(5) man page for the format of the contents of /proc/[pid]/stat
 */
static void
_dispatch_workq_count_runnable_workers(dispatch_workq_monitor_t mon)
{
	char path[128];
	char buf[4096];
	int running_count = 0;

	_dispatch_unfair_lock_lock(&mon->registered_tid_lock);

	for (int i = 0; i < mon->num_registered_tids; i++) {
		dispatch_tid tid = mon->registered_tids[i];
		int fd;
		size_t bytes_read = -1;

		int r = snprintf(path, sizeof(path), "/proc/%d/stat", tid);
		dispatch_assert(r > 0 && r < sizeof(path));

		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (unlikely(fd == -1)) {
			DISPATCH_CLIENT_CRASH(tid,
					"workq: registered worker exited prematurely");
		} else {
			bytes_read = read(fd, buf, sizeof(buf)-1);
			(void)close(fd);
		}

		if (bytes_read > 0) {
			buf[bytes_read] = '\0';
			char state;
			if (sscanf(buf, "%*d %*s %c", &state) == 1) {
				// _dispatch_debug("workq: Worker %d, state %c\n", tid, state);
				if (state == 'R') {
					running_count++;
				}
			} else {
				_dispatch_debug("workq: sscanf of state failed for %d", tid);
			}
		} else {
			_dispatch_debug("workq: Failed to read %s", path);
		}
	}

	mon->num_runnable = running_count;

	_dispatch_unfair_lock_unlock(&mon->registered_tid_lock);
}
#else
#error must define _dispatch_workq_count_runnable_workers
#endif

static void
_dispatch_workq_monitor_pools(void *context DISPATCH_UNUSED)
{
	// TODO: Once we switch away from the legacy priorities to
	//       newer QoS, we can loop in order of decreasing QoS
	//       and track the total number of runnable threads seen
	//       across pools.  We can then use that number to
	//       implement a global policy where low QoS queues
	//       are not eligible for over-subscription if the higher
	//       QoS queues have already consumed the target
	//       number of threads.
	for (int i = 0; i < WORKQ_NUM_PRIORITIES; i++) {
		dispatch_workq_monitor_t mon = &_dispatch_workq_monitors[i];
		dispatch_queue_t dq = mon->dq;

		if (!_dispatch_queue_class_probe(dq)) {
			_dispatch_debug("workq: %s is empty.", dq->dq_label);
			continue;
		}

		_dispatch_workq_count_runnable_workers(mon);
		_dispatch_debug("workq: %s has %d runnable wokers (target is %d)",
				dq->dq_label, mon->num_runnable, mon->target_runnable);

		if (mon->num_runnable == 0) {
			// We are below target, and no worker is runnable.
			// It is likely the program is stalled. Therefore treat
			// this as if dq were an overcommit queue and call poke
			// with the limit being the maximum number of workers for dq.
			int32_t floor = mon->target_runnable - WORKQ_MAX_TRACKED_TIDS;
			_dispatch_debug("workq: %s has no runnable workers; poking with floor %d",
					dq->dq_label, floor);
			_dispatch_global_queue_poke(dq, 1, floor);
		} else if (mon->num_runnable < mon->target_runnable) {
			// We are below target, but some workers are still runnable.
			// We want to oversubscribe to hit the desired load target.
			// However, this under-utilization may be transitory so set the
			// floor as a small multiple of threads per core.
			int32_t floor = (1 - WORKQ_OVERSUBSCRIBE_FACTOR) * mon->target_runnable;
			int32_t floor2 = mon->target_runnable - WORKQ_MAX_TRACKED_TIDS;
			floor = MAX(floor, floor2);
			_dispatch_debug("workq: %s under utilization target; poking with floor %d",
					dq->dq_label, floor);
			_dispatch_global_queue_poke(dq, 1, floor);
		}
	}
}
#endif // HAVE_DISPATCH_WORKQ_MONITORING


// temporary until we switch over to QoS based interface.
static dispatch_queue_t
get_root_queue_from_legacy_priority(int priority)
{
	switch (priority) {
	case WORKQ_HIGH_PRIOQUEUE:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_USER_INITIATED_QOS];
	case WORKQ_DEFAULT_PRIOQUEUE:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_DEFAULT_QOS];
	case WORKQ_LOW_PRIOQUEUE:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_UTILITY_QOS];
	case WORKQ_BG_PRIOQUEUE:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_MAINTENANCE_QOS];
	case WORKQ_BG_PRIOQUEUE_CONDITIONAL:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_BACKGROUND_QOS];
	case WORKQ_HIGH_PRIOQUEUE_CONDITIONAL:
		return &_dispatch_root_queues[DISPATCH_ROOT_QUEUE_IDX_USER_INTERACTIVE_QOS];
	default:
		return NULL;
	}
}

static void
_dispatch_workq_init_once(void *context DISPATCH_UNUSED)
{
#if HAVE_DISPATCH_WORKQ_MONITORING
	int target_runnable = dispatch_hw_config(active_cpus);
	for (int i = 0; i < WORKQ_NUM_PRIORITIES; i++) {
		dispatch_workq_monitor_t mon = &_dispatch_workq_monitors[i];
		mon->dq = get_root_queue_from_legacy_priority(i);
		void *buf = _dispatch_calloc(WORKQ_MAX_TRACKED_TIDS, sizeof(dispatch_tid));
		mon->registered_tids = buf;
		mon->target_runnable = target_runnable;
	}

	// Create monitoring timer that will periodically run on dispatch_mgr_q
	dispatch_source_t ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,
			0, 0, &_dispatch_mgr_q);
	dispatch_source_set_timer(ds, dispatch_time(DISPATCH_TIME_NOW, 0),
			NSEC_PER_SEC, 0);
	dispatch_source_set_event_handler_f(ds, _dispatch_workq_monitor_pools);
	dispatch_set_context(ds, ds); // avoid appearing as leaked
	dispatch_activate(ds);
#endif // HAVE_DISPATCH_WORKQ_MONITORING
}

#endif // DISPATCH_USE_INTERNAL_WORKQUEUE
