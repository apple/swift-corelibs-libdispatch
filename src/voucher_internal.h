/*
 * Copyright (c) 2013 Apple Inc. All rights reserved.
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

#ifndef __DISPATCH_VOUCHER_INTERNAL__
#define __DISPATCH_VOUCHER_INTERNAL__

#ifndef __DISPATCH_INDIRECT__
#error "Please #include <dispatch/dispatch.h> instead of this file directly."
#include <dispatch/base.h> // for HeaderDoc
#endif

#pragma mark -
#pragma mark voucher_recipe_t (disabled)

#if VOUCHER_ENABLE_RECIPE_OBJECTS
/*!
 * @group Voucher Creation SPI
 * SPI intended for clients that need to create vouchers.
 */

#if OS_OBJECT_USE_OBJC
OS_OBJECT_DECL(voucher_recipe);
#else
typedef struct voucher_recipe_s *voucher_recipe_t;
#endif

/*!
 * @function voucher_create
 *
 * @abstract
 * Creates a new voucher object from a recipe.
 *
 * @discussion
 * Error handling TBD
 *
 * @result
 * The newly created voucher object.
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_EXPORT OS_OBJECT_RETURNS_RETAINED OS_WARN_RESULT OS_NOTHROW
voucher_t
voucher_create(voucher_recipe_t recipe);
#endif // VOUCHER_ENABLE_RECIPE_OBJECTS

#if VOUCHER_ENABLE_GET_MACH_VOUCHER
/*!
 * @function voucher_get_mach_voucher
 *
 * @abstract
 * Returns the mach voucher port underlying the specified voucher object.
 *
 * @discussion
 * The caller must either maintain a reference on the voucher object while the
 * returned mach voucher port is in use to ensure it stays valid for the
 * duration, or it must retain the mach voucher port with mach_port_mod_refs().
 *
 * @param voucher
 * The voucher object to query.
 *
 * @result
 * A mach voucher port.
 */
__OSX_AVAILABLE_STARTING(__MAC_10_10,__IPHONE_8_0)
OS_VOUCHER_EXPORT OS_WARN_RESULT OS_NOTHROW
mach_voucher_t
voucher_get_mach_voucher(voucher_t voucher);
#endif // VOUCHER_ENABLE_GET_MACH_VOUCHER

#pragma mark -
#pragma mark voucher_t

#if TARGET_IPHONE_SIMULATOR && \
		IPHONE_SIMULATOR_HOST_MIN_VERSION_REQUIRED < 101100
#undef VOUCHER_USE_MACH_VOUCHER
#define VOUCHER_USE_MACH_VOUCHER 0
#endif
#ifndef VOUCHER_USE_MACH_VOUCHER
#if __has_include(<mach/mach_voucher.h>)
#define VOUCHER_USE_MACH_VOUCHER 1
#endif
#endif

#if VOUCHER_USE_MACH_VOUCHER
#undef DISPATCH_USE_IMPORTANCE_ASSERTION
#define DISPATCH_USE_IMPORTANCE_ASSERTION 0
#else
#undef MACH_RCV_VOUCHER
#define MACH_RCV_VOUCHER 0
#endif // VOUCHER_USE_MACH_VOUCHER

void _voucher_init(void);
void _voucher_atfork_child(void);
void _voucher_activity_heap_pressure_warn(void);
void _voucher_activity_heap_pressure_normal(void);
void _voucher_xref_dispose(voucher_t voucher);
void _voucher_dispose(voucher_t voucher);
size_t _voucher_debug(voucher_t v, char* buf, size_t bufsiz);
void _voucher_thread_cleanup(void *voucher);
mach_voucher_t _voucher_get_mach_voucher(voucher_t voucher);
voucher_t _voucher_create_without_importance(voucher_t voucher);
voucher_t _voucher_create_accounting_voucher(voucher_t voucher);
mach_voucher_t _voucher_create_mach_voucher_with_priority(voucher_t voucher,
		pthread_priority_t priority);
voucher_t _voucher_create_with_priority_and_mach_voucher(voucher_t voucher,
		pthread_priority_t priority, mach_voucher_t kv);
void _voucher_dealloc_mach_voucher(mach_voucher_t kv);

#if OS_OBJECT_USE_OBJC
_OS_OBJECT_DECL_SUBCLASS_INTERFACE(voucher, object)
#if VOUCHER_ENABLE_RECIPE_OBJECTS
_OS_OBJECT_DECL_SUBCLASS_INTERFACE(voucher_recipe, object)
#endif
#endif

voucher_t voucher_retain(voucher_t voucher);
void voucher_release(voucher_t voucher);

#define _TAILQ_IS_ENQUEUED(elm, field) \
		((elm)->field.tqe_prev != NULL)
#define _TAILQ_MARK_NOT_ENQUEUED(elm, field) \
		do { (elm)->field.tqe_prev = NULL; } while (0)

#define VOUCHER_NO_MACH_VOUCHER MACH_PORT_DEAD

#if VOUCHER_USE_MACH_VOUCHER

#if DISPATCH_DEBUG
#define DISPATCH_VOUCHER_DEBUG 1
#define DISPATCH_VOUCHER_ACTIVITY_DEBUG 1
#endif

