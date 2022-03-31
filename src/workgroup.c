/*
 * Copyright (c) 2019-2021 Apple Inc. All rights reserved.
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

#define PTHREAD_WORKGROUP_SPI 1

#include "internal.h"

#include <os/assumes.h>
#include <mach/mach_port.h>
#include <pthread/workgroup_private.h>

/* Declares struct symbols */

OS_OBJECT_CLASS_DECL(os_workgroup);
#if !USE_OBJC
OS_OBJECT_VTABLE_INSTANCE(os_workgroup,
		(void (*)(_os_object_t))_os_workgroup_explicit_xref_dispose,
		(void (*)(_os_object_t))_os_workgroup_explicit_dispose);
#endif // !USE_OBJC
#define WORKGROUP_CLASS OS_OBJECT_VTABLE(os_workgroup)

OS_OBJECT_CLASS_DECL(os_workgroup_interval);
#if !USE_OBJC
OS_OBJECT_VTABLE_INSTANCE(os_workgroup_interval,
		(void (*)(_os_object_t))_os_workgroup_interval_explicit_xref_dispose,
		(void (*)(_os_object_t))_os_workgroup_interval_explicit_dispose);
#endif // USE_OBJC
#define WORKGROUP_INTERVAL_CLASS OS_OBJECT_VTABLE(os_workgroup_interval)

OS_OBJECT_CLASS_DECL(os_workgroup_parallel);
#if !USE_OBJC
OS_OBJECT_VTABLE_INSTANCE(os_workgroup_parallel,
		(void (*)(_os_object_t))_os_workgroup_explicit_xref_dispose,
		(void (*)(_os_object_t))_os_workgroup_explicit_dispose);
#endif // USE_OBJC
#define WORKGROUP_PARALLEL_CLASS OS_OBJECT_VTABLE(os_workgroup_parallel)

#pragma mark Internal functions

/* These are default workgroup attributes to be used when no user attribute is
 * passed in in creation APIs.
 *
 * For all classes, workgroup propagation is currently not supported.
 *
 * Class						Default attribute			Eventually supported
 *
 * os_workgroup_t				propagating					nonpropagating, propagating
 * os_workgroup_interval_t		nonpropagating				nonpropagating, propagating
 * os_workgroup_parallel_t		nonpropagating				nonpropagating
 *
 * Class						Default attribute			supported
 * os_workgroup_t				differentiated				differentiated, undifferentiated
 * os_workgroup_interval_t		differentiated				differentiated
 * os_workgroup_parallel_t		undifferentiated			undifferentiated, differentiated
 */
static const struct os_workgroup_attr_s _os_workgroup_attr_default = {
	.sig = _OS_WORKGROUP_ATTR_RESOLVED_INIT,
	.wg_type = OS_WORKGROUP_TYPE_DEFAULT,
	.wg_attr_flags = 0,
};

static const struct os_workgroup_attr_s _os_workgroup_with_workload_id_attr_default = {
	.sig = _OS_WORKGROUP_ATTR_RESOLVED_INIT,
	.wg_type = OS_WORKGROUP_TYPE_DEFAULT,
	.wg_attr_flags = OS_WORKGROUP_ATTR_NONPROPAGATING,
};

static const struct os_workgroup_attr_s _os_workgroup_interval_attr_default = {
	.sig = _OS_WORKGROUP_ATTR_RESOLVED_INIT,
	.wg_type = OS_WORKGROUP_INTERVAL_TYPE_DEFAULT,
	.wg_attr_flags = OS_WORKGROUP_ATTR_NONPROPAGATING
};

static const struct os_workgroup_attr_s _os_workgroup_parallel_attr_default = {
	.sig = _OS_WORKGROUP_ATTR_RESOLVED_INIT,
	.wg_type = OS_WORKGROUP_TYPE_PARALLEL,
	.wg_attr_flags = OS_WORKGROUP_ATTR_NONPROPAGATING |
		OS_WORKGROUP_ATTR_UNDIFFERENTIATED,
};

void
_os_workgroup_xref_dispose(os_workgroup_t wg)
{
	os_workgroup_arena_t arena = wg->wg_arena;

	if (arena == NULL) {
		return;
	}

	arena->destructor(arena->client_arena);
	free(arena);
}

void
_os_workgroup_interval_xref_dispose(os_workgroup_interval_t wgi)
{
	uint64_t wg_state = wgi->wg_state;
	if (wg_state & OS_WORKGROUP_INTERVAL_STARTED) {
		os_crash("BUG IN CLIENT: Releasing last reference to workgroup interval "
			"while an interval has been started");
	}
}

#if !USE_OBJC
void
_os_workgroup_explicit_xref_dispose(os_workgroup_t wg)
{
	_os_workgroup_xref_dispose(wg);
	_os_object_release_internal(wg->_as_os_obj);
}

void
_os_workgroup_interval_explicit_xref_dispose(os_workgroup_interval_t wgi)
{
	_os_workgroup_interval_xref_dispose(wgi);
	_os_workgroup_explicit_xref_dispose(wgi->_as_wg);
}
#endif

static inline bool
_os_workgroup_is_configurable(uint64_t wg_state)
{
	return (wg_state & OS_WORKGROUP_OWNER) == OS_WORKGROUP_OWNER;
}

#if !USE_OBJC
void
_os_workgroup_explicit_dispose(os_workgroup_t wg)
{
	_os_workgroup_dispose(wg);
	free(wg);
}
#endif

void
_os_workgroup_dispose(os_workgroup_t wg)
{
	dispatch_assert(wg->joined_cnt == 0);

	kern_return_t kr;
	uint64_t wg_state = os_atomic_load(&wg->wg_state, relaxed);
	if (_os_workgroup_is_configurable(wg_state)) {
		kr = work_interval_destroy(wg->wi);
	} else {
		kr = mach_port_mod_refs(mach_task_self(), wg->port, MACH_PORT_RIGHT_SEND, -1);
	}
	os_assumes(kr == KERN_SUCCESS);
	if (wg_state & OS_WORKGROUP_LABEL_NEEDS_FREE) {
		free((void *)wg->name);
	}
}

void
_os_workgroup_debug(os_workgroup_t wg, char *buf, size_t size)
{
	snprintf(buf, size, "wg[%p] = {xref = %d, ref = %d, name = %s}",
			(void *) wg, wg->do_xref_cnt + 1, wg->do_ref_cnt + 1, wg->name);
}

void
_os_workgroup_interval_dispose(os_workgroup_interval_t wgi)
{
	work_interval_instance_free(wgi->wii);
}

#if !USE_OBJC
void
_os_workgroup_interval_explicit_dispose(os_workgroup_interval_t wgi)
{
	_os_workgroup_interval_dispose(wgi);
	_os_workgroup_explicit_dispose(wgi->_as_wg);
}
#endif

