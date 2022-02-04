/*
 * Copyright (c) 2020 Apple Inc. All rights reserved.
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

#ifndef __DISPATCH_APPLY_PRIVATE__
#define __DISPATCH_APPLY_PRIVATE__

#ifndef __DISPATCH_INDIRECT__
#error "Please #include <dispatch/private.h> instead of this file directly."
#include <dispatch/base.h> // for HeaderDoc
#endif

__BEGIN_DECLS

DISPATCH_ASSUME_NONNULL_BEGIN
/*!
 * @typedef dispatch_apply_attr_s dispatch_apply_attr_t
 *
 * @abstract
 * Pointer to an opaque structure for describing the workload to be executed by
 * dispatch_apply_with_attr.
 *
 * This struct must be initialized with dispatch_apply_attr_init before use
 * and must not be copied once initialized. It must be destroyed with
 * dispatch_apply_attr_destroy before going out of scope or being freed, to
 * avoid leaking associated system resources.
 */
#define __DISPATCH_APPLY_ATTR_SIZE__ 64

#if defined(__DISPATCH_BUILDING_DISPATCH__) && !defined(__OBJC__)
typedef struct dispatch_apply_attr_s dispatch_apply_attr_s;
typedef struct dispatch_apply_attr_s *dispatch_apply_attr_t;
#else
struct dispatch_apply_attr_opaque_s {
	char opaque[__DISPATCH_APPLY_ATTR_SIZE__];
};
typedef struct dispatch_apply_attr_opaque_s dispatch_apply_attr_s;
typedef struct dispatch_apply_attr_opaque_s *dispatch_apply_attr_t;
#endif

/*!
 * @function dispatch_apply_attr_init, dispatch_apply_attr_destroy
 *
 * @abstract
 * Initializer and destructor functions for the attribute structure. The
 * attribute structure must be initialized before calling any setters on it.
 *
 * Every call to dispatch_apply_attr_init must be paired with a corresponding
 * call to dispatch_apply_attr_destroy.
 */
SPI_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0))
DISPATCH_EXPORT DISPATCH_NONNULL1
void
dispatch_apply_attr_init(dispatch_apply_attr_t attr);

SPI_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0))
DISPATCH_EXPORT DISPATCH_NONNULL1
void
dispatch_apply_attr_destroy(dispatch_apply_attr_t attr);

/*!
 * @enum dispatch_apply_attr_entity_t
 *
 * @abstract
 * This enum describes an entity in the hardware for which parallelism via
 * dispatch_apply is being requested
 */
DISPATCH_ENUM(dispatch_apply_attr_entity, unsigned long,
	DISPATCH_APPLY_ATTR_ENTITY_CPU = 1,
	DISPATCH_APPLY_ATTR_ENTITY_CLUSTER = 2,
);

/*!
 * @function dispatch_apply_attr_set_parallelism
 *
 * @param attr
 * The dispatch_apply attribute to be modified
 *
 * @param entity
 * The named entity the requested configuration applies to.
 *
 * @param threads_per_entity
 * The number of worker threads to be created per named entity on the system.
 *
 * @abstract
 * Adds a request for the system to start enough worker threads such that
 * threads_per_entity number of threads will share each named entity. The
 * system will make a best effort to spread such worker threads evenly
 * across the available entity.
 *
 * @notes
 * At the present time, the only supported value of threads_per_entity is 1.
 */
SPI_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0))
DISPATCH_EXPORT
void
dispatch_apply_attr_set_parallelism(dispatch_apply_attr_t attr,
	dispatch_apply_attr_entity_t entity, size_t threads_per_entity);

/*!
 * @typedef dispatch_apply_attr_query_flags_t
 *
 * @abstract
 * Flags that affect calls to dispatch_apply_attr_query().
 *
 * @const DISPATCH_APPLY_ATTR_QUERY_FLAGS_MAX_CURRENT_SCOPE
 * Modifies DISPATCH_APPLY_ATTR_QUERY_MAXIMUM_WORKERS so that it takes into
 * account the current execution context. This may produce a tighter upper bound
 * on the number of worker threads. If dispatch_apply_with_attr is called from
 * the current execution context, it is guaranteed that the worker_index will
 * not exceed the result of this query. However if the current execution context
 * is changed (for example with dispatch or pthread functions) or the current
 * scope is left, that guarantee will not hold.
 */
