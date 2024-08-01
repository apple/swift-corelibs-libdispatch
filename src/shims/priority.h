/*
 * Copyright (c) 2008-2016 Apple Inc. All rights reserved.
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
 *
 * This file shares knowledge with libpthread and xnu on the pthread_priority_t
 * encoding. This is intentional to make the priority manipulations in dispatch
 * as fast as possible instead of having to make cross library calls when
 * encoding and decoding a pthread_priority_t. In addition, dispatch has its own
 * representation of priority via dispatch_priority_t which has some extra
 * information that is not of interest to pthread or xnu.
 *
 * This file encapsulates all the priority related manipulations and encodings
 * to and from the various formats that dispatch deals with.
 */

#ifndef __DISPATCH_SHIMS_PRIORITY__
#define __DISPATCH_SHIMS_PRIORITY__

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif // __APPLE__

#if HAVE_PTHREAD_QOS_H && __has_include(<pthread/qos_private.h>)
#include <pthread/qos.h>
#include <pthread/qos_private.h>
#ifndef _PTHREAD_PRIORITY_OVERCOMMIT_FLAG
#define _PTHREAD_PRIORITY_OVERCOMMIT_FLAG 0x80000000
#endif
#ifndef _PTHREAD_PRIORITY_SCHED_PRI_FLAG
#define _PTHREAD_PRIORITY_SCHED_PRI_FLAG 0x20000000
#endif
#ifndef _PTHREAD_PRIORITY_FALLBACK_FLAG
#define _PTHREAD_PRIORITY_FALLBACK_FLAG 0x04000000
#endif
#ifndef _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG
#define _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG 0x02000000
#endif
#ifndef _PTHREAD_PRIORITY_NEEDS_UNBIND_FLAG
#define _PTHREAD_PRIORITY_NEEDS_UNBIND_FLAG 0x01000000
#endif
#ifndef _PTHREAD_PRIORITY_COOPERATIVE_FLAG
#define _PTHREAD_PRIORITY_COOPERATIVE_FLAG 0x08000000
#endif
#ifndef _PTHREAD_PRIORITY_THREAD_TYPE_MASK
#define _PTHREAD_PRIORITY_THREAD_TYPE_MASK \
  (_PTHREAD_PRIORITY_OVERCOMMIT_FLAG | _PTHREAD_PRIORITY_COOPERATIVE_FLAG)
#endif

#else // HAVE_PTHREAD_QOS_H

OS_ENUM(qos_class, unsigned int,
	QOS_CLASS_USER_INTERACTIVE = 0x21,
	QOS_CLASS_USER_INITIATED = 0x19,
	QOS_CLASS_DEFAULT = 0x15,
	QOS_CLASS_UTILITY = 0x11,
	QOS_CLASS_BACKGROUND = 0x09,
	QOS_CLASS_MAINTENANCE = 0x05,
	QOS_CLASS_UNSPECIFIED = 0x00,
);
typedef unsigned long pthread_priority_t;
#define QOS_MIN_RELATIVE_PRIORITY (-15)
#define _PTHREAD_PRIORITY_OVERCOMMIT_FLAG 0x80000000
#define _PTHREAD_PRIORITY_SCHED_PRI_FLAG 0x20000000
#define _PTHREAD_PRIORITY_FALLBACK_FLAG 0x04000000
#define _PTHREAD_PRIORITY_COOPERATIVE_FLAG 0x08000000
#define _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG 0x02000000
#define _PTHREAD_PRIORITY_NEEDS_UNBIND_FLAG 0x01000000
#define _PTHREAD_PRIORITY_ENFORCE_FLAG  0x10000000
#define _PTHREAD_PRIORITY_THREAD_TYPE_MASK \
	(_PTHREAD_PRIORITY_OVERCOMMIT_FLAG | _PTHREAD_PRIORITY_COOPERATIVE_FLAG)

// Mask values
#if !TARGET_OS_SIMULATOR
#define _PTHREAD_PRIORITY_FLAGS_MASK (~0xffffff)
#define _PTHREAD_PRIORITY_QOS_CLASS_MASK 0x00ffff00
#define _PTHREAD_PRIORITY_QOS_CLASS_SHIFT (8ull)
#define _PTHREAD_PRIORITY_PRIORITY_MASK 0x000000ff
#endif

