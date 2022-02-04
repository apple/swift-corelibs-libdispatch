#ifndef __OS_WORKGROUP_OBJECT_PRIVATE__
#define __OS_WORKGROUP_OBJECT_PRIVATE__

#ifndef __OS_WORKGROUP_PRIVATE_INDIRECT__
#error "Please #include <os/workgroup.h> instead of this file directly."
#include <os/workgroup_base.h> // For header doc
#endif

#include <os/workgroup_object.h>

__BEGIN_DECLS

OS_WORKGROUP_ASSUME_NONNULL_BEGIN

/* Attribute creation and specification */

/* This is for clients who want to build their own workgroup attribute from
 * scratch instead of configuring their attributes on top of the default set of
 * attributes */
#define OS_WORKGROUP_ATTR_INITIALIZER_EMPTY { .sig = _OS_WORKGROUP_ATTR_SIG_EMPTY_INIT }

/*!
 * @enum os_workgroup_attr_flags_t
 *
 * @abstract A bitfield of flags describing options for workgroup configuration
 */
OS_ENUM(os_workgroup_attr_flags, uint32_t,
	/*!
	 * @const OS_WORKGROUP_ATTR_NONPROPAGATING
	 *
	 * Asynchronous work initiated by threads which are members of a
	 * workgroup with OS_WORKGROUP_ATTR_NONPROPAGATING attribute, will not
	 * automatically be tracked as part of the workgroup. This applies to work
	 * initiated by calls such as dispatch_async() that may propagate other
	 * execution context properties.
	 *
	 * os_workgroups which are propagating by default can opt out this behavior
	 * by specifying the OS_WORKGROUP_ATTR_NONPROPAGATING flag.
	 */
	OS_WORKGROUP_ATTR_NONPROPAGATING = (1 << 1),

	/*!
	 * @const OS_WORKGROUP_ATTR_UNDIFFERENTIATED
	 *
	 * Member threads of a workgroup with the attribute flag
	 * OS_WORKGROUP_ATTR_UNDIFFERENTIATED are tracked and measured together with
	 * other threads in their process by the system for scheduling and
	 * performance control.
	 *
	 * os_workgroups which are tracked separately from other threads in
	 * the process by default, can opt out of it by specifying the
	 * OS_WORKGROUP_ATTR_UNDIFFERENTIATED flag.
	 */
	OS_WORKGROUP_ATTR_UNDIFFERENTIATED = (1 << 2)
);

/*!
 * @function os_workgroup_attr_set_flags
 *
 * @abstract
 * Sets the user specified flags in the workgroup attribute. If invalid
 * attributes are specified, this function will set and return an error.
 */
API_AVAILABLE(macos(10.16), ios(14.0), tvos(14.0), watchos(7.0))
OS_WORKGROUP_EXPORT OS_WORKGROUP_WARN_RESULT
int
os_workgroup_attr_set_flags(os_workgroup_attr_t wga,
		os_workgroup_attr_flags_t flags);

/*!
 * @function os_workgroup_create
 *
 * @abstract
 * Creates an os_workgroup_t with the specified name and attributes.
 * A newly created os_workgroup_t has no initial member threads - in particular
 * the creating thread does not join the os_workgroup_t implicitly.
 *
 * @param name
 * A client specified string for labelling the workgroup. This parameter is
 * optional and can be NULL.
 *
 * @param wga
 * The requested set of os_workgroup_t attributes. NULL is to be specified for
 * the default set of attributes. A workgroup with default attributes is
 * propagating with asynchronous work and differentiated from other threads in
 * the process (see os_workgroup_attr_flags_t).
 *
 * The attribute flag OS_WORKGROUP_ATTR_NONPROPAGATING MUST currently be
 * specified. If it isn't or if invalid attributes are specified, this function
 * will return NULL and set an errno.
 */