#define os_workgroup_inc_refcount(wg)  \
	_os_object_retain_internal(wg->_as_os_obj);

#define os_workgroup_dec_refcount(wg)  \
	_os_object_release_internal(wg->_as_os_obj);

void
_os_workgroup_tsd_cleanup(void *ctxt) /* Destructor for the tsd key */
{
	os_workgroup_t wg = (os_workgroup_t) ctxt;
	if (wg != NULL) {
		char buf[512];
		snprintf(buf, sizeof(buf), "BUG IN CLIENT: Thread exiting without leaving workgroup '%s'", wg->name);

		os_crash(buf);
	}
}

static os_workgroup_t
_os_workgroup_get_current(void)
{
	return (os_workgroup_t) _dispatch_thread_getspecific(os_workgroup_key);
}

static void
_os_workgroup_set_current(os_workgroup_t new_wg)
{
	if (new_wg != NULL) {
		os_workgroup_inc_refcount(new_wg);
	}

	os_workgroup_t old_wg = _os_workgroup_get_current();
	_dispatch_thread_setspecific(os_workgroup_key, new_wg);

	if (old_wg != NULL) {
		os_workgroup_dec_refcount(old_wg);
	}
}

static inline bool
_os_workgroup_attr_is_resolved(os_workgroup_attr_t attr)
{
	return (attr->sig == _OS_WORKGROUP_ATTR_RESOLVED_INIT);
}

static inline bool
_os_workgroup_client_attr_initialized(os_workgroup_attr_t attr)
{
	return (attr->sig == _OS_WORKGROUP_ATTR_SIG_DEFAULT_INIT) ||
			(attr->sig == _OS_WORKGROUP_ATTR_SIG_EMPTY_INIT);
}

static inline bool
_os_workgroup_attr_is_propagating(os_workgroup_attr_t attr)
{
	return (attr->wg_attr_flags & OS_WORKGROUP_ATTR_NONPROPAGATING) == 0;
}

static inline bool
_os_workgroup_attr_is_differentiated(os_workgroup_attr_t attr)
{
	return (attr->wg_attr_flags & OS_WORKGROUP_ATTR_UNDIFFERENTIATED) == 0;
}

static inline bool
_os_workgroup_type_is_interval_type(os_workgroup_type_t wg_type)
{
	return (wg_type >= OS_WORKGROUP_INTERVAL_TYPE_DEFAULT) &&
			(wg_type <= OS_WORKGROUP_INTERVAL_TYPE_ARKIT);
}

static bool
_os_workgroup_type_is_audio_type(os_workgroup_type_t wg_type)
{
	return (wg_type == OS_WORKGROUP_INTERVAL_TYPE_COREAUDIO) ||
			(wg_type == OS_WORKGROUP_INTERVAL_TYPE_AUDIO_CLIENT);
}

static inline bool
_os_workgroup_type_is_parallel_type(os_workgroup_type_t wg_type)
{
	return wg_type == OS_WORKGROUP_TYPE_PARALLEL;
}

static inline bool
_os_workgroup_type_is_default_type(os_workgroup_type_t wg_type)
{
	return wg_type == OS_WORKGROUP_TYPE_DEFAULT;
}


static inline bool
_os_workgroup_has_backing_workinterval(os_workgroup_t wg)
{
	return wg->wi != NULL;
}

static inline uint32_t
_wi_flags_to_wi_type(uint32_t wi_flags)
{
	return wi_flags & WORK_INTERVAL_TYPE_MASK;
}

#if !TARGET_OS_SIMULATOR
static os_workgroup_type_t
_wi_flags_to_wg_type(uint32_t wi_flags)
{
	uint32_t type = _wi_flags_to_wi_type(wi_flags);
	bool is_unrestricted = (wi_flags & WORK_INTERVAL_FLAG_UNRESTRICTED);

	switch (type) {
	case WORK_INTERVAL_TYPE_DEFAULT:
		/* Technically, this could be OS_WORKGROUP_INTERVAL_TYPE_DEFAULT
		 * as well but we can't know so we just assume it's a regular
		 * workgroup
		 */
		return OS_WORKGROUP_TYPE_DEFAULT;
	case WORK_INTERVAL_TYPE_COREAUDIO:
		return (is_unrestricted ? OS_WORKGROUP_INTERVAL_TYPE_AUDIO_CLIENT :
				OS_WORKGROUP_INTERVAL_TYPE_COREAUDIO);
	case WORK_INTERVAL_TYPE_COREANIMATION:
		/* and WORK_INTERVAL_TYPE_CA_RENDER_SERVER */

		/* We cannot distinguish between
		 * OS_WORKGROUP_INTERVAL_TYPE_COREANIMATION and
		 * OS_WORKGROUP_INTERVAL_TYPE_CA_RENDER_SERVER since
		 * WORK_INTERVAL_TYPE_COREANIMATION and
		 * WORK_INTERVAL_TYPE_CA_RENDER_SERVER have the same value */
		return OS_WORKGROUP_INTERVAL_TYPE_COREANIMATION;
	case WORK_INTERVAL_TYPE_HID_DELIVERY:
		return OS_WORKGROUP_INTERVAL_TYPE_HID_DELIVERY;
	case WORK_INTERVAL_TYPE_COREMEDIA:
		return OS_WORKGROUP_INTERVAL_TYPE_COREMEDIA;
	case WORK_INTERVAL_TYPE_ARKIT:
		return OS_WORKGROUP_INTERVAL_TYPE_ARKIT;
	case WORK_INTERVAL_TYPE_CA_CLIENT:
		return OS_WORKGROUP_INTERVAL_TYPE_CA_CLIENT;
	default:
	{
		char buf[512];
		snprintf(buf, sizeof(buf), "BUG IN DISPATCH: Invalid wi flags = %u", wi_flags);
		os_crash(buf);
	}
	}
}
#endif