#endif // HAVE_PTHREAD_QOS_H

#if !defined(POLICY_RR) && defined(SCHED_RR)
#define POLICY_RR SCHED_RR
#endif // !defined(POLICY_RR) && defined(SCHED_RR)

/*
 * dispatch_priority_t encoding
 *
 *         flags             unused    override   fallback  req qos    relpri
 * |----------------------|----------|----------|---------|---------|----------|
 *       24 - 31             20-23      16-19     12-15      8-11       0-7
 *
 * Some of the fields here are laid out similar to what is laid out in
 * pthread_priority_t - namely flags and relpri. This is enforced via static
 * asserts on the mask and shift values. If the layout of pthread_priority_t
 * changes to add more flags etc, we need to make relevant changes in
 * dispatch_priority_t as well.
 */
typedef uint32_t dispatch_qos_t;
typedef uint32_t dispatch_priority_t;

// QoS encoding unique to dispatch_priority_t - used for req, fallback and
// override
#define DISPATCH_QOS_UNSPECIFIED        ((dispatch_qos_t)0)
#define DISPATCH_QOS_MAINTENANCE        ((dispatch_qos_t)1)
#define DISPATCH_QOS_BACKGROUND         ((dispatch_qos_t)2)
#define DISPATCH_QOS_UTILITY            ((dispatch_qos_t)3)
#define DISPATCH_QOS_DEFAULT            ((dispatch_qos_t)4)
#define DISPATCH_QOS_USER_INITIATED     ((dispatch_qos_t)5)
#define DISPATCH_QOS_USER_INTERACTIVE   ((dispatch_qos_t)6)
#define DISPATCH_QOS_MIN                DISPATCH_QOS_MAINTENANCE
#define DISPATCH_QOS_MAX                DISPATCH_QOS_USER_INTERACTIVE
#define DISPATCH_QOS_SATURATED          ((dispatch_qos_t)15)

#define DISPATCH_QOS_NBUCKETS           (DISPATCH_QOS_MAX - DISPATCH_QOS_MIN + 1)
#define DISPATCH_QOS_BUCKET(qos)        ((int)((qos) - DISPATCH_QOS_MIN))
#define DISPATCH_QOS_FOR_BUCKET(bucket) ((dispatch_qos_t)((uint32_t)bucket + DISPATCH_QOS_MIN))

#define DISPATCH_PRIORITY_QOS_MASK           ((dispatch_priority_t)0x00000f00)
#define DISPATCH_PRIORITY_QOS_SHIFT          8
#define DISPATCH_PRIORITY_REQUESTED_MASK     ((dispatch_priority_t)0x00000fff)
#define DISPATCH_PRIORITY_FALLBACK_QOS_MASK  ((dispatch_priority_t)0x0000f000)
#define DISPATCH_PRIORITY_FALLBACK_QOS_SHIFT 12
#define DISPATCH_PRIORITY_OVERRIDE_MASK      ((dispatch_priority_t)0x000f0000)
#define DISPATCH_PRIORITY_OVERRIDE_SHIFT     16
#define DISPATCH_PRIORITY_SATURATED_OVERRIDE DISPATCH_PRIORITY_OVERRIDE_MASK

// not passed to pthread
#define DISPATCH_PRIORITY_FLAG_FLOOR         ((dispatch_priority_t)0x40000000) // _PTHREAD_PRIORITY_INHERIT_FLAG
#define DISPATCH_PRIORITY_FLAG_ENFORCE       ((dispatch_priority_t)0x10000000) // _PTHREAD_PRIORITY_ENFORCE_FLAG
#define DISPATCH_PRIORITY_FLAG_INHERITED     ((dispatch_priority_t)0x20000000)

// Stuff which overlaps between dispatch_priority_t and pthread_priority_t

// relpri
#define DISPATCH_PRIORITY_RELPRI_MASK        ((dispatch_priority_t)0x000000ff)
#define DISPATCH_PRIORITY_RELPRI_SHIFT       0
#if !TARGET_OS_SIMULATOR
dispatch_static_assert(DISPATCH_PRIORITY_RELPRI_MASK == _PTHREAD_PRIORITY_PRIORITY_MASK, "relpri masks match");
dispatch_static_assert(DISPATCH_PRIORITY_RELPRI_SHIFT == _PTHREAD_PRIORITY_PRIORITY_SHIFT, "relpri shift match");
#endif