typedef struct voucher_s {
	_OS_OBJECT_HEADER(
	void *os_obj_isa,
	os_obj_ref_cnt,
	os_obj_xref_cnt);
	TAILQ_ENTRY(voucher_s) v_list;
	mach_voucher_t v_kvoucher, v_ipc_kvoucher; // if equal, only one reference
	voucher_t v_kvbase; // if non-NULL, v_kvoucher is a borrowed reference
	struct _voucher_atm_s *v_atm;
	struct _voucher_activity_s *v_activity;
#if VOUCHER_ENABLE_RECIPE_OBJECTS
	size_t v_recipe_extra_offset;
	mach_voucher_attr_recipe_size_t v_recipe_extra_size;
#endif
	unsigned int v_has_priority:1;
	unsigned int v_activities;
	mach_voucher_attr_recipe_data_t v_recipes[];
} voucher_s;

#if VOUCHER_ENABLE_RECIPE_OBJECTS
typedef struct voucher_recipe_s {
	_OS_OBJECT_HEADER(
	const _os_object_class_s *os_obj_isa,
	os_obj_ref_cnt,
	os_obj_xref_cnt);
	size_t vr_allocation_size;
	mach_voucher_attr_recipe_size_t volatile vr_size;
	mach_voucher_attr_recipe_t vr_data;
} voucher_recipe_s;
#endif

#define _voucher_recipes_base(r) (r[0])
#define _voucher_recipes_atm(r) (r[1])
#define _voucher_recipes_bits(r) (r[2])
#define _voucher_base_recipe(v) (_voucher_recipes_base((v)->v_recipes))
#define _voucher_atm_recipe(v) (_voucher_recipes_atm((v)->v_recipes))
#define _voucher_bits_recipe(v) (_voucher_recipes_bits((v)->v_recipes))
#define _voucher_recipes_size() (3 * sizeof(mach_voucher_attr_recipe_data_t))

#if TARGET_OS_EMBEDDED
#define VL_HASH_SIZE  64u // must be a power of two
#else
#define VL_HASH_SIZE 256u // must be a power of two
#endif
#define VL_HASH(kv) (MACH_PORT_INDEX(kv) & (VL_HASH_SIZE - 1))

typedef uint32_t _voucher_magic_t;
const _voucher_magic_t _voucher_magic_v1 = 0x0190cefa; // little-endian FACE9001
#define _voucher_recipes_magic(r) ((_voucher_magic_t*) \
		(_voucher_recipes_bits(r).content))
#define _voucher_magic(v) _voucher_recipes_magic((v)->v_recipes)
typedef uint32_t _voucher_priority_t;
#define _voucher_recipes_priority(r) ((_voucher_priority_t*) \
		(_voucher_recipes_bits(r).content + sizeof(_voucher_magic_t)))
#define _voucher_priority(v) _voucher_recipes_priority((v)->v_recipes)
#define _voucher_activity_ids(v) ((voucher_activity_id_t*) \
		(_voucher_bits_recipe(v).content + sizeof(_voucher_magic_t) + \
		sizeof(_voucher_priority_t)))
#define _voucher_bits_size(activities) \
		(sizeof(_voucher_magic_t) + sizeof(_voucher_priority_t) + \
		(activities) * sizeof(voucher_activity_id_t))

#if VOUCHER_ENABLE_RECIPE_OBJECTS
#define _voucher_extra_size(v) ((v)->v_recipe_extra_size)
#define _voucher_extra_recipes(v) ((char*)(v) + (v)->v_recipe_extra_offset)
#else
#define _voucher_extra_size(v) 0
#define _voucher_extra_recipes(v) NULL
#endif

