/*
 * This source file is part of the Swift.org open source project
 *
 * Copyright (c) 2015 Apple Inc. and the Swift project authors
 *
 * Licensed under Apache License v2.0 with Runtime Library Exception
 *
 * See http://swift.org/LICENSE.txt for license information
 * See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
 *
 */

// forward declarations for functions we are stubbing out
// in the intial linux port.

#ifndef __DISPATCH__STUBS__INTERNAL
#define __DISPATCH__STUBS__INTERNAL

#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, temp)                         \
	for ((var) = TAILQ_FIRST((head));                                      \
		(var) && ((temp) = TAILQ_NEXT((var), field), 1); (var) = (temp))
#endif

#if DISPATCH_DEBUG
#ifndef TRASHIT
#define TRASHIT(x) do { (x) = (void *)-1; } while (0)
#endif
#endif

/*
 * Stub out defines for some mach types and related macros
 */

typedef uint32_t mach_port_t;

#define  MACH_PORT_NULL (0)
#define  MACH_PORT_DEAD (-1)

typedef uint32_t mach_error_t;

typedef uint32_t mach_msg_return_t;

typedef uint32_t mach_msg_bits_t;

typedef void *dispatch_mach_msg_t;

typedef uint64_t firehose_activity_id_t;

typedef void *mach_msg_header_t;

// Print a warning when an unported code path executes.
#define LINUX_PORT_ERROR()  do { \
		printf("LINUX_PORT_ERROR_CALLED %s:%d: %s\n",\
		__FILE__,__LINE__,__FUNCTION__); } while (0)

/*
 * Stub out defines for other missing types
 */

// SIZE_T_MAX should not be hardcoded like this here.
#ifndef SIZE_T_MAX
#define SIZE_T_MAX (~(size_t)0)
#endif

#endif
