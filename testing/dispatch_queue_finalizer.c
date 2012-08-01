/*
 * Copyright (c) 2008-2011 Apple Inc. All rights reserved.
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

#include <dispatch/dispatch.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <bsdtests.h>
#include "dispatch_test.h"

void *ctxt_magic = NULL;

void
finalize(void *ctxt)
{
	test_ptr_null("finalizer ran", NULL);
	test_ptr("correct context", ctxt, ctxt_magic);
	test_stop();
}

void
never_call(void *ctxt)
{
	test_ptr_notnull("never_call should not run", NULL);
	test_ptr("correct context", ctxt, NULL);
}

int
main(void)
{
	dispatch_test_start("Dispatch Queue Finalizer");

#ifdef __LP64__
	ctxt_magic = (void*)((uintptr_t)arc4random() << 32 | arc4random());
#else
	ctxt_magic = (void*)arc4random();
#endif

	// we need a non-NULL value for the tests to work properly
	if (ctxt_magic == NULL) {
		ctxt_magic = &ctxt_magic;
	}

	dispatch_queue_t q = dispatch_queue_create("com.apple.testing.finalizer", NULL);
	test_ptr_notnull("dispatch_queue_new", q);

	dispatch_set_finalizer_f(q, finalize);

	dispatch_queue_t q_null_context = dispatch_queue_create("com.apple.testing.finalizer.context_null", NULL);

	dispatch_set_context(q_null_context, NULL);
	dispatch_release(q_null_context);

	// Don't test k
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		// Usign async to set the context helps test that blocks are
		// run before the release as opposed to just thrown away.
		dispatch_async(q, ^{
			dispatch_set_context(q, ctxt_magic);
		});

		dispatch_release(q);
	});

	dispatch_main();
	
	return 0;
}