#if DISPATCH_DEBUG && DISPATCH_VOUCHER_DEBUG
#define _dispatch_voucher_debug(msg, v, ...) \
		_dispatch_debug("voucher[%p]: " msg, v, ##__VA_ARGS__)
#define _dispatch_kvoucher_debug(msg, kv, ...) \
		_dispatch_debug("kvoucher[0x%08x]: " msg, kv, ##__VA_ARGS__)
#define _dispatch_voucher_debug_machport(name) \
		dispatch_debug_machport((name), __func__)
#else
#define _dispatch_voucher_debug(msg, v, ...)
#define _dispatch_kvoucher_debug(msg, kv, ...)
#define _dispatch_voucher_debug_machport(name) ((void)(name))
#endif

#if !(USE_OBJC && __OBJC2__) && !defined(__cplusplus)

DISPATCH_ALWAYS_INLINE
static inline voucher_t
_voucher_retain(voucher_t voucher)
{
#if !DISPATCH_VOUCHER_OBJC_DEBUG
	// not using _os_object_refcnt* because we don't need barriers:
	// vouchers are immutable and are in a hash table with a lock
	int xref_cnt = dispatch_atomic_inc2o(voucher, os_obj_xref_cnt, relaxed);
	_dispatch_voucher_debug("retain  -> %d", voucher, xref_cnt + 1);
	if (slowpath(xref_cnt <= 0)) {
		_dispatch_voucher_debug("resurrection", voucher);
		DISPATCH_CRASH("Voucher resurrection");
	}
#else
	os_retain(voucher);
	_dispatch_voucher_debug("retain  -> %d", voucher,
			voucher->os_obj_xref_cnt + 1);
#endif // DISPATCH_DEBUG
	return voucher;
}

DISPATCH_ALWAYS_INLINE
static inline void
_voucher_release(voucher_t voucher)
{
#if !DISPATCH_VOUCHER_OBJC_DEBUG
	// not using _os_object_refcnt* because we don't need barriers:
	// vouchers are immutable and are in a hash table with a lock
	int xref_cnt = dispatch_atomic_dec2o(voucher, os_obj_xref_cnt, relaxed);
	_dispatch_voucher_debug("release -> %d", voucher, xref_cnt + 1);
	if (fastpath(xref_cnt >= 0)) {
		return;
	}
	if (slowpath(xref_cnt < -1)) {
		_dispatch_voucher_debug("overrelease", voucher);
		DISPATCH_CRASH("Voucher overrelease");
	}
	return _os_object_xref_dispose((_os_object_t)voucher);
#else
	_dispatch_voucher_debug("release -> %d", voucher, voucher->os_obj_xref_cnt);
	return os_release(voucher);
#endif // DISPATCH_DEBUG
}

DISPATCH_ALWAYS_INLINE
static inline voucher_t
_voucher_get(void)
{
	return _dispatch_thread_getspecific(dispatch_voucher_key);
}

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline voucher_t
_voucher_copy(void)
{
	voucher_t voucher = _voucher_get();
	if (voucher) _voucher_retain(voucher);
	return voucher;
}

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline voucher_t
_voucher_copy_without_importance(void)
{
	voucher_t voucher = _voucher_get();
	if (voucher) voucher = _voucher_create_without_importance(voucher);
	return voucher;
}

DISPATCH_ALWAYS_INLINE
static inline void
_voucher_mach_voucher_set(mach_voucher_t kv)
{
	if (kv == VOUCHER_NO_MACH_VOUCHER) return;
	_dispatch_set_priority_and_mach_voucher(0, kv);
}

DISPATCH_ALWAYS_INLINE
static inline mach_voucher_t
_voucher_swap_and_get_mach_voucher(voucher_t ov, voucher_t voucher)
{
	if (ov == voucher) return VOUCHER_NO_MACH_VOUCHER;
	_dispatch_voucher_debug("swap from voucher[%p]", voucher, ov);
	_dispatch_thread_setspecific(dispatch_voucher_key, voucher);
	mach_voucher_t kv = voucher ? voucher->v_kvoucher : MACH_VOUCHER_NULL;
	mach_voucher_t okv = ov ? ov->v_kvoucher : MACH_VOUCHER_NULL;
	return (kv != okv) ? kv : VOUCHER_NO_MACH_VOUCHER;
}

DISPATCH_ALWAYS_INLINE
static inline void
_voucher_swap(voucher_t ov, voucher_t voucher)
{
	_voucher_mach_voucher_set(_voucher_swap_and_get_mach_voucher(ov, voucher));
	if (ov) _voucher_release(ov);
}

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline voucher_t
_voucher_adopt(voucher_t voucher)
{
	voucher_t ov = _voucher_get();
	_voucher_mach_voucher_set(_voucher_swap_and_get_mach_voucher(ov, voucher));
	return ov;
}

DISPATCH_ALWAYS_INLINE
static inline void
_voucher_replace(voucher_t voucher)
{
	voucher_t ov = _voucher_get();
	_voucher_swap(ov, voucher);
}

DISPATCH_ALWAYS_INLINE
static inline void
_voucher_clear(void)
{
	_voucher_replace(NULL);
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_voucher_get_priority(voucher_t voucher)
{
	return voucher && voucher->v_has_priority ?
			(pthread_priority_t)*_voucher_priority(voucher) : 0;
}

void _voucher_task_mach_voucher_init(void* ctxt);
extern dispatch_once_t _voucher_task_mach_voucher_pred;
extern mach_voucher_t _voucher_task_mach_voucher;

DISPATCH_ALWAYS_INLINE
static inline mach_voucher_t
_voucher_get_task_mach_voucher(void)
{
	dispatch_once_f(&_voucher_task_mach_voucher_pred, NULL,
			_voucher_task_mach_voucher_init);
	return _voucher_task_mach_voucher;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_voucher_mach_msg_set_mach_voucher(mach_msg_header_t *msg, mach_voucher_t kv,
		bool move_send)
{
	if (MACH_MSGH_BITS_HAS_VOUCHER(msg->msgh_bits)) return false;
	if (!kv) return false;
	msg->msgh_voucher_port = kv;
	msg->msgh_bits |= MACH_MSGH_BITS_SET_PORTS(0, 0, move_send ?
			MACH_MSG_TYPE_MOVE_SEND : MACH_MSG_TYPE_COPY_SEND);
	_dispatch_kvoucher_debug("msg[%p] set %s", kv, msg, move_send ?
			"move-send" : "copy-send");
	_dispatch_voucher_debug_machport(kv);
	return true;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_voucher_mach_msg_set(mach_msg_header_t *msg, voucher_t voucher)
{
	if (MACH_MSGH_BITS_HAS_VOUCHER(msg->msgh_bits)) return false;
	mach_voucher_t kv;
	if (voucher) {
		kv = _voucher_get_mach_voucher(voucher);
	} else {
		kv = _voucher_get_task_mach_voucher();
	}
	return _voucher_mach_msg_set_mach_voucher(msg, kv, false);
}

DISPATCH_ALWAYS_INLINE
static inline mach_voucher_t
_voucher_mach_msg_get(mach_msg_header_t *msg)
{
	if (!MACH_MSGH_BITS_HAS_VOUCHER(msg->msgh_bits)) return MACH_VOUCHER_NULL;
	mach_voucher_t kv = msg->msgh_voucher_port;
	msg->msgh_voucher_port = MACH_VOUCHER_NULL;
	msg->msgh_bits &= (mach_msg_bits_t)~MACH_MSGH_BITS_VOUCHER_MASK;
	return kv;
}

DISPATCH_ALWAYS_INLINE
static inline mach_voucher_t
_voucher_mach_msg_clear(mach_msg_header_t *msg, bool move_send)
{
	mach_msg_bits_t kvbits = MACH_MSGH_BITS_VOUCHER(msg->msgh_bits);
	mach_voucher_t kv = msg->msgh_voucher_port, kvm = MACH_VOUCHER_NULL;
	if ((kvbits == MACH_MSG_TYPE_COPY_SEND ||
			kvbits == MACH_MSG_TYPE_MOVE_SEND) && kv) {
		_dispatch_kvoucher_debug("msg[%p] clear %s", kv, msg, move_send ?
				"move-send" : "copy-send");
		_dispatch_voucher_debug_machport(kv);
		if (kvbits == MACH_MSG_TYPE_MOVE_SEND) {
			// <rdar://problem/15694142> return/drop received or pseudo-received
			// voucher reference (e.g. due to send failure).
			if (move_send) {
				kvm = kv;
			} else {
				_voucher_dealloc_mach_voucher(kv);
			}
		}
		msg->msgh_voucher_port = MACH_VOUCHER_NULL;
		msg->msgh_bits &= (mach_msg_bits_t)~MACH_MSGH_BITS_VOUCHER_MASK;
	}
	return kvm;
}

#pragma mark -
#pragma mark dispatch_continuation_t + voucher_t

#if DISPATCH_USE_KDEBUG_TRACE
DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_voucher_ktrace(int code, natural_t voucher, void *container)
{
	if (!voucher) return;
	__kdebug_trace(APPSDBG_CODE(DBG_MACH_CHUD, (0xfac >> 2)) | DBG_FUNC_NONE,
			code, (int)voucher, (int)(uintptr_t)container,
#ifdef __LP64__
			(int)((uintptr_t)container >> 32)
#else
			0
#endif
			);
}
#define _dispatch_voucher_ktrace_dc_push(dc) \
		_dispatch_voucher_ktrace(0x1, (dc)->dc_voucher ? \
				(dc)->dc_voucher->v_kvoucher : MACH_VOUCHER_NULL, (dc))
#define _dispatch_voucher_ktrace_dc_pop(dc) \
		_dispatch_voucher_ktrace(0x2, (dc)->dc_voucher ? \
				(dc)->dc_voucher->v_kvoucher : MACH_VOUCHER_NULL, (dc))
#define _dispatch_voucher_ktrace_dmsg_push(dmsg) \
		_dispatch_voucher_ktrace(0x3, (dmsg)->dmsg_voucher ? \
				(dmsg)->dmsg_voucher->v_kvoucher : MACH_VOUCHER_NULL, (dmsg))
#define _dispatch_voucher_ktrace_dmsg_pop(dmsg) \
		_dispatch_voucher_ktrace(0x4, (dmsg)->dmsg_voucher ? \
				(dmsg)->dmsg_voucher->v_kvoucher : MACH_VOUCHER_NULL, (dmsg))
#else
#define _dispatch_voucher_ktrace_dc_push(dc)
#define _dispatch_voucher_ktrace_dc_pop(dc)
#define _dispatch_voucher_ktrace_dmsg_push(dmsg)
#define _dispatch_voucher_ktrace_dmsg_pop(dmsg)
#endif // DISPATCH_USE_KDEBUG_TRACE

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_voucher_set(dispatch_continuation_t dc,
		dispatch_block_flags_t flags)
{
	unsigned long bits = (unsigned long)dc->do_vtable;
	voucher_t v = NULL;

	if (flags & DISPATCH_BLOCK_HAS_VOUCHER) {
		bits |= DISPATCH_OBJ_HAS_VOUCHER_BIT;
	} else if (!(flags & DISPATCH_BLOCK_NO_VOUCHER)) {
		v = _voucher_copy();
	}
	dc->do_vtable = (void*)bits;
	dc->dc_voucher = v;
	_dispatch_voucher_debug("continuation[%p] set", dc->dc_voucher, dc);
	_dispatch_voucher_ktrace_dc_push(dc);
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_voucher_adopt(dispatch_continuation_t dc)
{
	unsigned long bits = (unsigned long)dc->do_vtable;
	voucher_t v = DISPATCH_NO_VOUCHER;
	if (!(bits & DISPATCH_OBJ_HAS_VOUCHER_BIT)) {
		_dispatch_voucher_ktrace_dc_pop(dc);
		_dispatch_voucher_debug("continuation[%p] adopt", dc->dc_voucher, dc);
		v = dc->dc_voucher;
		dc->dc_voucher = NULL;
	}
	_dispatch_adopt_priority_and_replace_voucher(dc->dc_priority, v, 0);
}

#pragma mark -
#pragma mark _voucher_activity_heap

typedef uint32_t _voucher_atm_subid_t;
static const size_t _voucher_activity_hash_bits = 6;
static const size_t _voucher_activity_hash_size =
		1 << _voucher_activity_hash_bits;
#define VACTID_HASH(x) \
		(((uint32_t)(x) * 2654435761u) >> (32-_voucher_activity_hash_bits))
#define VATMID_HASH(x) \
		(((uint32_t)(x) * 2654435761u) >> (32-_voucher_activity_hash_bits))
#define VATMID2ACTID(x, flags) \
		(((voucher_activity_id_t)(x) & 0xffffffffffffff) | \
		(((voucher_activity_id_t)(flags) & 0xfe) << 55))

typedef struct _voucher_activity_metadata_s {
	_voucher_activity_buffer_t vam_client_metadata;
	struct _voucher_activity_metadata_opaque_s *vasm_baseaddr;
	_voucher_activity_bitmap_t volatile vam_buffer_bitmap;
	_voucher_activity_bitmap_t volatile vam_pressure_locked_bitmap;
	_voucher_activity_lock_s vam_atms_lock;
	_voucher_activity_lock_s vam_activities_lock;
	TAILQ_HEAD(, _voucher_atm_s) vam_atms[_voucher_activity_hash_size];
	TAILQ_HEAD(, _voucher_activity_s)
			vam_activities[_voucher_activity_hash_size];
} *_voucher_activity_metadata_t;

#pragma mark -
#pragma mark _voucher_atm_t

typedef struct _voucher_atm_s {
	int32_t volatile vatm_refcnt;
	mach_voucher_t vatm_kvoucher;
	atm_aid_t vatm_id;
	atm_guard_t vatm_generation;
	TAILQ_ENTRY(_voucher_atm_s) vatm_list;
#if __LP64__
	uintptr_t vatm_pad[3];
	// cacheline
#endif
} *_voucher_atm_t;

extern _voucher_atm_t _voucher_task_atm;

#pragma mark -
#pragma mark _voucher_activity_t

typedef struct _voucher_activity_s {
	voucher_activity_id_t va_id;
	voucher_activity_trace_id_t va_trace_id;
	uint64_t va_location;
	int32_t volatile va_refcnt;
	uint32_t volatile va_buffer_count;
	uint32_t va_buffer_limit;
	_voucher_activity_buffer_header_t volatile va_current_buffer;
	_voucher_atm_t va_atm;
#if __LP64__
	uint64_t va_unused;
#endif
	// cacheline
	_voucher_activity_lock_s va_buffers_lock;
	TAILQ_HEAD(_voucher_activity_buffer_list_s,
			_voucher_activity_buffer_header_s) va_buffers;
	TAILQ_ENTRY(_voucher_activity_s) va_list;
	TAILQ_ENTRY(_voucher_activity_s) va_atm_list;
	TAILQ_ENTRY(_voucher_activity_s) va_atm_used_list;
	pthread_mutex_t va_mutex;
	pthread_cond_t va_cond;
} *_voucher_activity_t;

_voucher_activity_tracepoint_t _voucher_activity_buffer_tracepoint_acquire_slow(
		_voucher_activity_t *vap, _voucher_activity_buffer_header_t *vabp,
		unsigned int slots, size_t strsize, uint16_t *stroffsetp);
void _voucher_activity_firehose_push(_voucher_activity_t act,
		_voucher_activity_buffer_header_t buffer);
extern _voucher_activity_t _voucher_activity_default;
extern voucher_activity_mode_t _voucher_activity_mode;

#if DISPATCH_DEBUG && DISPATCH_VOUCHER_ACTIVITY_DEBUG
#define _dispatch_voucher_activity_debug(msg, act, ...) \
		_dispatch_debug("activity[%p] <0x%llx>: atm[%p] <%lld>: " msg, (act), \
		(act) ? (act)->va_id : 0, (act) ? (act)->va_atm : NULL, \
		(act) && (act)->va_atm ? (act)->va_atm->vatm_id : 0, ##__VA_ARGS__)
#define _dispatch_voucher_atm_debug(msg, atm, ...) \
		_dispatch_debug("atm[%p] <%lld> kvoucher[0x%08x]: " msg, (atm), \
		(atm) ? (atm)->vatm_id : 0, (atm) ? (atm)->vatm_kvoucher : 0, \
		##__VA_ARGS__)
#else
#define _dispatch_voucher_activity_debug(msg, act, ...)
#define _dispatch_voucher_atm_debug(msg, atm, ...)
#endif

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_voucher_activity_timestamp(bool approx)
{
#if TARGET_IPHONE_SIMULATOR && \
		IPHONE_SIMULATOR_HOST_MIN_VERSION_REQUIRED < 101000
	(void)approx;
	return mach_absolute_time();
#else
	return approx ? mach_approximate_time() : mach_absolute_time();
#endif
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_voucher_activity_thread_id(void)
{
	uint64_t thread_id;
	pthread_threadid_np(NULL, &thread_id); // TODO: 15923074: use TSD thread_id
	return thread_id;
}

#define _voucher_activity_buffer_pos2length(pos) \
		({ _voucher_activity_buffer_position_u _pos = (pos); \
		_pos.vabp_pos.vabp_next_tracepoint_idx * \
		sizeof(struct _voucher_activity_tracepoint_s) + \
		_pos.vabp_pos.vabp_string_offset; })

DISPATCH_ALWAYS_INLINE
static inline _voucher_activity_tracepoint_t
_voucher_activity_buffer_tracepoint_acquire(
		_voucher_activity_buffer_header_t vab, unsigned int slots,
		size_t strsize, uint16_t *stroffsetp)
{
	if (!vab) return NULL;
	_voucher_activity_buffer_position_u pos_orig, pos;
	pos_orig.vabp_atomic_pos = vab->vabh_pos.vabp_atomic_pos;
	do {
		pos.vabp_atomic_pos = pos_orig.vabp_atomic_pos;
		pos.vabp_pos.vabp_next_tracepoint_idx += slots;
		pos.vabp_pos.vabp_string_offset += strsize;
		size_t len = _voucher_activity_buffer_pos2length(pos);
		if (len > _voucher_activity_buffer_size || pos.vabp_pos.vabp_flags) {
			return NULL;
		}
		if (len == _voucher_activity_buffer_size) {
			pos.vabp_pos.vabp_flags |= _voucher_activity_buffer_full;
		}
		pos.vabp_pos.vabp_refcnt++;
	} while (!dispatch_atomic_cmpxchgvw2o(vab, vabh_pos.vabp_atomic_pos,
			pos_orig.vabp_atomic_pos, pos.vabp_atomic_pos,
			&pos_orig.vabp_atomic_pos, relaxed));
	if (stroffsetp) *stroffsetp = pos.vabp_pos.vabp_string_offset;
	return (_voucher_activity_tracepoint_t)vab +
			pos_orig.vabp_pos.vabp_next_tracepoint_idx;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_voucher_activity_buffer_tracepoint_release(
		_voucher_activity_buffer_header_t vab)
{
	_voucher_activity_buffer_position_u pos_orig, pos;
	pos_orig.vabp_atomic_pos = vab->vabh_pos.vabp_atomic_pos;
	do {
		pos.vabp_atomic_pos = pos_orig.vabp_atomic_pos;
		pos.vabp_pos.vabp_refcnt--;
		if (!pos.vabp_pos.vabp_refcnt &&
				(pos.vabp_pos.vabp_flags & _voucher_activity_buffer_full)) {
			pos.vabp_pos.vabp_flags |= _voucher_activity_buffer_pushing;
		}
	} while (!dispatch_atomic_cmpxchgvw2o(vab, vabh_pos.vabp_atomic_pos,
			pos_orig.vabp_atomic_pos, pos.vabp_atomic_pos,
			&pos_orig.vabp_atomic_pos, relaxed));
	return (pos.vabp_pos.vabp_flags & _voucher_activity_buffer_pushing);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_voucher_activity_buffer_mark_full(_voucher_activity_buffer_header_t vab)
{
	_voucher_activity_buffer_position_u pos_orig, pos;
	pos_orig.vabp_atomic_pos = vab->vabh_pos.vabp_atomic_pos;
	do {
		pos.vabp_atomic_pos = pos_orig.vabp_atomic_pos;
		if (pos.vabp_pos.vabp_flags & _voucher_activity_buffer_full) {
			return false;
		}
		pos.vabp_pos.vabp_flags |= _voucher_activity_buffer_full;
		if (!pos.vabp_pos.vabp_refcnt) {
			pos.vabp_pos.vabp_flags |= _voucher_activity_buffer_pushing;
		}
	} while (!dispatch_atomic_cmpxchgvw2o(vab, vabh_pos.vabp_atomic_pos,
			pos_orig.vabp_atomic_pos, pos.vabp_atomic_pos,
			&pos_orig.vabp_atomic_pos, relaxed));
	return (pos.vabp_pos.vabp_flags & _voucher_activity_buffer_pushing);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_voucher_activity_buffer_is_full(_voucher_activity_buffer_header_t vab)
{
	_voucher_activity_buffer_position_u pos;
	pos.vabp_atomic_pos = vab->vabh_pos.vabp_atomic_pos;
	return (pos.vabp_pos.vabp_flags);
}

DISPATCH_ALWAYS_INLINE
static inline _voucher_activity_buffer_header_t
_voucher_activity_buffer_get_from_activity(_voucher_activity_t va)
{
	return va ? va->va_current_buffer : NULL;
}

DISPATCH_ALWAYS_INLINE
static inline _voucher_activity_t
_voucher_activity_get(void)
{
	_voucher_activity_t va;
	voucher_t v = _voucher_get();
	va = v && v->v_activity ? v->v_activity : _voucher_activity_default;
	return va;
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_voucher_activity_tracepoint_init(_voucher_activity_tracepoint_t vat,
		uint8_t type, uint8_t code_namespace, uint32_t code, uint64_t location,
		bool approx)
{
	if (!location) location = (uint64_t)__builtin_return_address(0);
	uint64_t timestamp = _voucher_activity_timestamp(approx);
	vat->vat_flags = _voucher_activity_trace_flag_tracepoint,
	vat->vat_type = type,
	vat->vat_namespace = code_namespace,
	vat->vat_code = code,
	vat->vat_timestamp = timestamp,
	vat->vat_thread = _voucher_activity_thread_id(),
	vat->vat_location = location;
	return timestamp;
}

DISPATCH_ALWAYS_INLINE
static inline uint64_t
_voucher_activity_tracepoint_init_with_id(_voucher_activity_tracepoint_t vat,
		voucher_activity_trace_id_t trace_id, uint64_t location, bool approx)
{
	uint8_t type = (uint8_t)(trace_id >> _voucher_activity_trace_id_type_shift);
	uint8_t cns = (uint8_t)(trace_id >>
			_voucher_activity_trace_id_code_namespace_shift);
	uint32_t code = (uint32_t)trace_id;
	return _voucher_activity_tracepoint_init(vat, type, cns, code, location,
			approx);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_voucher_activity_trace_id_is_subtype(voucher_activity_trace_id_t trace_id,
		uint8_t type)
{
	voucher_activity_trace_id_t type_id = voucher_activity_trace_id(type, 0, 0);
	return (trace_id & type_id) == type_id;
}
#define _voucher_activity_trace_id_is_subtype(trace_id, name) \
	_voucher_activity_trace_id_is_subtype(trace_id, \
			voucher_activity_tracepoint_type_ ## name)

DISPATCH_ALWAYS_INLINE
static inline bool
_voucher_activity_trace_id_enabled(voucher_activity_trace_id_t trace_id)
{
	switch (_voucher_activity_mode) {
	case voucher_activity_mode_release:
		return _voucher_activity_trace_id_is_subtype(trace_id, release);
	case voucher_activity_mode_stream:
	case voucher_activity_mode_debug:
		return _voucher_activity_trace_id_is_subtype(trace_id, debug) ||
				_voucher_activity_trace_id_is_subtype(trace_id, release);
	}
	return false;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_voucher_activity_trace_type_enabled(uint8_t type)
{
	voucher_activity_trace_id_t type_id = voucher_activity_trace_id(type, 0, 0);
	return _voucher_activity_trace_id_enabled(type_id);
}

DISPATCH_ALWAYS_INLINE
static inline bool
_voucher_activity_disabled(void)
{
	return slowpath(_voucher_activity_mode == voucher_activity_mode_disable);
}

DISPATCH_ALWAYS_INLINE
static inline void
_voucher_activity_trace_args_inline(uint8_t type, uint8_t code_namespace,
		uint32_t code, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
		uintptr_t arg4)
{
	if (!_voucher_activity_trace_type_enabled(type)) return;
	_voucher_activity_t act;
	_voucher_activity_buffer_header_t vab;
	_voucher_activity_tracepoint_t vat;
	act = _voucher_activity_get();
	vab = _voucher_activity_buffer_get_from_activity(act);
	vat = _voucher_activity_buffer_tracepoint_acquire(vab, 1, 0, NULL);
	if (!vat) return;
	_voucher_activity_tracepoint_init(vat, type, code_namespace, code, 0, true);
	vat->vat_flags |= _voucher_activity_trace_flag_tracepoint_args;
	vat->vat_data[0] = arg1;
	vat->vat_data[1] = arg2;
	vat->vat_data[2] = arg3;
	vat->vat_data[3] = arg4;
	if (_voucher_activity_buffer_tracepoint_release(vab)) {
		_voucher_activity_firehose_push(act, vab);
	}
}

DISPATCH_ALWAYS_INLINE
static inline void
_voucher_activity_trace_activity_event(voucher_activity_trace_id_t trace_id,
		voucher_activity_id_t va_id, _voucher_activity_tracepoint_flag_t flags)
{
	_voucher_activity_t act;
	_voucher_activity_buffer_header_t vab;
	_voucher_activity_tracepoint_t vat;
	act = _voucher_activity_get();
	vab = _voucher_activity_buffer_get_from_activity(act);
	vat = _voucher_activity_buffer_tracepoint_acquire(vab, 1, 0, NULL);
	if (!vat) return;
	_voucher_activity_tracepoint_init_with_id(vat, trace_id, 0, false);
	vat->vat_flags |= _voucher_activity_trace_flag_activity | flags;
	vat->vat_data[0] = va_id;
	if (_voucher_activity_buffer_tracepoint_release(vab)) {
		_voucher_activity_firehose_push(act, vab);
	}
}
#define _voucher_activity_trace_activity_event(trace_id, va_id, type) \
		_voucher_activity_trace_activity_event(trace_id, va_id, \
				_voucher_activity_trace_flag_ ## type)

DISPATCH_ALWAYS_INLINE
static inline void
_voucher_activity_trace_msg(voucher_t v, mach_msg_header_t *msg, uint32_t code)
{
	if (!v || !v->v_activity) return; // Don't use default activity for IPC
	const uint8_t type = voucher_activity_tracepoint_type_debug;
	const uint8_t code_namespace = _voucher_activity_tracepoint_namespace_ipc;
	if (!_voucher_activity_trace_type_enabled(type)) return;
	_voucher_activity_buffer_header_t vab;
	_voucher_activity_tracepoint_t vat;
	vab = _voucher_activity_buffer_get_from_activity(v->v_activity);
	vat = _voucher_activity_buffer_tracepoint_acquire(vab, 1, 0, NULL);
	if (!vat) return; // TODO: slowpath ?
	_voucher_activity_tracepoint_init(vat, type, code_namespace, code, 0, true);
	vat->vat_flags |= _voucher_activity_trace_flag_libdispatch;
#if __has_extension(c_static_assert)
	_Static_assert(sizeof(mach_msg_header_t) <= sizeof(vat->vat_data),
			"mach_msg_header_t too large");
#endif
	memcpy(vat->vat_data, msg, sizeof(mach_msg_header_t));
	if (_voucher_activity_buffer_tracepoint_release(vab)) {
		_voucher_activity_firehose_push(v->v_activity, vab);
	}
}
#define _voucher_activity_trace_msg(v, msg, type) \
		_voucher_activity_trace_msg(v, msg, \
				_voucher_activity_tracepoint_namespace_ipc_ ## type)

#endif // !(USE_OBJC && __OBJC2__) && !defined(__cplusplus)

#else // VOUCHER_USE_MACH_VOUCHER

#pragma mark -
#pragma mark Simulator / vouchers disabled

#define _dispatch_voucher_debug(msg, v, ...)
#define _dispatch_kvoucher_debug(msg, kv, ...)

DISPATCH_ALWAYS_INLINE
static inline voucher_t
_voucher_retain(voucher_t voucher)
{
	return voucher;
}

DISPATCH_ALWAYS_INLINE
static inline void
_voucher_release(voucher_t voucher)
{
	(void)voucher;
}

DISPATCH_ALWAYS_INLINE
static inline voucher_t
_voucher_get(void)
{
	return NULL;
}

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline voucher_t
_voucher_copy(void)
{
	return NULL;
}

DISPATCH_ALWAYS_INLINE DISPATCH_WARN_RESULT
static inline voucher_t
_voucher_copy_without_importance(void)
{
	return NULL;
}

DISPATCH_ALWAYS_INLINE
static inline mach_voucher_t
_voucher_swap_and_get_mach_voucher(voucher_t ov, voucher_t voucher)
{
	(void)ov; (void)voucher;
	return MACH_VOUCHER_NULL;
}

DISPATCH_ALWAYS_INLINE
static inline voucher_t
_voucher_adopt(voucher_t voucher)
{
	return voucher;
}

DISPATCH_ALWAYS_INLINE
static inline void
_voucher_replace(voucher_t voucher)
{
	(void)voucher;
}

DISPATCH_ALWAYS_INLINE
static inline void
_voucher_clear(void)
{
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_voucher_get_priority(voucher_t voucher)
{
	(void)voucher;
	return 0;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_voucher_mach_msg_set_mach_voucher(mach_msg_header_t *msg, mach_voucher_t kv,
		bool move_send)
{
	(void)msg; (void)kv; (void)move_send;
	return false;

}

DISPATCH_ALWAYS_INLINE
static inline bool
_voucher_mach_msg_set(mach_msg_header_t *msg, voucher_t voucher)
{
	(void)msg; (void)voucher;
	return false;
}

DISPATCH_ALWAYS_INLINE
static inline mach_voucher_t
_voucher_mach_msg_get(mach_msg_header_t *msg)
{
	(void)msg;
	return 0;
}

DISPATCH_ALWAYS_INLINE
static inline mach_voucher_t
_voucher_mach_msg_clear(mach_msg_header_t *msg, bool move_send)
{
	(void)msg; (void)move_send;
	return MACH_VOUCHER_NULL;
}

#define _dispatch_voucher_ktrace_dmsg_push(dmsg)
#define _dispatch_voucher_ktrace_dmsg_pop(dmsg)

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_voucher_set(dispatch_continuation_t dc,
		dispatch_block_flags_t flags)
{
	(void)dc; (void)flags;
}

DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_continuation_voucher_adopt(dispatch_continuation_t dc)
{
	(void)dc;
}

#define _voucher_activity_trace_msg(v, msg, type)

DISPATCH_ALWAYS_INLINE
static inline bool
_voucher_activity_disabled(void)
{
	return true;
}

#endif // VOUCHER_USE_MACH_VOUCHER

#endif /* __DISPATCH_VOUCHER_INTERNAL__ */
