/*
 * Copyright (c) 2010-2011 Apple Inc. All rights reserved.
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
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#if DISPATCH_API_VERSION >= 20100825 && DISPATCH_API_VERSION != 20101110

static char *ctxts[] = {"ctxt for app", "ctxt for key 1",
		"ctxt for key 2", "ctxt for key 1 bis", "ctxt for key 4"};
volatile long ctxts_destroyed;
static dispatch_group_t g;

static void
destructor(void *ctxt)
{
	fprintf(stderr, "destructor of %s\n", (char*)ctxt);
	(void)__sync_add_and_fetch(&ctxts_destroyed, 1);
	dispatch_group_leave(g);
}

static void
test_context_for_key(void)
{
	g = dispatch_group_create();
	dispatch_queue_t q = dispatch_queue_create("q", NULL);
#if DISPATCH_API_VERSION >= 20100518
	dispatch_queue_t tq = dispatch_queue_create("tq", DISPATCH_QUEUE_CONCURRENT);
#else
	dispatch_queue_t tq = dispatch_queue_create("tq", NULL);
	dispatch_queue_set_width(tq, LONG_MAX);
#endif
	dispatch_queue_t ttq = dispatch_get_global_queue(0, 0);
	dispatch_group_enter(g);
#if DISPATCH_API_VERSION >= 20101011
	dispatch_queue_set_specific(tq, &ctxts[4], ctxts[4], destructor);
#else
	dispatch_set_context_for_key(tq, &ctxts[4], ctxts[4], ttq, destructor);
#endif
	dispatch_set_target_queue(tq, ttq);
	dispatch_group_enter(g);
	dispatch_set_context(q, ctxts[0]);
	dispatch_set_target_queue(q, tq);
	dispatch_set_finalizer_f(q, destructor);

	dispatch_async(q, ^{
		dispatch_group_enter(g);
#if DISPATCH_API_VERSION >= 20101011
		dispatch_queue_set_specific(q, &ctxts[1], ctxts[1], destructor);
#else
		dispatch_set_context_for_key(q, &ctxts[1], ctxts[1], ttq, destructor);
#endif
	});
	dispatch_retain(q);
	dispatch_async(dispatch_get_global_queue(0, 0), ^{
		dispatch_group_enter(g);
#if DISPATCH_API_VERSION >= 20101011
		dispatch_queue_set_specific(q, &ctxts[2], ctxts[2], destructor);
#else
		dispatch_set_context_for_key(q, &ctxts[2], ctxts[2], ttq, destructor);
#endif
		dispatch_async(dispatch_get_global_queue(0, 0), ^{
			void *ctxt;
#if DISPATCH_API_VERSION >= 20101011
			ctxt = dispatch_queue_get_specific(q, &ctxts[2]);
#else
			ctxt = dispatch_get_context_for_key(q, &ctxts[2]);
#endif
			test_ptr("get context for key 2", ctxt, ctxts[2]);
			dispatch_release(q);
		});
	});
	dispatch_async(q, ^{
		void *ctxt;
#if DISPATCH_API_VERSION >= 20101011
		ctxt = dispatch_get_specific(&ctxts[1]);
		test_ptr("get current context for key 1", ctxt, ctxts[1]);
		ctxt = dispatch_get_specific(&ctxts[4]);
		test_ptr("get current context for key 4 (on target queue)", ctxt, ctxts[4]);
		ctxt = dispatch_queue_get_specific(q, &ctxts[1]);
#else
		ctxt = dispatch_get_context_for_key(tq, &ctxts[4]);
		test_ptr("get context for key 4 (on target queue)", ctxt, ctxts[4]);
		ctxt = dispatch_get_context_for_key(q, &ctxts[1]);
#endif
		test_ptr("get context for key 1", ctxt, ctxts[1]);
	});
	dispatch_async(q, ^{
		dispatch_group_enter(g);
		void *ctxt;
#if DISPATCH_API_VERSION >= 20101011
		dispatch_queue_set_specific(q, &ctxts[1], ctxts[3], destructor);
		ctxt = dispatch_queue_get_specific(q, &ctxts[1]);
#else
		dispatch_set_context_for_key(q, &ctxts[1], ctxts[3], ttq, destructor);
		ctxt = dispatch_get_context_for_key(q, &ctxts[1]);
#endif
		test_ptr("get context for key 1", ctxt, ctxts[3]);
	});
	dispatch_async(q, ^{
		void *ctxt;
#if DISPATCH_API_VERSION >= 20101011
		dispatch_queue_set_specific(q, &ctxts[1], NULL, destructor);
		ctxt = dispatch_queue_get_specific(q, &ctxts[1]);
#else
		dispatch_set_context_for_key(q, &ctxts[1], NULL, ttq, destructor);
		ctxt = dispatch_get_context_for_key(q, &ctxts[1]);
#endif
		test_ptr("get context for key 1", ctxt, NULL);
	});
	void *ctxt = dispatch_get_context(q);
	test_ptr("get context for app", ctxt, ctxts[0]);
	dispatch_release(tq);
	dispatch_release(q);
	dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
	test_long("contexts destroyed", ctxts_destroyed, 5);
	dispatch_release(g);
}
#endif

int
main(void)
{
	dispatch_test_start("Dispatch Queue Specific"); // rdar://problem/8429188

	dispatch_async(dispatch_get_main_queue(), ^{
#if DISPATCH_API_VERSION >= 20100825 && DISPATCH_API_VERSION != 20101110
		test_context_for_key();
#endif
		test_stop();
	});
	dispatch_main();
}