// flags
#define DISPATCH_PRIORITY_FLAGS_MASK         ((dispatch_priority_t)0xff000000)
#if !TARGET_OS_SIMULATOR
dispatch_static_assert(DISPATCH_PRIORITY_FLAGS_MASK == _PTHREAD_PRIORITY_FLAGS_MASK, "pthread priority flags mask match");
#endif
#define DISPATCH_PRIORITY_FLAG_OVERCOMMIT    ((dispatch_priority_t)0x80000000)
dispatch_static_assert(DISPATCH_PRIORITY_FLAG_OVERCOMMIT == _PTHREAD_PRIORITY_OVERCOMMIT_FLAG, "overcommit flags match");
#define DISPATCH_PRIORITY_FLAG_FALLBACK      ((dispatch_priority_t)0x04000000)
dispatch_static_assert(DISPATCH_PRIORITY_FLAG_FALLBACK == _PTHREAD_PRIORITY_FALLBACK_FLAG, "fallback flags match");
#define DISPATCH_PRIORITY_FLAG_MANAGER       ((dispatch_priority_t)0x02000000)
dispatch_static_assert(DISPATCH_PRIORITY_FLAG_MANAGER == _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG, "manager flags match");
#define DISPATCH_PRIORITY_FLAG_COOPERATIVE   ((dispatch_priority_t)0x08000000)
dispatch_static_assert(DISPATCH_PRIORITY_FLAG_COOPERATIVE == _PTHREAD_PRIORITY_COOPERATIVE_FLAG, "cooperative flags match");

// Subset of pthread priority flags that we care about in dispatch_priority_t
#define DISPATCH_PRIORITY_PTHREAD_PRIORITY_FLAGS_MASK \
		(DISPATCH_PRIORITY_FLAG_OVERCOMMIT | DISPATCH_PRIORITY_FLAG_FALLBACK | \
		DISPATCH_PRIORITY_FLAG_MANAGER | DISPATCH_PRIORITY_FLAG_COOPERATIVE)

// Subset of pthread priority flags about thread req type
#define DISPATCH_PRIORITY_THREAD_TYPE_MASK \
		(DISPATCH_PRIORITY_FLAG_OVERCOMMIT | DISPATCH_PRIORITY_FLAG_COOPERATIVE)

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_qos_class_valid(qos_class_t cls, int relpri)
{
	switch ((unsigned int)cls) {
	case QOS_CLASS_MAINTENANCE:
	case QOS_CLASS_BACKGROUND:
	case QOS_CLASS_UTILITY:
	case QOS_CLASS_DEFAULT:
	case QOS_CLASS_USER_INITIATED:
	case QOS_CLASS_USER_INTERACTIVE:
	case QOS_CLASS_UNSPECIFIED:
		break;
	default:
		return false;
	}
	return QOS_MIN_RELATIVE_PRIORITY <= relpri && relpri <= 0;
}

#pragma mark dispatch_qos

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dispatch_qos_from_qos_class(qos_class_t cls)
{
	switch ((unsigned int)cls) {
	case QOS_CLASS_USER_INTERACTIVE: return DISPATCH_QOS_USER_INTERACTIVE;
	case QOS_CLASS_USER_INITIATED:   return DISPATCH_QOS_USER_INITIATED;
	case QOS_CLASS_DEFAULT:          return DISPATCH_QOS_DEFAULT;
	case QOS_CLASS_UTILITY:          return DISPATCH_QOS_UTILITY;
	case QOS_CLASS_BACKGROUND:       return DISPATCH_QOS_BACKGROUND;
	case QOS_CLASS_MAINTENANCE:      return DISPATCH_QOS_MAINTENANCE;
	default: return DISPATCH_QOS_UNSPECIFIED;
	}
}

