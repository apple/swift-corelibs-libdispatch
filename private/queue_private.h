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

/*
 * IMPORTANT: This header file describes INTERNAL interfaces to libdispatch
 * which are subject to change in future releases of Mac OS X. Any applications
 * relying on these interfaces WILL break.
 */

#ifndef __DISPATCH_QUEUE_PRIVATE__
#define __DISPATCH_QUEUE_PRIVATE__

#ifndef __DISPATCH_INDIRECT__
#error "Please #include <dispatch/private.h> instead of this file directly."
#include <dispatch/base.h> // for HeaderDoc
#endif

DISPATCH_ASSUME_NONNULL_BEGIN

__BEGIN_DECLS

/*!
 * @enum dispatch_queue_flags_t
 *
 * @constant DISPATCH_QUEUE_OVERCOMMIT
 * The queue will create a new thread for invoking blocks, regardless of how
 * busy the computer is.
 */
enum {
	DISPATCH_QUEUE_OVERCOMMIT = 0x2ull,
};

#define DISPATCH_QUEUE_FLAGS_MASK (DISPATCH_QUEUE_OVERCOMMIT)

/*!
 * @function dispatch_queue_attr_make_with_overcommit
 *
 * @discussion
 * Returns a dispatch queue attribute value with the overcommit flag set to the
 * specified value.
 *
 * This attribute only makes sense when the specified queue is targeted at
 * a root queue. Passing this attribute to dispatch_queue_create_with_target()
 * with a target queue that is not a root queue will result in an assertion and
 * the process being terminated.
 *
 * It is recommended to not specify a target queue at all when using this
 * attribute and to use dispatch_queue_attr_make_with_qos_class() to select the
 * appropriate QoS class instead.
 *
 * Queues created with this attribute cannot change target after having been
 * activated. See dispatch_set_target_queue() and dispatch_activate().
 *
 * @param attr
 * A queue attribute value to be combined with the overcommit flag, or NULL.
 *
 * @param overcommit
 * Boolean overcommit flag.
 *
 * @return
 * Returns an attribute value which may be provided to dispatch_queue_create().
 * This new value combines the attributes specified by the 'attr' parameter and
 * the overcommit flag.
 */
API_AVAILABLE(macos(10.10), ios(8.0))
DISPATCH_EXPORT DISPATCH_WARN_RESULT DISPATCH_PURE DISPATCH_NOTHROW
dispatch_queue_attr_t
dispatch_queue_attr_make_with_overcommit(dispatch_queue_attr_t _Nullable attr,
		bool overcommit);

/*!
 * @typedef dispatch_queue_priority_t
 *
 * @constant DISPATCH_QUEUE_PRIORITY_NON_INTERACTIVE
 * Items dispatched to the queue will run at non-interactive priority.
 * This priority level is intended for user-initiated application activity that
 * is long-running and CPU or IO intensive and that the user is actively waiting
 * on, but that should not interfere with interactive use of the application.
 *
 * This global queue priority level is mapped to QOS_CLASS_UTILITY.
 */
#define DISPATCH_QUEUE_PRIORITY_NON_INTERACTIVE INT8_MIN

/*!
 * @function dispatch_queue_set_label_nocopy
 *
 * @abstract
 * Set the label for a given queue, without copying the input string.
 *
 * @discussion
 * The queue must have been initially created with a NULL label, else using
 * this function to set the queue label is undefined.
 *
 * The caller of this function must make sure the label pointer remains valid
 * while it is used as the queue label and while any callers to
 * dispatch_queue_get_label() may have obtained it. Since the queue lifetime
 * may extend past the last release, it is advised to call this function with
 * a constant string or NULL before the queue is released, or to destroy the
 * label from a finalizer for that queue.
 *
 * This function should be called before any work item could call
 * dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL) or from the context of
 * the queue itself.
 *
 * @param queue
 * The queue to adjust. Attempts to set the label of the main queue or a global
 * concurrent queue will be ignored.
 *
 * @param label
 * The new label for the queue.
 */