static uint32_t
_wg_type_to_wi_flags(os_workgroup_type_t wg_type)
{
	switch (wg_type) {
	case OS_WORKGROUP_INTERVAL_TYPE_DEFAULT:
		return WORK_INTERVAL_TYPE_DEFAULT | WORK_INTERVAL_FLAG_UNRESTRICTED;
	case OS_WORKGROUP_INTERVAL_TYPE_COREAUDIO:
		return (WORK_INTERVAL_TYPE_COREAUDIO |
				WORK_INTERVAL_FLAG_ENABLE_AUTO_JOIN |
				WORK_INTERVAL_FLAG_ENABLE_DEFERRED_FINISH);
	case OS_WORKGROUP_INTERVAL_TYPE_COREANIMATION:
		return WORK_INTERVAL_TYPE_COREANIMATION;
	case OS_WORKGROUP_INTERVAL_TYPE_CA_RENDER_SERVER:
		return WORK_INTERVAL_TYPE_CA_RENDER_SERVER;
	case OS_WORKGROUP_INTERVAL_TYPE_HID_DELIVERY:
		return WORK_INTERVAL_TYPE_HID_DELIVERY;
	case OS_WORKGROUP_INTERVAL_TYPE_COREMEDIA:
		return WORK_INTERVAL_TYPE_COREMEDIA;
	case OS_WORKGROUP_INTERVAL_TYPE_ARKIT:
		return (WORK_INTERVAL_TYPE_ARKIT |
				WORK_INTERVAL_FLAG_FINISH_AT_DEADLINE);
	case OS_WORKGROUP_INTERVAL_TYPE_AUDIO_CLIENT:
		return (WORK_INTERVAL_TYPE_COREAUDIO | WORK_INTERVAL_FLAG_UNRESTRICTED |
				WORK_INTERVAL_FLAG_ENABLE_AUTO_JOIN |
				WORK_INTERVAL_FLAG_ENABLE_DEFERRED_FINISH);
	case OS_WORKGROUP_INTERVAL_TYPE_CA_CLIENT:
		return WORK_INTERVAL_TYPE_CA_CLIENT | WORK_INTERVAL_FLAG_UNRESTRICTED;
	case OS_WORKGROUP_TYPE_DEFAULT:
		/* Non-interval workgroup types */
		return WORK_INTERVAL_FLAG_UNRESTRICTED;
	default:
		os_crash("Creating an os_workgroup of unknown type");
	}
}

static inline uint32_t
_wg_type_to_wi_type(os_workgroup_type_t wg_type)
{
	return _wi_flags_to_wi_type(_wg_type_to_wi_flags(wg_type));
}

static inline int
_os_workgroup_get_wg_wi_types_from_port(mach_port_t port,
		os_workgroup_type_t *out_wg_type, uint32_t *out_wi_type)
{
	os_workgroup_type_t wg_type = OS_WORKGROUP_TYPE_DEFAULT;
	uint32_t wi_type = WORK_INTERVAL_TYPE_DEFAULT;

#if !TARGET_OS_SIMULATOR
	uint32_t wi_flags = 0;
	int ret = work_interval_get_flags_from_port(port, &wi_flags);
	if (ret != 0) {
		return ret;
	}
	wg_type = _wi_flags_to_wg_type(wi_flags);
	wi_type = _wi_flags_to_wi_type(wi_flags);
#else
	(void)port;
#endif

	if (out_wg_type) *out_wg_type = wg_type;
	if (out_wi_type) *out_wi_type = wi_type;

	return 0;
}

static work_interval_t
_os_workgroup_create_work_interval(os_workgroup_attr_t attr)
{
	/* All workgroups are joinable */
	uint32_t flags = WORK_INTERVAL_FLAG_JOINABLE;

	flags |= _wg_type_to_wi_flags(attr->wg_type);

	if (_os_workgroup_attr_is_differentiated(attr)) {
		flags |= WORK_INTERVAL_FLAG_GROUP;
	}

	work_interval_t wi;
	int rv = work_interval_create(&wi, flags);
	if (rv) {
		return NULL;
	}

	return wi;
}

struct os_workgroup_workload_id_table_entry_s {
	const char* wl_id;
	os_workgroup_type_t wl_type;
};

#if !TARGET_OS_SIMULATOR
static const struct os_workgroup_workload_id_table_entry_s
		_os_workgroup_workload_id_table[] = {
	{
		.wl_id = "com.apple.coreaudio.hal.iothread",
		.wl_type = OS_WORKGROUP_INTERVAL_TYPE_COREAUDIO,
	},
	{
		.wl_id = "com.apple.coreaudio.hal.clientthread",
		.wl_type = OS_WORKGROUP_INTERVAL_TYPE_AUDIO_CLIENT,
	},
};
#endif // !TARGET_OS_SIMULATOR

static os_workgroup_type_t
_os_workgroup_lookup_type_from_workload_id(const char *workload_id)
{
	os_workgroup_type_t workload_type = OS_WORKGROUP_TYPE_DEFAULT;

	if (!workload_id) {
		DISPATCH_CLIENT_CRASH(0, "Workload identifier must not be NULL");
	}
#if !TARGET_OS_SIMULATOR
	for (size_t i = 0; i < countof(_os_workgroup_workload_id_table); i++) {
		if (!strcasecmp(workload_id, _os_workgroup_workload_id_table[i].wl_id)){
			workload_type = _os_workgroup_workload_id_table[i].wl_type;
			if (_os_workgroup_type_is_default_type(workload_type)) {
				DISPATCH_INTERNAL_CRASH(i, "Invalid workload ID type");
			}
			break;
		}
	}
#if OS_WORKGROUP_LOG_UKNOWN_WORKLOAD_ID
	if (_os_workgroup_type_is_default_type(workload_type)) {
		_dispatch_log("WARNING: os_workgroup: Unknown workload ID \"%s\"",
				workload_id);
	}
#endif
#endif // !TARGET_OS_SIMULATOR
	return workload_type;
}

static inline os_workgroup_attr_t
_os_workgroup_workload_id_attr_resolve(const char *workload_id,
		os_workgroup_attr_t attr,
		const os_workgroup_attr_s *default_attr)
{
	/* N.B: expects to be called with the attr pointer returned by
	 *      _os_workgroup_client_attr_resolve() (i.e. a mutable local copy) */
	os_workgroup_type_t wl_type =
			_os_workgroup_lookup_type_from_workload_id(workload_id);
	if (_os_workgroup_type_is_default_type(wl_type)) {
		/* Unknown workload ID, fallback to attribute type */
		return attr;
	}
	/* Require matching types between workload ID and attribute.
	 * Use workload ID type as the type implied by the default attribute */
	if (attr->wg_type == default_attr->wg_type) {
		attr->wg_type = wl_type;
	} else if (wl_type != attr->wg_type) {
		/* Workload ID and attribute type mismatch */
		return NULL;
	}
	return attr;
}

static inline bool
_os_workgroup_workload_id_is_valid_for_wi_type(const char *workload_id,
		uint32_t wi_type)
{
	os_workgroup_type_t wl_type =
			_os_workgroup_lookup_type_from_workload_id(workload_id);
	if (_os_workgroup_type_is_default_type(wl_type)) {
		/* Unknown workload ID, nothing to match */
		return true;
	}
	/* Require matching workinterval types between workload ID and passed in
	 * type of port or workgroup object. */
	if (_wg_type_to_wi_type(wl_type) != wi_type) {
		return false;
	}
	return true;
}

static inline bool
_os_workgroup_join_token_initialized(os_workgroup_join_token_t token)
{
	return (token->sig == _OS_WORKGROUP_JOIN_TOKEN_SIG_INIT);
}

