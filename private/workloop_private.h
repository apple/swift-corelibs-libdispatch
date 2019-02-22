/*
 * Copyright (c) 2017-2018 Apple Inc. All rights reserved.
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

#ifndef __DISPATCH_WORKLOOP_PRIVATE__
#define __DISPATCH_WORKLOOP_PRIVATE__

#ifndef __DISPATCH_INDIRECT__
#error "Please #include <dispatch/private.h> instead of this file directly."
#include <dispatch/base.h> // for HeaderDoc
#endif

/******************************************************************************\
 *
 * THIS FILE IS AN IN-PROGRESS INTERFACE THAT IS SUBJECT TO CHANGE
 *
\******************************************************************************/

DISPATCH_ASSUME_NONNULL_BEGIN

__BEGIN_DECLS

/*!
 * @typedef dispatch_workloop_t
 *
 * @abstract
 * Dispatch workloops invoke workitems submitted to them in priority order.
 *
 * @discussion
 * A dispatch workloop is a flavor of dispatch_queue_t that is a priority
 * ordered queue (using the QOS class of the submitted workitems as the
 * ordering).
 *
 * Between each workitem invocation, the workloop will evaluate whether higher
 * priority workitems have since been submitted and execute these first.
 *
 * Serial queues targeting a workloop maintain FIFO execution of their
 * workitems. However, the workloop may reorder workitems submitted to
 * independent serial queues targeting it with respect to each other,
 * based on their priorities.
 *
 * A dispatch workloop is a "subclass" of dispatch_queue_t which can be passed
 * to all APIs accepting a dispatch queue, except for functions from the
 * dispatch_sync() family. dispatch_async_and_wait() must be used for workloop
 * objects. Functions from the dispatch_sync() family on queues targeting
 * a workloop are still permitted but discouraged for performance reasons.
 */
#if defined(__DISPATCH_BUILDING_DISPATCH__) && !defined(__OBJC__)
typedef struct dispatch_workloop_s *dispatch_workloop_t;
#else
DISPATCH_DECL_SUBCLASS(dispatch_workloop, dispatch_queue);
#endif