API_AVAILABLE(macos(10.16), ios(14.0), tvos(14.0), watchos(7.0))
OS_WORKGROUP_EXPORT OS_WORKGROUP_RETURNS_RETAINED
os_workgroup_t _Nullable
os_workgroup_create(const char * _Nullable name,
		os_workgroup_attr_t _Nullable wga);

/*!
 * @function os_workgroup_create_with_workload_id
 *
 * @abstract
 * Creates an os_workgroup_t with the specified name and workload identifier.
 *
 * The newly created os_workgroup_t has no initial member threads - in
 * particular the creating thread does not join the os_workgroup_t implicitly.
 *
 * @param name
 * A client specified string for labelling the workgroup. This parameter is
 * optional and can be NULL.
 *
 * @param workload_id
 * A system-defined workload identifier string determining the configuration
 * parameters to apply to the workgroup and its member threads.
 * Must not be NULL.
 * See discussion for the detailed rules used to combine the information
 * specified by the `workload_id` and `wga` arguments.
 *
 * @param wga
 * The requested set of os_workgroup_t attributes. NULL is to be specified for
 * the default set of attributes. By default, a workgroup created with workload
 * identifier is nonpropagating with asynchronous work and differentiated from
 * other threads in the process (see os_workgroup_attr_flags_t).
 * Currently NULL or the default set of attributes are the only valid
 * attributes for this function.
 * See discussion for the detailed rules used to combine the information
 * specified by the `workload_id` and `wga` arguments.
 *
 * @discussion
 * Rules used for resolution of configuration parameters potentially specified
 * by both workload identifier and attributes, applied in order:
 * - If the provided attributes are NULL or equal to the default set of
 *   attributes, no parameters are considered to be explicitly specified via
 *   attribute.
 * - If the provided workload identifier is known, and the provided attributes
 *   explicitly specify a parameter that is also configured by the identifier,
 *   the two parameter values must match or this function will fail and return
 *   an error.
 * - If the provided workload identifier is known, the parameters configured by
 *   the identifier will be used.
 * - If the provided workload identifier is unknown, the parameters specified
 *   via the provided attributes will be used as a fallback.
 * - If a given parameter is neither configured by a known workload identifier
 *   or explicitly specified via an attribute, a system-dependent fallback
 *   value will be used.
 *
 * @result
 * The newly created workgroup object, or NULL if invalid arguments were
 * specified (in which case errno is also set).
 */
SPI_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0))
OS_WORKGROUP_EXPORT OS_WORKGROUP_RETURNS_RETAINED
os_workgroup_t _Nullable
os_workgroup_create_with_workload_id(const char * _Nullable name,
		const char *workload_id, os_workgroup_attr_t _Nullable wga);

/*!
 * @function os_workgroup_create_with_workload_id_and_port
 *
 * @abstract
 * Create an os_workgroup_t object with the specified name and workload
 * identifier from a send right returned by a previous call to
 * os_workgroup_copy_port, potentially in a different process.
 *
 * The newly created os_workgroup_t has no initial member threads - in
 * particular the creating thread does not join the os_workgroup_t implicitly.
 *
 * @param name
 * A client specified string for labelling the workgroup. This parameter is
 * optional and can be NULL.
 *
 * @param workload_id
 * A system-defined workload identifier string determining the configuration
 * parameters to apply to the workgroup and its member threads.
 * Must not be NULL.
 * See discussion for the detailed rules used to combine the information
 * specified by the `workload_id` and `mach_port` arguments.
 *
 * @param mach_port
 * The send right to create the workgroup from. No reference is consumed
 * on the specified send right.
 * See discussion for the detailed rules used to combine the information
 * specified by the `workload_id` and `mach_port` arguments.
 *
 * @discussion
 * Rules used for resolution of configuration parameters potentially specified
 * by both workload identifier and send right, applied in order:
 * - If the provided workload identifier is known, and the provided send right
 *   references a workgroup that was created with a parameter that is also
 *   configured by the identifier, the parameter value configured by the
 *   identifier will be used. For certain parameters such as the kernel
 *   work_interval type underlying a workgroup interval type, it is required
 *   that the two parameter values must match, or this function will fail and
 *   return an error.
 * - If the provided workload identifier is known, the parameters configured by
 *   the identifier will be used.
 * - If the provided workload identifier is unknown, the parameters used to
 *   create the workgroup referenced by the provided send right are used.
 * - If a given parameter is neither configured by a known workload identifier
 *   or was used to create the workgroup referenced by the provided send right,
 *   a system-dependent fallback value will be used.
 *
 * @result
 * The newly created workgroup object, or NULL if invalid arguments were
 * specified (in which case errno is also set).
 */