static inline void
_os_workgroup_set_name(os_workgroup_t wg, const char *name)
{
	if (name) {
		const char *tmp = _dispatch_strdup_if_mutable(name);
		if (tmp != name) {
			wg->wg_state |= OS_WORKGROUP_LABEL_NEEDS_FREE;
			name = tmp;
		}
	}
	wg->name = name;
}

static inline bool
_os_workgroup_client_attr_is_valid(os_workgroup_attr_t attr)
{
	return (attr && _os_workgroup_client_attr_initialized(attr));
}

static inline os_workgroup_attr_t
_os_workgroup_client_attr_resolve(os_workgroup_attr_t attr,
		os_workgroup_attr_t client_attr,
		const os_workgroup_attr_s *default_attr)
{
	if (client_attr == NULL) {
		*attr = *default_attr;
	} else {
		if (!_os_workgroup_client_attr_is_valid(client_attr)) {
			return NULL;
		}

		// Make a local copy of the attr
		*attr = *client_attr;

		switch (attr->sig) {
			case _OS_WORKGROUP_ATTR_SIG_DEFAULT_INIT:
				/* For any fields which are 0, we fill in with default values */
				if (attr->wg_attr_flags == 0) {
					attr->wg_attr_flags = default_attr->wg_attr_flags;
				}
				if (attr->wg_type == 0) {
					attr->wg_type = default_attr->wg_type;
				}
				break;
			case _OS_WORKGROUP_ATTR_SIG_EMPTY_INIT:
				/* Nothing to do, the client built the attr up from scratch */
				break;
			default:
				return NULL;
		}

		/* Mark it as resolved */
		attr->sig = _OS_WORKGROUP_ATTR_RESOLVED_INIT;
	}

	os_assert(_os_workgroup_attr_is_resolved(attr));
	return attr;
}

static inline bool
_start_time_is_in_past(os_clockid_t clock, uint64_t start)
{
	switch (clock) {
		case OS_CLOCK_MACH_ABSOLUTE_TIME:
			return start <= mach_absolute_time();
	}
}

struct os_workgroup_pthread_ctx_s {
	os_workgroup_t wg;
	void *(*start_routine)(void *);
	void *arg;
};

static void *
_os_workgroup_pthread_start(void *wrapper_arg)
{
	struct os_workgroup_pthread_ctx_s *ctx = wrapper_arg;
	os_workgroup_t wg = ctx->wg;
	void *(*start_routine)(void *) = ctx->start_routine;
	void *arg = ctx->arg;

	free(ctx);

	os_workgroup_join_token_s token;
	int rc = os_workgroup_join(wg, &token);
	if (rc != 0) {
		DISPATCH_CLIENT_CRASH(rc, "pthread_start os_workgroup_join failed");
	}

	void *result = start_routine(arg);

	os_workgroup_leave(wg, &token);
	os_workgroup_dec_refcount(wg);

	return result;
}

static int
_os_workgroup_pthread_create_with_workgroup(pthread_t *thread,
		os_workgroup_t wg, const pthread_attr_t *attr,
		void *(*start_routine)(void *), void *arg)
{
	struct os_workgroup_pthread_ctx_s *ctx = _dispatch_calloc(1, sizeof(*ctx));

	os_workgroup_inc_refcount(wg);

	ctx->wg = wg;
	ctx->start_routine = start_routine;
	ctx->arg = arg;

	int rc = pthread_create(thread, attr, _os_workgroup_pthread_start, ctx);
	if (rc != 0) {
		os_workgroup_dec_refcount(wg);
		free(ctx);
	}

	return rc;
}

static const struct pthread_workgroup_functions_s _os_workgroup_pthread_functions = {
	.pwgf_version = PTHREAD_WORKGROUP_FUNCTIONS_VERSION,
	.pwgf_create_with_workgroup = _os_workgroup_pthread_create_with_workgroup,
};

void
_workgroup_init(void)
{
	pthread_install_workgroup_functions_np(&_os_workgroup_pthread_functions);
}

#pragma mark Private functions

int
os_workgroup_attr_set_interval_type(os_workgroup_attr_t attr,
		os_workgroup_interval_type_t interval_type)
{
	int ret = 0;
	if (_os_workgroup_client_attr_is_valid(attr) &&
		 _os_workgroup_type_is_interval_type(interval_type)) {
		attr->wg_type = interval_type;
	} else {
		ret = EINVAL;
	}
	return ret;
}

int
os_workgroup_attr_set_flags(os_workgroup_attr_t attr,
		os_workgroup_attr_flags_t flags)
{
	int ret = 0;
	if (_os_workgroup_client_attr_is_valid(attr)) {
		attr->wg_attr_flags = flags;
	} else {
		ret = EINVAL;
	}

	return ret;
}

os_workgroup_t
os_workgroup_interval_copy_current_4AudioToolbox(void)
{
	os_workgroup_t wg = _os_workgroup_get_current();

	if (wg) {
		if (_os_workgroup_type_is_audio_type(wg->wg_type)) {
			wg = os_retain(wg);
		} else {
			wg = NULL;
		}
	}

	return wg;
}

#pragma mark Public functions