DISPATCH_ALWAYS_INLINE
static inline qos_class_t
_dispatch_qos_to_qos_class(dispatch_qos_t qos)
{
	switch (qos) {
	case DISPATCH_QOS_USER_INTERACTIVE: return QOS_CLASS_USER_INTERACTIVE;
	case DISPATCH_QOS_USER_INITIATED:   return QOS_CLASS_USER_INITIATED;
	case DISPATCH_QOS_DEFAULT:          return QOS_CLASS_DEFAULT;
	case DISPATCH_QOS_UTILITY:          return QOS_CLASS_UTILITY;
	case DISPATCH_QOS_BACKGROUND:       return QOS_CLASS_BACKGROUND;
	case DISPATCH_QOS_MAINTENANCE:      return (qos_class_t)QOS_CLASS_MAINTENANCE;
	default: return QOS_CLASS_UNSPECIFIED;
	}
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dispatch_qos_from_queue_priority(intptr_t priority)
{
	switch (priority) {
	case DISPATCH_QUEUE_PRIORITY_BACKGROUND:      return DISPATCH_QOS_BACKGROUND;
	case DISPATCH_QUEUE_PRIORITY_NON_INTERACTIVE: return DISPATCH_QOS_UTILITY;
	case DISPATCH_QUEUE_PRIORITY_LOW:             return DISPATCH_QOS_UTILITY;
	case DISPATCH_QUEUE_PRIORITY_DEFAULT:         return DISPATCH_QOS_DEFAULT;
	case DISPATCH_QUEUE_PRIORITY_HIGH:            return DISPATCH_QOS_USER_INITIATED;
	default: return _dispatch_qos_from_qos_class((qos_class_t)priority);
	}
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dispatch_qos_from_pp(pthread_priority_t pp)
{
#if !TARGET_OS_SIMULATOR
	pp &= _PTHREAD_PRIORITY_QOS_CLASS_MASK;
	pp >>= _PTHREAD_PRIORITY_QOS_CLASS_SHIFT;
	return (dispatch_qos_t)__builtin_ffs((int)pp);
#else
	qos_class_t qos = _pthread_qos_class_decode(pp, NULL, NULL);
	return _dispatch_qos_from_qos_class(qos);
#endif
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dispatch_qos_from_pp_unsafe(pthread_priority_t pp)
{
#if !TARGET_OS_SIMULATOR
	// this assumes we know there is a QOS and pp has been masked off properly
	pp >>= _PTHREAD_PRIORITY_QOS_CLASS_SHIFT;
	DISPATCH_COMPILER_CAN_ASSUME(pp);
	return (dispatch_qos_t)__builtin_ffs((int)pp);
#else
	qos_class_t qos = _pthread_qos_class_decode(pp, NULL, NULL);
	return _dispatch_qos_from_qos_class(qos);
#endif
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_qos_to_pp(dispatch_qos_t qos)
{
#if !TARGET_OS_SIMULATOR
	pthread_priority_t pp = 0;
	if (qos) {
		pp = 1ul << ((qos - 1) + _PTHREAD_PRIORITY_QOS_CLASS_SHIFT);
	}
	return pp | _PTHREAD_PRIORITY_PRIORITY_MASK;
#else
	return _pthread_qos_class_encode(_dispatch_qos_to_qos_class(qos), 0, 0);
#endif
}

// including maintenance
DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_qos_is_background(dispatch_qos_t qos)
{
	return qos && qos <= DISPATCH_QOS_BACKGROUND;
}

#pragma mark pthread_priority_t

#if TARGET_OS_SIMULATOR
// We need to define these fallbacks for the simulator use case - we use the
// definitions provided by the priority_private.h xnu header in the non-simulator
// case
DISPATCH_ALWAYS_INLINE
static inline bool
_pthread_priority_has_qos(pthread_priority_t pp)
{
	return _pthread_qos_class_decode(pp, NULL, NULL) != QOS_CLASS_UNSPECIFIED;
}

DISPATCH_ALWAYS_INLINE
static inline int
_pthread_priority_relpri(pthread_priority_t pp)
{
	int relpri;
	(void) _pthread_qos_class_decode(pp, &relpri, NULL);
	return relpri;
}
#endif

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_pthread_priority_modify_flags(pthread_priority_t pp, unsigned long flags_to_strip,
		unsigned long flags_to_add)
{
#if !TARGET_OS_SIMULATOR
	return (pp & (~flags_to_strip | ~_PTHREAD_PRIORITY_FLAGS_MASK)) | flags_to_add;
#else
	qos_class_t qos; int relpri; unsigned long flags;
	qos = _pthread_qos_class_decode(pp, &relpri, &flags);
	return _pthread_qos_class_encode(qos, relpri, (flags & ~flags_to_strip) | flags_to_add);
#endif
}
// For removing specific flags without adding any
#define _pthread_priority_strip_flags(pp, flags) _pthread_priority_modify_flags(pp, flags, 0)

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_pthread_priority_strip_all_flags(pthread_priority_t pp)
{
#if !TARGET_OS_SIMULATOR
	return (pp & ~_PTHREAD_PRIORITY_FLAGS_MASK);
#else
	qos_class_t qos; int relpri;
	qos = _pthread_qos_class_decode(pp, &relpri, NULL);
	return _pthread_qos_class_encode(qos, relpri, 0);
#endif
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_pthread_priority_strip_relpri(pthread_priority_t pp)
{
#if !TARGET_OS_SIMULATOR
	return (pp & ~_PTHREAD_PRIORITY_PRIORITY_MASK);
#else
	qos_class_t qos; unsigned long flags;
	qos = _pthread_qos_class_decode(pp, NULL, &flags);
	return _pthread_qos_class_encode(qos, 0, flags);
#endif
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_pthread_priority_strip_qos_and_relpri(pthread_priority_t pp)
{
#if !TARGET_OS_SIMULATOR
	return (pp & _PTHREAD_PRIORITY_FLAGS_MASK);
#else
	unsigned long flags;
	(void) _pthread_qos_class_decode(pp, NULL, &flags);
	return _pthread_qos_class_encode(QOS_CLASS_UNSPECIFIED, 0, flags);
#endif
}

#pragma mark dispatch_priority

#define _dispatch_priority_make(qos, relpri) \
	(qos ? ((((qos) << DISPATCH_PRIORITY_QOS_SHIFT) & DISPATCH_PRIORITY_QOS_MASK) | \
	 ((dispatch_priority_t)(relpri - 1) & DISPATCH_PRIORITY_RELPRI_MASK)) : 0)

#define _dispatch_priority_make_override(qos) \
	(((qos) << DISPATCH_PRIORITY_OVERRIDE_SHIFT) & \
	 DISPATCH_PRIORITY_OVERRIDE_MASK)

#define _dispatch_priority_make_floor(qos) \
	(qos ? (_dispatch_priority_make(qos) | DISPATCH_PRIORITY_FLAG_FLOOR) : 0)

#define _dispatch_priority_make_fallback(qos) \
	(qos ? ((((qos) << DISPATCH_PRIORITY_FALLBACK_QOS_SHIFT) & \
	 DISPATCH_PRIORITY_FALLBACK_QOS_MASK) | DISPATCH_PRIORITY_FLAG_FALLBACK) : 0)

DISPATCH_ALWAYS_INLINE
static inline int
_dispatch_priority_relpri(dispatch_priority_t dbp)
{
	if (dbp & DISPATCH_PRIORITY_QOS_MASK) {
		return (int8_t)(dbp & DISPATCH_PRIORITY_RELPRI_MASK) + 1;
	}
	return 0;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dispatch_priority_qos(dispatch_priority_t dbp)
{
	dbp &= DISPATCH_PRIORITY_QOS_MASK;
	return dbp >> DISPATCH_PRIORITY_QOS_SHIFT;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dispatch_priority_fallback_qos(dispatch_priority_t dbp)
{
	dbp &= DISPATCH_PRIORITY_FALLBACK_QOS_MASK;
	return dbp >> DISPATCH_PRIORITY_FALLBACK_QOS_SHIFT;
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_qos_t
_dispatch_priority_override_qos(dispatch_priority_t dbp)
{
	dbp &= DISPATCH_PRIORITY_OVERRIDE_MASK;
	return dbp >> DISPATCH_PRIORITY_OVERRIDE_SHIFT;
}

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_queue_priority_manually_selected(dispatch_priority_t pri)
{
	return !(pri & DISPATCH_PRIORITY_FLAG_INHERITED) &&
			(pri & (DISPATCH_PRIORITY_FLAG_FALLBACK |
			DISPATCH_PRIORITY_FLAG_FLOOR |
			DISPATCH_PRIORITY_REQUESTED_MASK));
}

DISPATCH_ALWAYS_INLINE
static inline dispatch_priority_t
_dispatch_priority_from_pp_impl(pthread_priority_t pp, bool keep_flags)
{
	dispatch_assert(!(pp & _PTHREAD_PRIORITY_SCHED_PRI_FLAG));

#if !TARGET_OS_SIMULATOR
	dispatch_priority_t dbp;
	if (keep_flags) {
		dbp = pp & (DISPATCH_PRIORITY_PTHREAD_PRIORITY_FLAGS_MASK |
				DISPATCH_PRIORITY_RELPRI_MASK);
	} else {
		dbp = pp & DISPATCH_PRIORITY_RELPRI_MASK;
	}

	dbp |= (_dispatch_qos_from_pp(pp) << DISPATCH_PRIORITY_QOS_SHIFT);
	return dbp;
#else
	int relpri; unsigned long flags;
	dispatch_priority_t dbp;

	qos_class_t qos = _pthread_qos_class_decode(pp, &relpri, &flags);

	dbp = _dispatch_priority_make(_dispatch_qos_from_qos_class(qos), relpri);
	if (keep_flags) {
		dbp |= (flags & DISPATCH_PRIORITY_PTHREAD_PRIORITY_FLAGS_MASK);
	}
#endif
	return dbp;
}
#define _dispatch_priority_from_pp(pp) \
		_dispatch_priority_from_pp_impl(pp, true)
#define _dispatch_priority_from_pp_strip_flags(pp) \
		_dispatch_priority_from_pp_impl(pp, false)

#define DISPATCH_PRIORITY_TO_PP_STRIP_FLAGS     0x1
#define DISPATCH_PRIORITY_TO_PP_PREFER_FALLBACK 0x2

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_priority_to_pp_strip_flags(dispatch_priority_t dbp)
{
	dispatch_qos_t qos = _dispatch_priority_qos(dbp);
	pthread_priority_t pp;
#if !TARGET_OS_SIMULATOR
	pp = dbp & DISPATCH_PRIORITY_RELPRI_MASK; // relpri
	if (qos) {
		pp |= (1ul << ((qos - 1) + _PTHREAD_PRIORITY_QOS_CLASS_SHIFT));
	}
#else
	int relpri = _dispatch_priority_relpri(dbp);
	pp = _pthread_qos_class_encode(_dispatch_qos_to_qos_class(qos), relpri, 0);
#endif
	return pp;
}

DISPATCH_ALWAYS_INLINE
static inline pthread_priority_t
_dispatch_priority_to_pp_prefer_fallback(dispatch_priority_t dbp)
{
	dispatch_qos_t qos;
#if !TARGET_OS_SIMULATOR
	pthread_priority_t pp;

	if (dbp & DISPATCH_PRIORITY_FLAG_FALLBACK) {
		pp = dbp & DISPATCH_PRIORITY_PTHREAD_PRIORITY_FLAGS_MASK;
		pp |= _PTHREAD_PRIORITY_PRIORITY_MASK;
		qos = _dispatch_priority_fallback_qos(dbp);

		dispatch_assert(qos != DISPATCH_QOS_UNSPECIFIED);
	} else {
		pp = dbp & (DISPATCH_PRIORITY_PTHREAD_PRIORITY_FLAGS_MASK |
				DISPATCH_PRIORITY_RELPRI_MASK);
		qos = _dispatch_priority_qos(dbp);
		if (unlikely(!qos)) return pp;
	}
	return pp | (1ul << ((qos - 1) + _PTHREAD_PRIORITY_QOS_CLASS_SHIFT));
#else
	unsigned long flags;
	int relpri;

	if (dbp & DISPATCH_PRIORITY_FLAG_FALLBACK) {
		flags = dbp & DISPATCH_PRIORITY_PTHREAD_PRIORITY_FLAGS_MASK;
		relpri = 0;
		qos = _dispatch_priority_fallback_qos(dbp);
	} else {
		flags = dbp & DISPATCH_PRIORITY_PTHREAD_PRIORITY_FLAGS_MASK;
		relpri = _dispatch_priority_relpri(dbp);
		qos = _dispatch_priority_qos(dbp);
	}
	return _pthread_qos_class_encode(_dispatch_qos_to_qos_class(qos), relpri, flags);
#endif
}

#endif // __DISPATCH_SHIMS_PRIORITY__