SPI_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0))
OS_WORKGROUP_EXPORT OS_WORKGROUP_RETURNS_RETAINED
os_workgroup_t _Nullable
os_workgroup_create_with_workload_id_and_port(const char * _Nullable name,
		const char *workload_id, mach_port_t mach_port);

/*!
 * @function os_workgroup_create_with_workload_id_and_workgroup
 *
 * @abstract
 * Create a new os_workgroup object with the specified name and workload
 * identifier from an existing os_workgroup.
 *
 * The newly created os_workgroup_t has no initial member threads - in
 * particular the creating thread does not join the os_workgroup_t implicitly.
 *
 * @param name
 * A client specified string for labelling the workgroup. This parameter is
 * optional and can be NULL.
 *
 * @param workload_id
 * A system-defined workload identifier string determining the configuration
 * parameters to apply to the workgroup and its member threads.
 * Must not be NULL.
 * See discussion for the detailed rules used to combine the information
 * specified by the `workload_id` and `wg` arguments.
 *
 * @param wg
 * The existing workgroup to create a new workgroup object from.
 * See discussion for the detailed rules used to combine the information
 * specified by the `workload_id` and `wg` arguments.
 *
 * @discussion
 * Rules used for resolution of configuration parameters potentially specified
 *  by both workload identifier and existing workgroup, applied in order:
 * - If the provided workload identifier is known, and the provided workgroup
 *   was created with a parameter that is also configured by the identifier,
 *   the parameter value configured by the identifier will be used. For certain
 *   parameters such as the kernel work_interval type underlying a workgroup
 *   interval type, it is required that the two parameter values must match, or
 *   this function will fail and return an error.
 * - If the provided workload identifier is known, the parameters configured by
 *   the identifier will be used.
 * - If the provided workload identifier is unknown, the parameters used to
 *   create the provided workgroup will be used.
 * - If a given parameter is neither configured by a known workload identifier
 *   or was used to create the provided workgroup, a system-dependent fallback
 *   value will be used.
 *
 * @result
 * The newly created workgroup object, or NULL if invalid arguments were
 * specified (in which case errno is also set).
 */
SPI_AVAILABLE(macos(12.0), ios(15.0), tvos(15.0), watchos(8.0))
OS_WORKGROUP_EXPORT OS_WORKGROUP_RETURNS_RETAINED
os_workgroup_t _Nullable
os_workgroup_create_with_workload_id_and_workgroup(const char * _Nullable name,
		const char *workload_id, os_workgroup_t wg);

/* To be deprecated once coreaudio adopts */
#define OS_WORKGROUP_ATTR_INITIALIZER OS_WORKGROUP_ATTR_INITIALIZER_DEFAULT

typedef uint32_t os_workgroup_index;

/* Deprecated in favor of os_workgroup_join */
OS_WORKGROUP_EXPORT OS_WORKGROUP_WARN_RESULT
int
os_workgroup_join_self(os_workgroup_t wg, os_workgroup_join_token_t token_out,
		os_workgroup_index *_Nullable id_out);

/* Deprecated in favor of os_workgroup_leave */
OS_WORKGROUP_EXPORT
void
os_workgroup_leave_self(os_workgroup_t wg, os_workgroup_join_token_t token);

OS_WORKGROUP_ASSUME_NONNULL_END

__END_DECLS

#endif /* __OS_WORKGROUP_OBJECT__ */