os_workgroup_t
os_workgroup_create(const char *name, os_workgroup_attr_t attr)
{
	os_workgroup_t wg = NULL;
	work_interval_t wi = NULL;

	/* Resolve the input attributes */
	os_workgroup_attr_s wga;
	attr = _os_workgroup_client_attr_resolve(&wga, attr,
			&_os_workgroup_attr_default);
	if (attr == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* Do some sanity checks */
	if (!_os_workgroup_type_is_default_type(attr->wg_type)) {
		errno = EINVAL;
		return NULL;
	}

	/* We don't support propagating workgroups yet */
	if (_os_workgroup_attr_is_propagating(attr)) {
		errno = ENOTSUP;
		return NULL;
	}

	wi = _os_workgroup_create_work_interval(attr);
	if (wi == NULL) {
		return NULL;
	}

	wg = (os_workgroup_t) _os_object_alloc(WORKGROUP_CLASS,
			sizeof(struct os_workgroup_s));
	wg->wi = wi;
	wg->wg_state = OS_WORKGROUP_OWNER;
	wg->wg_type = attr->wg_type;

	_os_workgroup_set_name(wg, name);

	return wg;
}

os_workgroup_interval_t
os_workgroup_interval_create(const char *name, os_clockid_t clock,
		os_workgroup_attr_t attr)
{
	os_workgroup_interval_t wgi = NULL;
	work_interval_t wi = NULL;

	/* Resolve the input attributes */
	os_workgroup_attr_s wga;
	attr = _os_workgroup_client_attr_resolve(&wga, attr,
			&_os_workgroup_interval_attr_default);
	if (attr == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* Do some sanity checks */
	if (!_os_workgroup_type_is_interval_type(attr->wg_type)) {
		errno = EINVAL;
		return NULL;
	}

	if (!_os_workgroup_attr_is_differentiated(attr)) {
		errno = EINVAL;
		return NULL;
	}

	/* We don't support propagating workgroup yet */
	if (_os_workgroup_attr_is_propagating(attr)) {
		errno = ENOTSUP;
		return NULL;
	}

	wi = _os_workgroup_create_work_interval(attr);
	if (wi == NULL) {
		return NULL;
	}

	wgi = (os_workgroup_interval_t) _os_object_alloc(WORKGROUP_INTERVAL_CLASS,
			sizeof(struct os_workgroup_interval_s));
	wgi->wi = wi;
	wgi->clock = clock;
	wgi->wii = work_interval_instance_alloc(wi);
	wgi->wii_lock = OS_UNFAIR_LOCK_INIT;
	wgi->wg_type = attr->wg_type;
	wgi->wg_state = OS_WORKGROUP_OWNER;

	_os_workgroup_set_name(wgi->_as_wg, name);

	return wgi;
}

os_workgroup_t
os_workgroup_create_with_workload_id(const char * name,
		const char *workload_id, os_workgroup_attr_t attr)
{
	os_workgroup_t wg = NULL;
	work_interval_t wi = NULL;

	const os_workgroup_attr_s *default_attr =
			&_os_workgroup_with_workload_id_attr_default;

	/* Resolve the input attributes */
	os_workgroup_attr_s wga;
	attr = _os_workgroup_client_attr_resolve(&wga, attr, default_attr);
	if (attr == NULL) {
		_os_workgroup_error_log("Invalid attribute pointer");
		errno = EINVAL;
		return NULL;
	}

	/* Resolve workload ID */
	attr = _os_workgroup_workload_id_attr_resolve(workload_id, attr,
			default_attr);
	if (attr == NULL) {
		_os_workgroup_error_log("Mismatched workload ID and attribute "
				"interval type: %s vs %hd", workload_id, wga.wg_type);
		errno = EINVAL;
		return NULL;
	}

	/* Require default attribute flags. */
	if (attr->wg_attr_flags != default_attr->wg_attr_flags) {
		_os_workgroup_error_log("Non-default attribute flags: 0x%x",
				attr->wg_attr_flags);
		errno = EINVAL;
		return NULL;
	}

	/* Do some sanity checks */
	if (!_os_workgroup_type_is_default_type(attr->wg_type)) {
		_os_workgroup_error_log("Non-default workload type: %s (%hd)",
				workload_id, attr->wg_type);
		errno = EINVAL;
		return NULL;
	}

	/* We don't support propagating workgroups yet */
	if (_os_workgroup_attr_is_propagating(attr)) {
		_os_workgroup_error_log("Unsupported attribute flags: 0x%x",
				attr->wg_attr_flags);
		errno = ENOTSUP;
		return NULL;
	}

	wi = _os_workgroup_create_work_interval(attr);
	if (wi == NULL) {
		return NULL;
	}

	wg = (os_workgroup_t) _os_object_alloc(WORKGROUP_CLASS,
			sizeof(struct os_workgroup_s));
	wg->wi = wi;
	wg->wg_state = OS_WORKGROUP_OWNER;
	wg->wg_type = attr->wg_type;

	_os_workgroup_set_name(wg, name);

	return wg;
}

os_workgroup_interval_t
os_workgroup_interval_create_with_workload_id(const char *name,
		const char *workload_id, os_clockid_t clock, os_workgroup_attr_t attr)
{
	os_workgroup_interval_t wgi = NULL;
	work_interval_t wi = NULL;

	const os_workgroup_attr_s *default_attr =
			&_os_workgroup_interval_attr_default;

	/* Resolve the input attributes */
	os_workgroup_attr_s wga;
	attr = _os_workgroup_client_attr_resolve(&wga, attr, default_attr);
	if (attr == NULL) {
		_os_workgroup_error_log("Invalid attribute pointer");
		errno = EINVAL;
		return NULL;
	}

	/* Resolve workload ID */
	attr = _os_workgroup_workload_id_attr_resolve(workload_id, attr,
			default_attr);
	if (attr == NULL) {
		_os_workgroup_error_log("Mismatched workload ID and attribute "
				"interval type: %s vs %hd", workload_id, wga.wg_type);
		errno = EINVAL;
		return NULL;
	}

	/* Require default attribute flags. */
	if (attr->wg_attr_flags != default_attr->wg_attr_flags) {
		_os_workgroup_error_log("Non-default attribute flags: 0x%x",
				attr->wg_attr_flags);
		errno = EINVAL;
		return NULL;
	}

	/* Do some sanity checks */
	if (!_os_workgroup_type_is_interval_type(attr->wg_type)) {
		_os_workgroup_error_log("Invalid workload interval type: %s (%hd)",
				workload_id, attr->wg_type);
		errno = EINVAL;
		return NULL;
	}

	if (!_os_workgroup_attr_is_differentiated(attr)) {
		_os_workgroup_error_log("Invalid attribute flags: 0x%x",
				attr->wg_attr_flags);
		errno = EINVAL;
		return NULL;
	}

	/* We don't support propagating workgroup yet */
	if (_os_workgroup_attr_is_propagating(attr)) {
		_os_workgroup_error_log("Unsupported attribute flags: 0x%x",
				attr->wg_attr_flags);
		errno = ENOTSUP;
		return NULL;
	}

	wi = _os_workgroup_create_work_interval(attr);
	if (wi == NULL) {
		return NULL;
	}

	wgi = (os_workgroup_interval_t) _os_object_alloc(WORKGROUP_INTERVAL_CLASS,
			sizeof(struct os_workgroup_interval_s));
	wgi->wi = wi;
	wgi->clock = clock;
	wgi->wii = work_interval_instance_alloc(wi);
	wgi->wii_lock = OS_UNFAIR_LOCK_INIT;
	wgi->wg_type = attr->wg_type;
	wgi->wg_state = OS_WORKGROUP_OWNER;

	_os_workgroup_set_name(wgi->_as_wg, name);

	return wgi;
}

int
os_workgroup_join_self(os_workgroup_t wg, os_workgroup_join_token_t token,
		os_workgroup_index * __unused id_out)
{
	return os_workgroup_join(wg, token);
}

void
os_workgroup_leave_self(os_workgroup_t wg, os_workgroup_join_token_t token)
{
	return os_workgroup_leave(wg, token);
}

#pragma mark Public functions

os_workgroup_parallel_t
os_workgroup_parallel_create(const char *name, os_workgroup_attr_t attr)
{
	os_workgroup_parallel_t wgp = NULL;

	// Clients should only specify NULL attributes.
	os_workgroup_attr_s wga;
	if (attr == NULL) {
		wga = _os_workgroup_parallel_attr_default;
		attr = &wga;
	} else {
		// Make a local copy of the attr
		if (!_os_workgroup_client_attr_is_valid(attr)) {
			errno = EINVAL;
			return NULL;
		}

		wga = *attr;
		attr = &wga;

		switch (attr->sig) {
			case _OS_WORKGROUP_ATTR_SIG_DEFAULT_INIT:
			{
				/* For any fields which are 0, we fill in with default values */
				if (attr->wg_attr_flags == 0) {
					attr->wg_attr_flags = _os_workgroup_parallel_attr_default.wg_attr_flags;
				}
				if (attr->wg_type == 0) {
					attr->wg_type = _os_workgroup_parallel_attr_default.wg_type;
				}
			}
			// Fallthrough
			case _OS_WORKGROUP_ATTR_SIG_EMPTY_INIT:
				break;
			default:
				errno = EINVAL;
				return NULL;
		}
		/* Mark it as resolved */
		attr->sig = _OS_WORKGROUP_ATTR_RESOLVED_INIT;
	}

	os_assert(_os_workgroup_attr_is_resolved(attr));

	/* Do some sanity checks */
	if (!_os_workgroup_type_is_parallel_type(attr->wg_type)) {
		errno = EINVAL;
		return NULL;
	}

	/* We don't support propagating workgroups yet */
	if (_os_workgroup_attr_is_propagating(attr)) {
		errno = ENOTSUP;
		return NULL;
	}

	wgp = (os_workgroup_t) _os_object_alloc(WORKGROUP_PARALLEL_CLASS,
			sizeof(struct os_workgroup_parallel_s));
	wgp->wi = NULL;
	wgp->wg_state = OS_WORKGROUP_OWNER;
	wgp->wg_type = attr->wg_type;

	_os_workgroup_set_name(wgp, name);

	return wgp;
}

int
os_workgroup_copy_port(os_workgroup_t wg, mach_port_t *mach_port_out)
{
	os_assert(wg != NULL);
	os_assert(mach_port_out != NULL);

	*mach_port_out = MACH_PORT_NULL;
	int rv = 0;

	uint64_t wg_state = os_atomic_load(&wg->wg_state, relaxed);
	if (wg_state & OS_WORKGROUP_CANCELED) {
		return EINVAL;
	}

	if (!_os_workgroup_has_backing_workinterval(wg)) {
		return EINVAL;
	}

	if (_os_workgroup_is_configurable(wg_state)) {
		rv = work_interval_copy_port(wg->wi, mach_port_out);
		if (rv < 0) {
			rv = errno;
		}
		return rv;
	}

	kern_return_t kr = mach_port_mod_refs(mach_task_self(), wg->port,
			MACH_PORT_RIGHT_SEND, 1);
	os_assumes(kr == KERN_SUCCESS);
	*mach_port_out = wg->port;
	return rv;
}

os_workgroup_t
os_workgroup_create_with_port(const char *name, mach_port_t port)
{
	if (!MACH_PORT_VALID(port)) {
		errno = EINVAL;
		return NULL;
	}

	os_workgroup_type_t wg_type;
	int ret = _os_workgroup_get_wg_wi_types_from_port(port, &wg_type, NULL);
	if (ret != 0) {
		return NULL;
	}

	os_workgroup_t wg = NULL;
	wg = (os_workgroup_t) _os_object_alloc(WORKGROUP_CLASS,
			sizeof(struct os_workgroup_s));
	_os_workgroup_set_name(wg, name);

	kern_return_t kr;
	kr = mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);
	os_assumes(kr == KERN_SUCCESS);
	wg->port = port;
	wg->wg_type = wg_type;

	return wg;
}

os_workgroup_t
os_workgroup_create_with_workload_id_and_port(const char *name,
		const char *workload_id, mach_port_t port)
{
	if (!MACH_PORT_VALID(port)) {
		_os_workgroup_error_log("Invalid mach port 0x%x", port);
		errno = EINVAL;
		return NULL;
	}

	os_workgroup_type_t wg_type;
	uint32_t wi_type;
	int ret = _os_workgroup_get_wg_wi_types_from_port(port, &wg_type, &wi_type);
	if (ret != 0) {
		_os_workgroup_error_log("Invalid mach port 0x%x", port);
		return NULL;
	}

	/* Validate workload ID is compatible with port workinterval type */
	if (!_os_workgroup_workload_id_is_valid_for_wi_type(workload_id, wi_type)) {
		_os_workgroup_error_log("Mismatched workload ID and port "
				"interval type: %s vs %hd", workload_id, wg_type);
		errno = EINVAL;
		return NULL;
	}

	os_workgroup_t wg = NULL;
	wg = (os_workgroup_t) _os_object_alloc(WORKGROUP_CLASS,
			sizeof(struct os_workgroup_s));
	_os_workgroup_set_name(wg, name);

	kern_return_t kr;
	kr = mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);
	os_assumes(kr == KERN_SUCCESS);
	wg->port = port;
	wg->wg_type = wg_type;

	return wg;
}