API_AVAILABLE(macos(10.12), ios(10.0), tvos(10.0), watchos(3.0))
DISPATCH_EXPORT DISPATCH_NONNULL1 DISPATCH_NOTHROW
void
dispatch_queue_set_label_nocopy(dispatch_queue_t queue, const char *label);

/*!
 * @function dispatch_queue_set_width
 *
 * @abstract
 * Set the width of concurrency for a given queue. The width of a serial queue
 * is one.
 *
 * @discussion
 * This SPI is DEPRECATED and will be removed in a future release.
 * Uses of this SPI to make a queue concurrent by setting its width to LONG_MAX
 * should be replaced by passing DISPATCH_QUEUE_CONCURRENT to
 * dispatch_queue_create().
 * Uses of this SPI to limit queue concurrency are not recommended and should
 * be replaced by alternative mechanisms such as a dispatch semaphore created
 * with the desired concurrency width.
 *
 * @param queue
 * The queue to adjust. Attempts to set the width of the main queue or a global
 * concurrent queue will be ignored.
 *
 * @param width
 * The new maximum width of concurrency depending on available resources.
 * If zero is passed, then the value is promoted to one.
 * Negative values are magic values that map to automatic width values.
 * Unknown negative values default to DISPATCH_QUEUE_WIDTH_MAX_LOGICAL_CPUS.
 */
#define DISPATCH_QUEUE_WIDTH_ACTIVE_CPUS		-1
#define DISPATCH_QUEUE_WIDTH_MAX_PHYSICAL_CPUS	-2
#define DISPATCH_QUEUE_WIDTH_MAX_LOGICAL_CPUS	-3

API_DEPRECATED("Use dispatch_queue_create(name, DISPATCH_QUEUE_CONCURRENT)",
		macos(10.6,10.10), ios(4.0,8.0))
DISPATCH_EXPORT DISPATCH_NONNULL_ALL DISPATCH_NOTHROW
void
dispatch_queue_set_width(dispatch_queue_t dq, long width);

#ifdef __BLOCKS__
/*!
 * @function dispatch_pthread_root_queue_create
 *
 * @abstract
 * Creates a new concurrent dispatch root queue with a pthread-based pool of
 * worker threads owned by the application.
 *
 * @discussion
 * Dispatch pthread root queues are similar to the global concurrent dispatch
 * queues in that they invoke blocks concurrently, however the blocks are not
 * executed on ordinary worker threads but use a dedicated pool of pthreads not
 * shared with the global queues or any other pthread root queues.
 *
 * NOTE: this is a special-purpose facility that should only be used in very
 * limited circumstances, in almost all cases the global concurrent queues
 * should be preferred. While this facility allows for more flexibility in
 * configuring worker threads for special needs it comes at the cost of
 * increased overall memory usage due to reduced thread sharing and higher
 * latency in worker thread bringup.
 *
 * Dispatch pthread root queues do not support suspension, application context
 * and change of width or of target queue. They can however be used as the
 * target queue for serial or concurrent queues obtained via
 * dispatch_queue_create() or dispatch_queue_create_with_target(), which
 * enables the blocks submitted to those queues to be processed on the root
 * queue's pthread pool.
 *
 * When a dispatch pthread root queue is no longer needed, it should be
 * released with dispatch_release(). Existing worker pthreads and pending blocks
 * submitted to the root queue will hold a reference to the queue so it will not
 * be deallocated until all blocks have finished and worker threads exited.
 *
 * @param label
 * A string label to attach to the queue.
 * This parameter is optional and may be NULL.
 *
 * @param flags
 * Pass flags value returned by dispatch_pthread_root_queue_flags_pool_size()
 * or 0 if unused.
 *
 * @param attr
 * Attributes passed to pthread_create(3) when creating worker pthreads. This
 * parameter is copied and can be destroyed after this call returns.
 * This parameter is optional and may be NULL.
 *
 * @param configure
 * Configuration block called on newly created worker pthreads before any blocks
 * for the root queue are executed. The block may configure the current thread
 * as needed.
 * This parameter is optional and may be NULL.
 *
 * @result
 * The newly created dispatch pthread root queue.
 */