/*!
 * @function dispatch_workloop_create
 *
 * @abstract
 * Creates a new dispatch workloop to which workitems may be submitted.
 *
 * @param label
 * A string label to attach to the workloop.
 *
 * @result
 * The newly created dispatch workloop.
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_MALLOC DISPATCH_RETURNS_RETAINED DISPATCH_WARN_RESULT
DISPATCH_NOTHROW
dispatch_workloop_t
dispatch_workloop_create(const char *_Nullable label);

/*!
 * @function dispatch_workloop_create_inactive
 *
 * @abstract
 * Creates a new inactive dispatch workloop that can be setup and then
 * activated.
 *
 * @discussion
 * Creating an inactive workloop allows for it to receive further configuration
 * before it is activated, and workitems can be submitted to it.
 *
 * Submitting workitems to an inactive workloop is undefined and will cause the
 * process to be terminated.
 *
 * @param label
 * A string label to attach to the workloop.
 *
 * @result
 * The newly created dispatch workloop.
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_MALLOC DISPATCH_RETURNS_RETAINED DISPATCH_WARN_RESULT
DISPATCH_NOTHROW
dispatch_workloop_t
dispatch_workloop_create_inactive(const char *_Nullable label);

/*!
 * @function dispatch_workloop_set_autorelease_frequency
 *
 * @abstract
 * Sets the autorelease frequency of the workloop.
 *
 * @discussion
 * See dispatch_queue_attr_make_with_autorelease_frequency().
 * The default policy for a workloop is
 * DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM.
 *
 * @param workloop
 * The dispatch workloop to modify.
 *
 * This workloop must be inactive, passing an activated object is undefined
 * and will cause the process to be terminated.
 *
 * @param frequency
 * The requested autorelease frequency.
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_NONNULL_ALL DISPATCH_NOTHROW
void
dispatch_workloop_set_autorelease_frequency(dispatch_workloop_t workloop,
		dispatch_autorelease_frequency_t frequency);

DISPATCH_ENUM(dispatch_workloop_param_flags, uint64_t,
	DISPATCH_WORKLOOP_NONE DISPATCH_ENUM_API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0)) = 0x0,
	DISPATCH_WORKLOOP_FIXED_PRIORITY DISPATCH_ENUM_API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0)) = 0x1,
);

/*!
 * @function dispatch_workloop_set_qos_class_floor
 *
 * @abstract
 * Sets the QOS class floor of a workloop.
 *
 * @discussion
 * See dispatch_set_qos_class_floor().
 *
 * This function is strictly equivalent to dispatch_set_qos_class_floor() but
 * allows to pass extra flags.
 *
 * Using both dispatch_workloop_set_scheduler_priority() and
 * dispatch_set_qos_class_floor() or dispatch_workloop_set_qos_class_floor()
 * is undefined and will cause the process to be terminated.
 *
 * @param workloop
 * The dispatch workloop to modify.
 *
 * This workloop must be inactive, passing an activated object is undefined
 * and will cause the process to be terminated.
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_NONNULL_ALL DISPATCH_NOTHROW
void
dispatch_workloop_set_qos_class_floor(dispatch_workloop_t workloop,
		dispatch_qos_class_t qos, int relpri, dispatch_workloop_param_flags_t flags);

/*!
 * @function dispatch_workloop_set_scheduler_priority
 *
 * @abstract
 * Sets the scheduler priority for a dispatch workloop.
 *
 * @discussion
 * This sets the scheduler priority of the threads that the runtime will bring
 * up to service this workloop.
 *
 * QOS propagation still functions on these workloops, but its effect on the
 * priority of the thread brought up to service this workloop is ignored.
 *
 * Using both dispatch_workloop_set_scheduler_priority() and
 * dispatch_set_qos_class_floor() or dispatch_workloop_set_qos_class_floor()
 * is undefined and will cause the process to be terminated.
 *
 * @param workloop
 * The dispatch workloop to modify.
 *
 * This workloop must be inactive, passing an activated object is undefined
 * and will cause the process to be terminated.
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_NONNULL_ALL DISPATCH_NOTHROW
void
dispatch_workloop_set_scheduler_priority(dispatch_workloop_t workloop,
		int priority, dispatch_workloop_param_flags_t flags);

/*!
 * @function dispatch_workloop_set_cpupercent
 *
 * @abstract
 * Sets the cpu percent and refill attributes for a dispatch workloop.
 *
 * @discussion
 * This should only used if the workloop was also setup with the
 * DISPATCH_WORKLOOP_FIXED_PRIORITY flag as a safe guard against
 * busy loops that could starve the rest of the system forever.
 *
 * If DISPATCH_WORKLOOP_FIXED_PRIORITY wasn't passed, using this function is
 * undefined and will cause the process to be terminated.
 *
 * @param workloop
 * The dispatch workloop to modify.
 *
 * This workloop must be inactive, passing an activated object is undefined
 * and will cause the process to be terminated.
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_NONNULL_ALL DISPATCH_NOTHROW
void
dispatch_workloop_set_cpupercent(dispatch_workloop_t workloop, uint8_t percent,
		uint32_t refillms);

/*!
 * @function dispatch_workloop_is_current()
 *
 * @abstract
 * Returns whether the current thread has been made by the runtime to service
 * this workloop.
 *
 * @discussion
 * Note that when using <code>dispatch_async_and_wait(workloop, ^{ ... })</code>
 * then <code>workloop</code> will be seen as the "current" one by the submitted
 * workitem, but that is not the case when using dispatch_sync() on a queue
 * targeting the workloop.
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_NONNULL_ALL DISPATCH_NOTHROW
bool
dispatch_workloop_is_current(dispatch_workloop_t workloop);

/*!
 * @function dispatch_workloop_copy_current()
 *
 * @abstract
 * Returns a copy of the workoop that is being serviced on the calling thread
 * if any.
 *
 * @discussion
 * If the thread is not a workqueue thread, or is not servicing a dispatch
 * workloop, then NULL is returned.
 *
 * This returns a retained object that must be released with dispatch_release().
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_RETURNS_RETAINED DISPATCH_NOTHROW
dispatch_workloop_t _Nullable
dispatch_workloop_copy_current(void);

// Equivalent to dispatch_workloop_set_qos_class_floor(workoop, qos, 0, flags)
API_DEPRECATED_WITH_REPLACEMENT("dispatch_workloop_set_qos_class_floor",
		macos(10.14,10.14), ios(12.0,12.0), tvos(12.0,12.0), watchos(5.0,5.0))
DISPATCH_EXPORT DISPATCH_NONNULL_ALL DISPATCH_NOTHROW
void
dispatch_workloop_set_qos_class(dispatch_workloop_t workloop,
		dispatch_qos_class_t qos, dispatch_workloop_param_flags_t flags);

API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_NOTHROW
bool
_dispatch_workloop_should_yield_4NW(void);

/*!
 * @function dispatch_async_and_wait
 *
 * @abstract
 * Submits a block for synchronous execution on a dispatch queue.
 *
 * @discussion
 * Submits a workitem to a dispatch queue like dispatch_async(), however
 * dispatch_async_and_wait() will not return until the workitem has finished.
 *
 * Like functions of the dispatch_sync family, dispatch_async_and_wait() is
 * subject to dead-lock (See dispatch_sync() for details).
 *
 * However, dispatch_async_and_wait() differs from functions of the
 * dispatch_sync family in two fundamental ways: how it respects queue
 * attributes and how it chooses the execution context invoking the workitem.
 *
 * <b>Differences with dispatch_sync()</b>
 *
 * Work items submitted to a queue with dispatch_async_and_wait() observe all
 * queue attributes of that queue when invoked (inluding autorelease frequency
 * or QOS class).
 *
 * When the runtime has brought up a thread to invoke the asynchronous workitems
 * already submitted to the specified queue, that servicing thread will also be
 * used to execute synchronous work submitted to the queue with
 * dispatch_async_and_wait().
 *
 * However, if the runtime has not brought up a thread to service the specified
 * queue (because it has no workitems enqueued, or only synchronous workitems),
 * then dispatch_async_and_wait() will invoke the workitem on the calling thread,
 * similar to the behaviour of functions in the dispatch_sync family.
 *
 * As an exception, if the queue the work is submitted to doesn't target
 * a global concurrent queue (for example because it targets the main queue),
 * then the workitem will never be invoked by the thread calling
 * dispatch_async_and_wait().
 *
 * In other words, dispatch_async_and_wait() is similar to submitting
 * a dispatch_block_create()d workitem to a queue and then waiting on it, as
 * shown in the code example below. However, dispatch_async_and_wait() is
 * significantly more efficient when a new thread is not required to execute
 * the workitem (as it will use the stack of the submitting thread instead of
 * requiring heap allocations).
 *
 * <code>
 *     dispatch_block_t b = dispatch_block_create(0, block);
 *     dispatch_async(queue, b);
 *     dispatch_block_wait(b, DISPATCH_TIME_FOREVER);
 *     Block_release(b);
 * </code>
 *
 * @param queue
 * The target dispatch queue to which the block is submitted.
 * The result of passing NULL in this parameter is undefined.
 *
 * @param block
 * The block to be invoked on the target dispatch queue.
 * The result of passing NULL in this parameter is undefined.
 */