DISPATCH_ENUM(dispatch_apply_attr_query_flags, unsigned long,
	DISPATCH_APPLY_ATTR_QUERY_FLAGS_MAX_CURRENT_SCOPE DISPATCH_ENUM_API_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0)) = 1,
);

/*!
 * @typedef dispatch_apply_attr_query_t
 *
 * @abstract
 * Enumeration indicating question dispatch_apply_attr_query() should answer
 * about its arguments.
 *
 * @const DISPATCH_APPLY_ATTR_QUERY_VALID
 * Query if the properties requested by this attribute are invalid or
 * unsatisfiable. For example, some properties may describe how the workload will
 * use certain hardware resources. On machines which lack that hardware, an
 * attribute with those properties may be invalid.
 * Passing an invalid attribute to dispatch_apply_with_attr will have undefined
 * behaviour.
 * If the attribute is valid, the query returns 1. If it is not valid, the query
 * returns 0.
 *
 * @const DISPATCH_APPLY_ATTR_QUERY_MAXIMUM_WORKERS
 * Calculates an upper bound of how many parallel worker threads
 * dispatch_apply_with_attr could create when running a workload with the
 * specified attribute. This will include the thread calling
 * dispatch_apply_with_attr as a worker. This is an upper bound; depending on
 * conditions, such as the load of other work on the system and the execution
 * context where dispatch_apply_with_attr is called, fewer parallel worker
 * threads may actually be created.
 *
 * A good use of this query is to determine the size of a working arena
 * (such as preallocated memory space or other resources) appropriate for the
 * the maximum number of workers. This API can be used in coordination
 * with the worker_index block argument in dispatch_apply_with_attr to provide
 * each parallel worker thread with their own slice of the arena.
 *
 * @const DISPATCH_APPLY_ATTR_QUERY_LIKELY_WORKERS
 * Calculates a good guess of how many parallel worker threads
 * dispatch_apply_with_attr would likely create when running a workload with
 * the specified attribute. This will include the thread calling
 * dispatch_apply_with_attr as a worker. This is only a guess; depending on
 * conditions, dispatch_apply_with_attr may actually create more or fewer
 * parallel worker threads than this value.
 *
 * Compared to QUERY_MAXIMUM_WORKERS, this query tries to predict the behavior
 * of dispatch_apply_with_attr more faithfully. The number of parallel worker
 * threads to be used may be affected by aspects of the current execution context
 * like the thread's QOS class, scheduling priority, queue hierarchy, and current
 * workloop; as well as transitory aspects of the system like power state and
 * computational loads from other tasks. For those reasons, repeating this query
 * for the same attribute may produce a different result.
 */
DISPATCH_ENUM(dispatch_apply_attr_query, unsigned long,
	DISPATCH_APPLY_ATTR_QUERY_VALID DISPATCH_ENUM_API_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0)) = 0,
	DISPATCH_APPLY_ATTR_QUERY_MAXIMUM_WORKERS DISPATCH_ENUM_API_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0)) = 1,
	DISPATCH_APPLY_ATTR_QUERY_LIKELY_WORKERS DISPATCH_ENUM_API_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0)) = 2,
);

/*!
 * @function dispatch_apply_attr_query
 *
 * @abstract
 * Query how dispatch_apply_with_attr will respond to a certain attr, such
 * as how the attr may affect its choice of how many parallel worker threads
 * to use.
 *
 * @param attr
 * The dispatch_apply attribute describing a workload
 *
 * @param which
 * An enumeration value indicating which question this function should answer
 * about its arguments. See dispatch_apply_attr_query_t for possible values and
 * explanations.
 *
 * @param flags
 * Flags for the query that describe factors beyond the workload (which
 * is described by the attr). See dispatch_apply_attr_query_flags_t for
 * valid values. Pass 0 if no flags are needed.
 *
 * @return
 * Returns the numerical answer to the query. See dispatch_apply_attr_query_t.
 * Most types of query return 0 if the properties requested by this attribute
 * are invalid or unsatisfiable. (Exceptions will described in
 * dispatch_apply_attr_query_t entries).
 */
SPI_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0))
DISPATCH_EXPORT
size_t
dispatch_apply_attr_query(dispatch_apply_attr_t attr,
		dispatch_apply_attr_query_t which,
		dispatch_apply_attr_query_flags_t flags);

