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
// in the intial android port.

#ifndef __DISPATCH__ANDROID__STUBS__INTERNAL
#define __DISPATCH__ANDROID__STUBS__INTERNAL

/*
 * Missing sys/queue.h macro stubs
 */

#ifndef TAILQ_FOREACH_SAFE
#	define TAILQ_FOREACH_SAFE(var, head, field, tvar)                      \
    	    for ((var) = TAILQ_FIRST((head));                              \
        	    (var) && ((tvar) = TAILQ_NEXT((var), field), 1);           \
            	(var) = (tvar))
#endif /* TAILQ_FOREACH_SAFE */

#ifndef TRASHIT
#	define TRASHIT(x)      do {(x) = (void *)-1;} while (0)
#endif /* TRASHIT */

#endif /* __DISPATCH__ANDROID__STUBS__INTERNAL */