os_workgroup_t
os_workgroup_create_with_workgroup(const char *name, os_workgroup_t wg)
{
	uint64_t wg_state = os_atomic_load(&wg->wg_state, relaxed);
	if (wg_state & OS_WORKGROUP_CANCELED) {
		errno = EINVAL;
		return NULL;
	}

	os_workgroup_t new_wg = NULL;

	new_wg = (os_workgroup_t) _os_object_alloc(WORKGROUP_CLASS,
			sizeof(struct os_workgroup_s));
	_os_workgroup_set_name(new_wg, name);
	new_wg->wg_type = wg->wg_type;

	/* We intentionally don't copy the context */

	if (_os_workgroup_has_backing_workinterval(wg)) {

		if (_os_workgroup_is_configurable(wg_state)) {
			int rv = work_interval_copy_port(wg->wi, &new_wg->port);

			if (rv < 0) {
				goto error;
			}
		} else {
			kern_return_t kr;
			kr = mach_port_mod_refs(mach_task_self(), wg->port, MACH_PORT_RIGHT_SEND, 1);

			if (kr != KERN_SUCCESS) {
				goto error;
			}
			new_wg->port = wg->port;
		}
	}

	return new_wg;

error:
	wg_state = os_atomic_load(&new_wg->wg_state, relaxed);
	if (wg_state & OS_WORKGROUP_LABEL_NEEDS_FREE) {
		free((void *)new_wg->name);
	}
	free(new_wg);

	return NULL;
}