/*!
 * @function dispatch_apply_with_attr
 *
 * * @abstract
 * Submits a block for parallel invocation, with an attribute structure
 * describing the workload.
 *
 * @discussion
 * Submits a block for parallel invocation. The system will try to use worker
 * threads that match the configuration of the current thread. The system will
 * try to start an appropriate number of worker threads to maximimize
 * throughput given the available hardware and current system conditions. An
 * attribute structure that describes the nature of the workload may be passed.
 * The system will use the attribute's properties to improve its scheduling
 * choices, such as how many worker threads to create and how to distribute them
 * across processors.
 *
 * This function waits for all invocations of the task block to complete before
 * returning.
 *
 * Each invocation of the block will be passed 2 arguments:
 *    - the current index of iteration
 *    - the index of the worker thread invoking the block
 *
 * The worker index will be in the range [0, n)
 * where n = dispatch_apply_attr_query(attr, DISPATCH_APPLY_ATTR_QUERY_MAXIMUM_WORKERS, 0)
 *
 * Worker threads may start in any order. Some worker indexes within the
 * permissible range may not actually be used, depending on conditions.
 * Generally, one worker thread will use one worker index, but this is not
 * guaranteed; worker index MAY NOT match thread one-to-one. No assumptions
 * should be made about which CPU a worker runs on. Two invocations of
 * the block MAY have different worker indexes even if they run on the same
 * thread or the same processor. However, two invocations of the block running
 * at the same time WILL NEVER have the same worker index.
 *
 * When this API is called inside another dispatch_apply_with_attr or
 * dispatch_apply, it will execute as a serial loop.
 *
 * @param iterations
 * The number of iterations to perform.
 *
 * The choice of how to divide a large workload into a number of iterations can
 * have substantial effects on the performance of executing that workload.
 * If the number of iterations is very small, the system may not effectively
 * spread and balance the work across the available hardware. As a rough
 * guideline, the number of iterations should be at least three times the maximum
 * worker index. On the other hand, a workload should not be finely divided into
 * a huge number of iterations, each doing only a miniscule amount of work, since
 * there is a small overhead cost of accounting and invocation for each iteration.
 *
 * @param attr
 * The dispatch_apply_attr_t describing specialized properties of the workload.
 * This value can be NULL. If non-NULL, the attribute must have been initialized
 * with dispatch_apply_attr_init().
 *
 * If the attribute requests properties that are invalid or meaningless on this
 * system, the function will have undefined behaviour. This is a programming
 * error. An attribute's validity can be checked with dispatch_apply_attr_query.
 *
 * @param block
 * The block to be invoked the specified number of iterations.
 * The result of passing NULL in this parameter is undefined.
 */
#ifdef __BLOCKS__
SPI_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0))
DISPATCH_EXPORT
void
dispatch_apply_with_attr(size_t iterations, dispatch_apply_attr_t _Nullable attr,
    DISPATCH_NOESCAPE void (^block)(size_t iteration, size_t worker_index));
#endif

/*!
 * @function dispatch_apply_with_attr_f
 *
 * * @abstract
 * Submits a function for parallel invocation, with an attribute structure
 * describing the workload.
 *
 * @discussion
 * See dispatch_apply_with_attr() for details.
 *
 * @param iterations
 * The number of iterations to perform.
 *
 * @param attr
 * The dispatch_apply_attr_t describing specialized properties of the workload.
 * This value can be NULL. If non-NULL, the attribute must have been initialized
 * with dispatch_apply_attr_init().
 *
 * If the attribute requests properties that are invalid or meaningless on this
 * system, the function will have undefined behaviour. This is a programming
 * error. An attribute's validity can be checked with dispatch_apply_attr_query.
 *
 * @param context
 * The application-defined context parameter to pass to the function.

 * @param work
 * The application-defined function to invoke on the specified queue. The first
 * parameter passed to this function is the context provided to
 * dispatch_apply_with_attr_f(). The second parameter passed to this function is
 * the current index of iteration. The third parameter passed to this function is
 * the index of the worker thread invoking the function.
 * See dispatch_apply_with_attr() for details.
 * The result of passing NULL in this parameter is undefined.
 */
SPI_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0))
DISPATCH_EXPORT
void
dispatch_apply_with_attr_f(size_t iterations, dispatch_apply_attr_t _Nullable attr,
    void *_Nullable context, void (*work)(void *_Nullable context, size_t iteration, size_t worker_index));

DISPATCH_ASSUME_NONNULL_END

__END_DECLS
#endif /* __DISPATCH_APPLY_PRIVATE__ */