#ifdef __BLOCKS__
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_NONNULL_ALL DISPATCH_NOTHROW
void
dispatch_async_and_wait(dispatch_queue_t queue,
		DISPATCH_NOESCAPE dispatch_block_t block);
#endif

/*!
 * @function dispatch_async_and_wait_f
 *
 * @abstract
 * Submits a function for synchronous execution on a dispatch queue.
 *
 * @discussion
 * See dispatch_async_and_wait() for details.
 *
 * @param queue
 * The target dispatch queue to which the function is submitted.
 * The result of passing NULL in this parameter is undefined.
 *
 * @param context
 * The application-defined context parameter to pass to the function.
 *
 * @param work
 * The application-defined function to invoke on the target queue. The first
 * parameter passed to this function is the context provided to
 * dispatch_async_and_wait_f().
 * The result of passing NULL in this parameter is undefined.
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_NONNULL1 DISPATCH_NONNULL3 DISPATCH_NOTHROW
void
dispatch_async_and_wait_f(dispatch_queue_t queue,
		void *_Nullable context, dispatch_function_t work);

/*!
 * @function dispatch_barrier_async_and_wait
 *
 * @abstract
 * Submits a block for synchronous execution on a dispatch queue.
 *
 * @discussion
 * Submits a block to a dispatch queue like dispatch_async_and_wait(), but marks
 * that block as a barrier (relevant only on DISPATCH_QUEUE_CONCURRENT
 * queues).
 *
 * @param queue
 * The target dispatch queue to which the block is submitted.
 * The result of passing NULL in this parameter is undefined.
 *
 * @param work
 * The application-defined block to invoke on the target queue.
 * The result of passing NULL in this parameter is undefined.
 */
#ifdef __BLOCKS__
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_NONNULL_ALL DISPATCH_NOTHROW
void
dispatch_barrier_async_and_wait(dispatch_queue_t queue,
		DISPATCH_NOESCAPE dispatch_block_t block);
#endif

/*!
 * @function dispatch_barrier_async_and_wait_f
 *
 * @abstract
 * Submits a function for synchronous execution on a dispatch queue.
 *
 * @discussion
 * Submits a function to a dispatch queue like dispatch_async_and_wait_f(), but
 * marks that function as a barrier (relevant only on DISPATCH_QUEUE_CONCURRENT
 * queues).
 *
 * @param queue
 * The target dispatch queue to which the function is submitted.
 * The result of passing NULL in this parameter is undefined.
 *
 * @param context
 * The application-defined context parameter to pass to the function.
 *
 * @param work
 * The application-defined function to invoke on the target queue. The first
 * parameter passed to this function is the context provided to
 * dispatch_barrier_async_and_wait_f().
 * The result of passing NULL in this parameter is undefined.
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
DISPATCH_EXPORT DISPATCH_NONNULL1 DISPATCH_NONNULL3 DISPATCH_NOTHROW
void
dispatch_barrier_async_and_wait_f(dispatch_queue_t queue,
		void *_Nullable context, dispatch_function_t work);

__END_DECLS

DISPATCH_ASSUME_NONNULL_END

#endif