os_workgroup_t
os_workgroup_create_with_workload_id_and_workgroup(const char *name,
		const char *workload_id, os_workgroup_t wg)
{
	uint64_t wg_state = os_atomic_load(&wg->wg_state, relaxed);
	if (wg_state & OS_WORKGROUP_CANCELED) {
		_os_workgroup_error_log("Workgroup already cancelled");
		errno = EINVAL;
		return NULL;
	}

	/* Validate workload ID is compatible with workgroup workinterval type */
	if (!_os_workgroup_workload_id_is_valid_for_wi_type(workload_id,
			_wg_type_to_wi_type(wg->wg_type))) {
		_os_workgroup_error_log("Mismatched workload ID and workgroup "
				"interval type: %s vs %hd", workload_id, wg->wg_type);
		errno = EINVAL;
		return NULL;
	}

	os_workgroup_t new_wg = NULL;

	new_wg = (os_workgroup_t) _os_object_alloc(WORKGROUP_CLASS,
			sizeof(struct os_workgroup_s));
	_os_workgroup_set_name(new_wg, name);
	new_wg->wg_type = wg->wg_type;

	/* We intentionally don't copy the context */

	if (_os_workgroup_has_backing_workinterval(wg)) {

		if (_os_workgroup_is_configurable(wg_state)) {
			int rv = work_interval_copy_port(wg->wi, &new_wg->port);

			if (rv < 0) {
				_os_workgroup_error_log("Invalid workgroup work_interval");
				goto error;
			}
		} else {
			kern_return_t kr;
			kr = mach_port_mod_refs(mach_task_self(), wg->port, MACH_PORT_RIGHT_SEND, 1);

			if (kr != KERN_SUCCESS) {
				_os_workgroup_error_log("Invalid workgroup port 0x%x", wg->port);
				goto error;
			}
			new_wg->port = wg->port;
		}
	}

	return new_wg;

error:
	wg_state = os_atomic_load(&new_wg->wg_state, relaxed);
	if (wg_state & OS_WORKGROUP_LABEL_NEEDS_FREE) {
		free((void *)new_wg->name);
	}
	free(new_wg);

	return NULL;
}

int
os_workgroup_max_parallel_threads(os_workgroup_t wg, os_workgroup_mpt_attr_t __unused attr)
{
	os_assert(wg != NULL);

	qos_class_t qos = QOS_CLASS_USER_INTERACTIVE;

	switch (wg->wg_type) {
	case OS_WORKGROUP_INTERVAL_TYPE_COREAUDIO:
	case OS_WORKGROUP_INTERVAL_TYPE_AUDIO_CLIENT:
		return pthread_time_constraint_max_parallelism(0);
	default:
		return pthread_qos_max_parallelism(qos, 0);
	}
}

int
os_workgroup_join(os_workgroup_t wg, os_workgroup_join_token_t token)
{
	os_workgroup_t cur_wg = _os_workgroup_get_current();
	if (cur_wg) {
		// We currently don't allow joining multiple workgroups at all, period
		errno = EALREADY;
		return errno;
	}

	uint64_t wg_state = os_atomic_load(&wg->wg_state, relaxed);
	if (wg_state & OS_WORKGROUP_CANCELED) {
		errno = EINVAL;
		return errno;
	}

	int rv = 0;

	if (_os_workgroup_has_backing_workinterval(wg)) {
		if (_os_workgroup_is_configurable(wg_state)) {
			rv = work_interval_join(wg->wi);
		} else {
			rv = work_interval_join_port(wg->port);
		}
	}

	if (rv) {
		rv = errno;
		return rv;
	}

	os_atomic_inc(&wg->joined_cnt, relaxed);

	bzero(token, sizeof(struct os_workgroup_join_token_s));
	token->sig = _OS_WORKGROUP_JOIN_TOKEN_SIG_INIT;

	token->thread = _dispatch_thread_port();
	token->old_wg = cur_wg; /* should be null */
	token->new_wg = wg;

	_os_workgroup_set_current(wg);
	return rv;
}

void
os_workgroup_leave(os_workgroup_t wg, os_workgroup_join_token_t token)
{
	if (!_os_workgroup_join_token_initialized(token)) {
		os_crash("Join token is corrupt");
	}

	if (token->thread != _dispatch_thread_port()) {
		os_crash("Join token provided is for a different thread");
	}

	os_workgroup_t cur_wg = _os_workgroup_get_current();
	if ((token->new_wg != cur_wg) || (cur_wg != wg)) {
		os_crash("Join token provided is for a different workgroup than the "
				"last one joined by thread");
	}
	os_assert(token->old_wg == NULL);

	if (_os_workgroup_has_backing_workinterval(wg)) {
		dispatch_assume(work_interval_leave() == 0);
	}
	uint32_t old_joined_cnt = os_atomic_dec_orig(&wg->joined_cnt, relaxed);
	if (old_joined_cnt == 0) {
		DISPATCH_INTERNAL_CRASH(0, "Joined count underflowed");
	}
	_os_workgroup_set_current(NULL);
}

int
os_workgroup_set_working_arena(os_workgroup_t wg, void * _Nullable client_arena,
		uint32_t max_workers, os_workgroup_working_arena_destructor_t destructor)
{
	size_t arena_size;
	// We overflowed, we can't allocate this
	if (os_mul_and_add_overflow(sizeof(mach_port_t), max_workers, sizeof(struct os_workgroup_arena_s), &arena_size)) {
		errno = ENOMEM;
		return errno;
	}

	os_workgroup_arena_t wg_arena = calloc(arena_size, 1);
	if (wg_arena == NULL) {
		errno = ENOMEM;
		return errno;
	}
	wg_arena->max_workers = max_workers;
	wg_arena->client_arena = client_arena;
	wg_arena->destructor = destructor;

	_os_workgroup_atomic_flags old_state, new_state;
	os_workgroup_arena_t old_arena = NULL;

	bool success = os_atomic_rmw_loop(&wg->wg_atomic_flags, old_state, new_state, relaxed, {
		if (_wg_joined_cnt(old_state) > 0) { // We can't change the arena while it is in use
			os_atomic_rmw_loop_give_up(break);
		}
		old_arena = _wg_arena(old_state);

		// Remove the old arena and put the new one in
		new_state = old_state;
		new_state &= ~OS_WORKGROUP_ARENA_MASK;
		new_state |= (uint64_t) wg_arena;
	});

	if (!success) {
		free(wg_arena);
		errno = EBUSY;
		return errno;
	}

	if (old_arena) {
		old_arena->destructor(old_arena->client_arena);
		free(old_arena);
	}

	return 0;
}