API_AVAILABLE(macos(10.9), ios(6.0))
DISPATCH_EXPORT DISPATCH_MALLOC DISPATCH_RETURNS_RETAINED DISPATCH_WARN_RESULT
DISPATCH_NOTHROW
dispatch_queue_t
dispatch_pthread_root_queue_create(const char *_Nullable label,
	unsigned long flags, const pthread_attr_t *_Nullable attr,
	dispatch_block_t _Nullable configure);

/*!
 * @function dispatch_pthread_root_queue_flags_pool_size
 *
 * @abstract
 * Returns flags argument to pass to dispatch_pthread_root_queue_create() to
 * specify the maximum size of the pthread pool to use for a pthread root queue.
 *
 * @param pool_size
 * Maximum size of the pthread pool to use for the root queue. The number of
 * pthreads created for this root queue will never exceed this number but there
 * is no guarantee that the specified number will be reached.
 * Pass 0 to specify that a default pool size determined by the system should
 * be used.
 * NOTE: passing pool_size == 1 does NOT make the pthread root queue equivalent
 *       to a serial queue.
 *
 * @result
 * The flags argument to pass to dispatch_pthread_root_queue_create().
 */
DISPATCH_INLINE DISPATCH_ALWAYS_INLINE
unsigned long
dispatch_pthread_root_queue_flags_pool_size(uint8_t pool_size)
{
	#define _DISPATCH_PTHREAD_ROOT_QUEUE_FLAG_POOL_SIZE (0x80000000ul)
	return (_DISPATCH_PTHREAD_ROOT_QUEUE_FLAG_POOL_SIZE |
			(unsigned long)pool_size);
}

#endif /* __BLOCKS__ */

/*!
 * @function dispatch_pthread_root_queue_copy_current
 *
 * @abstract
 * Returns a reference to the pthread root queue object that has created the
 * currently executing thread, or NULL if the current thread is not associated
 * to a pthread root queue.
 *
 * @result
 * A new reference to a pthread root queue object or NULL.
 */
API_AVAILABLE(macos(10.12), ios(10.0), tvos(10.0), watchos(3.0))
DISPATCH_EXPORT DISPATCH_RETURNS_RETAINED DISPATCH_WARN_RESULT DISPATCH_NOTHROW
dispatch_queue_t _Nullable
dispatch_pthread_root_queue_copy_current(void);

/*!
 * @constant DISPATCH_APPLY_CURRENT_ROOT_QUEUE
 * @discussion Constant to pass to the dispatch_apply() and dispatch_apply_f()
 * functions to indicate that the root queue for the current thread should be
 * used (i.e. one of the global concurrent queues or a queue created with
 * dispatch_pthread_root_queue_create()). If there is no such queue, the
 * default priority global concurrent queue will be used.
 */
#define DISPATCH_APPLY_CURRENT_ROOT_QUEUE ((dispatch_queue_t _Nonnull)0)

/*!
 * @function dispatch_async_enforce_qos_class_f
 *
 * @abstract
 * Submits a function for asynchronous execution on a dispatch queue.
 *
 * @discussion
 * See dispatch_async() for details. The QOS will be enforced as if
 * this was called:
 * <code>
 *     dispatch_async(queue, dispatch_block_create(DISPATCH_BLOCK_ENFORCE_QOS_CLASS, ^{
 *         work(context);
 *     });
 * </code>
 *
 * @param queue
 * The target dispatch queue to which the function is submitted.
 * The system will hold a reference on the target queue until the function
 * has returned.
 * The result of passing NULL in this parameter is undefined.
 *
 * @param context
 * The application-defined context parameter to pass to the function.
 *
 * @param work
 * The application-defined function to invoke on the target queue. The first
 * parameter passed to this function is the context provided to
 * dispatch_async_f().
 * The result of passing NULL in this parameter is undefined.
 */
API_AVAILABLE(macos(10.11), ios(9.0))
DISPATCH_EXPORT DISPATCH_NONNULL1 DISPATCH_NONNULL3 DISPATCH_NOTHROW
void
dispatch_async_enforce_qos_class_f(dispatch_queue_t queue,
	void *_Nullable context, dispatch_function_t work);


__END_DECLS

DISPATCH_ASSUME_NONNULL_END

#endif