void *
os_workgroup_get_working_arena(os_workgroup_t wg, os_workgroup_index *_Nullable index_out)
{
	if (_os_workgroup_get_current() != wg) {
		os_crash("Thread is not a member of the workgroup");
	}

	/* At this point, we know that since this thread is a member of the wg, we
	 * won't have the arena replaced out from under us so we can modify it
	 * safely */
	dispatch_assert(wg->joined_cnt > 0);

	os_workgroup_arena_t arena = os_atomic_load(&wg->wg_arena, relaxed);
	if (arena == NULL) {
		return NULL;
	}

	/* if the max_workers was 0 and the client wants an index, then they will
	 * fail */
	if (index_out != NULL && arena->max_workers == 0) {
		os_crash("The arena associated with workgroup is not to be partitioned");
	}

	if (index_out) {
		/* Find the index of the current thread in the arena */
		uint32_t found_index = 0;
		bool found = false;
		for (uint32_t i = 0; i < arena->max_workers; i++) {
			if (arena->arena_indices[i] == _dispatch_thread_port()) {
				found_index = i;
				found = true;
				break;
			}
		}

		if (!found) {
			/* Current thread doesn't already have an index, give it one */
			found_index = os_atomic_inc_orig(&arena->next_worker_index, relaxed);

			if (found_index >= arena->max_workers) {
				os_crash("Exceeded the maximum number of workers who can access the arena");
			}
			arena->arena_indices[found_index] = _dispatch_thread_port();
		}

		*index_out = found_index;
	}

	return arena->client_arena;
}

void
os_workgroup_cancel(os_workgroup_t wg)
{
	os_atomic_or(&wg->wg_state, OS_WORKGROUP_CANCELED, relaxed);
}

bool
os_workgroup_testcancel(os_workgroup_t wg)
{
	return os_atomic_load(&wg->wg_state, relaxed) & OS_WORKGROUP_CANCELED;
}

int
os_workgroup_interval_start(os_workgroup_interval_t wgi, uint64_t start,
		uint64_t deadline, os_workgroup_interval_data_t __unused data)
{
	os_workgroup_t cur_wg = _os_workgroup_get_current();
	if (cur_wg != wgi->_as_wg) {
		os_crash("Thread is not a member of the workgroup");
	}

	if (deadline < start || (!_start_time_is_in_past(wgi->clock, start))) {
		errno = EINVAL;
		return errno;
	}

	bool success = os_unfair_lock_trylock(&wgi->wii_lock);
	if (!success) {
		// Someone else is concurrently in a start, update or finish method. We
		// can't make progress here
		errno = EBUSY;
		return errno;
	}

	int rv = 0;
	uint64_t old_state, new_state;
	os_atomic_rmw_loop(&wgi->wg_state, old_state, new_state, relaxed, {
		if (old_state & (OS_WORKGROUP_CANCELED | OS_WORKGROUP_INTERVAL_STARTED)) {
			rv = EINVAL;
			os_atomic_rmw_loop_give_up(break);
		}
		if (!_os_workgroup_is_configurable(old_state)) {
			rv = EPERM;
			os_atomic_rmw_loop_give_up(break);
		}
		new_state = old_state | OS_WORKGROUP_INTERVAL_STARTED;
	});

	if (rv) {
		os_unfair_lock_unlock(&wgi->wii_lock);
		errno = rv;
		return rv;
	}

	work_interval_instance_t wii = wgi->wii;
	work_interval_instance_clear(wii);

	work_interval_instance_set_start(wii, start);
	work_interval_instance_set_deadline(wii, deadline);
	rv = work_interval_instance_start(wii);
	if (rv != 0) {
		/* If we failed to start the interval in the kernel, clear the started
		 * field */
		os_atomic_and(&wgi->wg_state, ~OS_WORKGROUP_INTERVAL_STARTED, relaxed);
	}

	os_unfair_lock_unlock(&wgi->wii_lock);

	return rv;
}

int
os_workgroup_interval_update(os_workgroup_interval_t wgi, uint64_t deadline,
		os_workgroup_interval_data_t __unused data)
{
	os_workgroup_t cur_wg = _os_workgroup_get_current();
	if (cur_wg != wgi->_as_wg) {
		os_crash("Thread is not a member of the workgroup");
	}

	bool success = os_unfair_lock_trylock(&wgi->wii_lock);
	if (!success) {
		// Someone else is concurrently in a start, update or finish method. We
		// can't make progress here
		errno = EBUSY;
		return errno;
	}

	uint64_t wg_state = os_atomic_load(&wgi->wg_state, relaxed);
	if (!_os_workgroup_is_configurable(wg_state)) {
		os_unfair_lock_unlock(&wgi->wii_lock);
		errno = EPERM;
		return errno;
	}

	/* Note: We allow updating and finishing an workgroup_interval that has
	 * already started even if the workgroup has been cancelled - since
	 * cancellation happens asynchronously and doesn't care about ongoing
	 * intervals. However a subsequent new interval cannot be started */
	if (!(wg_state & OS_WORKGROUP_INTERVAL_STARTED)) {
		os_unfair_lock_unlock(&wgi->wii_lock);
		errno = EINVAL;
		return errno;
	}

	work_interval_instance_t wii = wgi->wii;
	work_interval_instance_set_deadline(wii, deadline);
	int rv = work_interval_instance_update(wii);
	if (rv != 0) {
		rv = errno;
	}

	os_unfair_lock_unlock(&wgi->wii_lock);
	return rv;
}

int
os_workgroup_interval_finish(os_workgroup_interval_t wgi,
		os_workgroup_interval_data_t __unused data)
{
	os_workgroup_t cur_wg = _os_workgroup_get_current();
	if (cur_wg != wgi->_as_wg) {
		os_crash("Thread is not a member of the workgroup");
	}

	bool success = os_unfair_lock_trylock(&wgi->wii_lock);
	if (!success) {
		// Someone else is concurrently in a start, update or finish method. We
		// can't make progress here
		errno = EBUSY;
		return errno;
	}

	uint64_t wg_state = os_atomic_load(&wgi->wg_state, relaxed);
	if (!_os_workgroup_is_configurable(wg_state)) {
		os_unfair_lock_unlock(&wgi->wii_lock);
		errno = EPERM;
		return errno;
	}
	if (!(wg_state & OS_WORKGROUP_INTERVAL_STARTED)) {
		os_unfair_lock_unlock(&wgi->wii_lock);
		errno = EINVAL;
		return errno;
	}

	work_interval_instance_t wii = wgi->wii;
	uint64_t current_finish = 0;
	switch (wgi->clock) {
		case OS_CLOCK_MACH_ABSOLUTE_TIME:
			current_finish = mach_absolute_time();
			break;
	}

	work_interval_instance_set_finish(wii, current_finish);
	int rv = work_interval_instance_finish(wii);
	if (rv != 0) {
		rv = errno;
	} else {
		/* If we succeeded in finishing, clear the started bit */
		os_atomic_and(&wgi->wg_state, ~OS_WORKGROUP_INTERVAL_STARTED, relaxed);
	}

	os_unfair_lock_unlock(&wgi->wii_lock);
	return rv;
}
