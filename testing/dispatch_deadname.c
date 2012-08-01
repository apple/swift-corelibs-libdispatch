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
#include <dispatch/private.h>
#include <mach/mach.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#if TEST_MACHPORT_DEBUG
#define test_mach_assume_zero(x) ({kern_return_t _kr = (x); \
		if (_kr) fprintf(stderr, "mach error 0x%x \"%s\": %s\n", \
		_kr, mach_error_string(_kr), #x); _kr; })
void
test_mach_debug_port(mach_port_t name, const char *str)
{
	mach_port_type_t type;
	mach_msg_bits_t ns = 0, nr = 0, nso = 0, nd = 0;
	unsigned int dnreqs = 0, dnrsiz;
	kern_return_t kr = mach_port_type(mach_task_self(), name, &type);

	if (kr) {
		fprintf(stderr, "machport[0x%08x] = { error(0x%x) \"%s\" }: %s\n",
				name, kr, mach_error_string(kr), str);
		return;
	}
	if (type & MACH_PORT_TYPE_SEND) {
		test_mach_assume_zero(mach_port_get_refs(mach_task_self(), name,
				MACH_PORT_RIGHT_SEND, &ns));
	}
	if (type & MACH_PORT_TYPE_SEND_ONCE) {
		test_mach_assume_zero(mach_port_get_refs(mach_task_self(), name,
				MACH_PORT_RIGHT_SEND_ONCE, &nso));
	}
	if (type & MACH_PORT_TYPE_DEAD_NAME) {
		test_mach_assume_zero(mach_port_get_refs(mach_task_self(), name,
				MACH_PORT_RIGHT_DEAD_NAME, &nd));
	}
	if (type & (MACH_PORT_TYPE_RECEIVE|MACH_PORT_TYPE_SEND|
			MACH_PORT_TYPE_SEND_ONCE)) {
		test_mach_assume_zero(mach_port_dnrequest_info(mach_task_self(), name,
				&dnrsiz, &dnreqs));
		}
	if (type & MACH_PORT_TYPE_RECEIVE) {
		mach_port_status_t status = { .mps_pset = 0, };
		mach_msg_type_number_t cnt = MACH_PORT_RECEIVE_STATUS_COUNT;
		test_mach_assume_zero(mach_port_get_refs(mach_task_self(), name,
				MACH_PORT_RIGHT_RECEIVE, &nr));
		test_mach_assume_zero(mach_port_get_attributes(mach_task_self(), name,
				MACH_PORT_RECEIVE_STATUS, (void*)&status, &cnt));
		fprintf(stderr, "machport[0x%08x] = { R(%03u) S(%03u) SO(%03u) D(%03u) "
				"dnreqs(%03u) nsreq(%s) pdreq(%s) srights(%s) sorights(%03u) "
				"qlim(%03u) msgcount(%03u) mkscount(%03u) seqno(%03u) }: %s\n",
				name, nr, ns, nso, nd, dnreqs, status.mps_nsrequest ? "Y":"N",
				status.mps_pdrequest ? "Y":"N", status.mps_srights ? "Y":"N",
				status.mps_sorights, status.mps_qlimit, status.mps_msgcount,
				status.mps_mscount, status.mps_seqno, str);
	} else if (type & (MACH_PORT_TYPE_SEND|MACH_PORT_TYPE_SEND_ONCE|
			MACH_PORT_TYPE_DEAD_NAME)) {
		fprintf(stderr, "machport[0x%08x] = { R(%03u) S(%03u) SO(%03u) D(%03u) "
				"dnreqs(%03u) }: %s\n", name, nr, ns, nso, nd, dnreqs, str);
	} else {
		fprintf(stderr, "machport[0x%08x] = { type(0x%08x) }: %s\n", name, type,
				str);
	}
}
#define test_mach_debug_port(x) test_mach_debug_port(x, __func__)
#else
#define test_mach_debug_port(x)
#endif

static dispatch_group_t g;
static volatile long sent, received;

void
test_dead_name(void)
{
	dispatch_group_enter(g);
	dispatch_async(dispatch_get_global_queue(0, 0), ^{
		dispatch_source_t ds0;
		kern_return_t kr;

		mach_port_t mp = pthread_mach_thread_np(pthread_self());
		assert(mp);
		kr = mach_port_mod_refs(mach_task_self(), mp, MACH_PORT_RIGHT_SEND, 1);
		test_mach_error("mach_port_mod_refs", kr, KERN_SUCCESS);
		ds0 = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_SEND, mp,
				DISPATCH_MACH_SEND_DEAD, dispatch_get_main_queue());
		test_ptr_notnull("DISPATCH_SOURCE_TYPE_MACH_SEND", ds0);
		dispatch_source_set_event_handler(ds0, ^{
			test_long("DISPATCH_MACH_SEND_DEAD",
					dispatch_source_get_handle(ds0), mp);
			dispatch_release(ds0);
			dispatch_group_leave(g);
		});
		dispatch_resume(ds0);

		// give the mgr queue time to start, otherwise the mgr queue will run
		// on this thread, thus defeating the test which assumes that this
		// thread will die.
		usleep(100000);
	});
	dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
}

void
test_register_already_dead_name(void)
{
	dispatch_source_t ds0;
	kern_return_t kr;
	mach_port_t mp;

	dispatch_group_enter(g);
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &mp);
	test_mach_error("mach_port_allocate", kr, KERN_SUCCESS);
	kr = mach_port_insert_right(mach_task_self(), mp, mp,
			MACH_MSG_TYPE_MAKE_SEND);
	test_mach_error("mach_port_insert_right", kr, KERN_SUCCESS);

	kr = mach_port_mod_refs(mach_task_self(), mp,
			MACH_PORT_RIGHT_RECEIVE, -1); // turn send right into dead name
	test_mach_error("mach_port_mod_refs", kr, KERN_SUCCESS);

	ds0 = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_SEND, mp,
			DISPATCH_MACH_SEND_DEAD, dispatch_get_global_queue(0, 0));
	dispatch_source_set_event_handler(ds0, ^{
		test_long("DISPATCH_MACH_SEND_DEAD",
				dispatch_source_get_handle(ds0), mp);
		dispatch_source_cancel(ds0);
		dispatch_release(ds0);
	});
	dispatch_source_set_cancel_handler(ds0, ^{
		kern_return_t kr = mach_port_deallocate(mach_task_self(), mp);
		test_mach_error("mach_port_deallocate", kr, KERN_SUCCESS);
		dispatch_group_leave(g);
	});
	dispatch_resume(ds0);
	dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
}

void
test_receive_and_dead_name(void)
{
	dispatch_source_t ds0, ds;
	kern_return_t kr;
	mach_port_t mp;

	received = 0;
	dispatch_group_enter(g);
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &mp);
	test_mach_error("mach_port_allocate", kr, KERN_SUCCESS);
	kr = mach_port_insert_right(mach_task_self(), mp, mp,
			MACH_MSG_TYPE_MAKE_SEND);
	test_mach_error("mach_port_insert_right", kr, KERN_SUCCESS);
	ds0 = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_SEND, mp,
			DISPATCH_MACH_SEND_DEAD, dispatch_get_global_queue(0, 0));
	dispatch_source_set_event_handler(ds0, ^{
		test_long("DISPATCH_MACH_SEND_DEAD",
				dispatch_source_get_handle(ds0), mp);
		dispatch_source_cancel(ds0);
		dispatch_release(ds0);
	});
	dispatch_source_set_cancel_handler(ds0, ^{
		kern_return_t kr = mach_port_deallocate(mach_task_self(), mp);
		test_mach_error("mach_port_deallocate", kr, KERN_SUCCESS);
		dispatch_group_leave(g);
	});
	dispatch_resume(ds0);
	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, mp, 0,
			dispatch_get_global_queue(0, 0));
	dispatch_source_set_event_handler(ds, ^{
		__sync_add_and_fetch(&received, 1);
		usleep(100000); // rdar://problem/7676437 race with send source re-arm
		mach_msg_empty_rcv_t msg = { .header = {
			.msgh_size = sizeof(mach_msg_empty_rcv_t),
			.msgh_local_port = mp,
		}};
		kern_return_t kr = mach_msg_receive(&msg.header);
		test_mach_error("mach_msg_receive", kr, KERN_SUCCESS);
	});
	dispatch_source_set_cancel_handler(ds, ^{
		kern_return_t kr = mach_port_mod_refs(mach_task_self(), mp,
				MACH_PORT_RIGHT_RECEIVE, -1); // turns send right into dead name
		test_mach_error("mach_port_mod_refs", kr, KERN_SUCCESS);
	});
	dispatch_resume(ds);
	mach_msg_empty_send_t msg = { .header = {
		.msgh_bits = MACH_MSGH_BITS_REMOTE(MACH_MSG_TYPE_COPY_SEND),
		.msgh_size = sizeof(mach_msg_empty_send_t),
		.msgh_remote_port = mp,
	}};
	kr = mach_msg_send(&msg.header);
	test_mach_error("mach_msg_send", kr, KERN_SUCCESS);
	usleep(200000);
	dispatch_source_cancel(ds);
	dispatch_release(ds);
	test_long("DISPATCH_SOURCE_TYPE_MACH_RECV", received, 1);
	if (received > 1 ) {
		test_stop();
	}
	dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
}

#if DISPATCH_API_VERSION >= 20110201 && defined(MACH_NOTIFY_SEND_POSSIBLE) && \
		defined(MACH_SEND_NOTIFY)

#define TEST_SP_MSGCOUNT 11 // (2*qlim)+1

static bool
send_until_timeout(mach_port_t mp)
{
	kern_return_t kr;

	do {
		test_mach_debug_port(mp);
		mach_msg_empty_send_t msg = { .header = {
			.msgh_bits = MACH_MSGH_BITS_REMOTE(MACH_MSG_TYPE_COPY_SEND),
			.msgh_size = sizeof(mach_msg_empty_send_t),
			.msgh_remote_port = mp,
		}};
		kr = mach_msg(&msg.header,
				MACH_SEND_MSG|MACH_SEND_NOTIFY|MACH_SEND_TIMEOUT,
				msg.header.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
				MACH_PORT_NULL);
		if (kr == MACH_SEND_TIMED_OUT) {
			mach_msg_destroy(&msg.header);
			test_mach_error("mach_msg(MACH_SEND_MSG) timed out", kr,
					MACH_SEND_TIMED_OUT);
		} else {
			test_mach_error("mach_msg(MACH_SEND_MSG)", kr, KERN_SUCCESS);
			if (kr) test_stop();
		}
	} while (!kr && __sync_add_and_fetch(&sent, 1) < TEST_SP_MSGCOUNT);
	test_mach_debug_port(mp);
	return kr;
}

void
test_send_possible(void) // rdar://problem/8758200
{
	dispatch_source_t ds0, ds, dsp;
	kern_return_t kr;
	mach_port_t mp;

	sent = 0;
	received = 0;
	dispatch_group_enter(g);
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &mp);
	test_mach_error("mach_port_allocate", kr, KERN_SUCCESS);
	test_mach_debug_port(mp);
	kr = mach_port_insert_right(mach_task_self(), mp, mp,
			MACH_MSG_TYPE_MAKE_SEND);
	test_mach_error("mach_port_insert_right", kr, KERN_SUCCESS);
	test_mach_debug_port(mp);
	ds0 = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_SEND, mp,
			DISPATCH_MACH_SEND_DEAD, dispatch_get_global_queue(0, 0));
	dispatch_source_set_registration_handler(ds0, ^{
		test_long("DISPATCH_MACH_SEND_DEAD registered",
				dispatch_source_get_handle(ds0), mp);
		test_mach_debug_port(mp);
	});
	dispatch_source_set_event_handler(ds0, ^{
		test_long("DISPATCH_MACH_SEND_DEAD delivered",
				dispatch_source_get_handle(ds0), mp);
		test_mach_debug_port(mp);
		dispatch_source_cancel(ds0);
		dispatch_release(ds0);
	});
	dispatch_source_set_cancel_handler(ds0, ^{
		test_long("DISPATCH_MACH_SEND_DEAD canceled",
				dispatch_source_get_handle(ds0), mp);
		test_mach_debug_port(mp);
		kern_return_t kr = mach_port_deallocate(mach_task_self(), mp);
		test_mach_error("mach_port_deallocate", kr, KERN_SUCCESS);
		test_mach_debug_port(mp);
		dispatch_group_leave(g);
	});
	dispatch_resume(ds0);
	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, mp, 0,
			dispatch_get_global_queue(0, 0));
	dispatch_source_set_registration_handler(ds, ^{
		test_long("DISPATCH_SOURCE_TYPE_MACH_RECV registered",
				dispatch_source_get_handle(ds), mp);
		test_mach_debug_port(mp);
	});
	dispatch_source_set_event_handler(ds, ^{
		test_long("DISPATCH_SOURCE_TYPE_MACH_RECV delivered",
				dispatch_source_get_handle(ds), mp);
		kern_return_t kr;
		do {
			test_mach_debug_port(mp);
			usleep(10000); // simulate slow receiver
			mach_msg_empty_rcv_t msg = { .header = {
				.msgh_size = sizeof(mach_msg_empty_rcv_t),
				.msgh_local_port = mp,
			}};
			kr = mach_msg(&msg.header,
					MACH_RCV_MSG|MACH_RCV_TIMEOUT, 0, msg.header.msgh_size,
					msg.header.msgh_local_port, MACH_MSG_TIMEOUT_NONE,
					MACH_PORT_NULL);
			if (kr == MACH_RCV_TIMED_OUT) {
				test_mach_error("mach_msg(MACH_RCV_MSG) timed out", kr,
						MACH_RCV_TIMED_OUT);
			} else {
				test_mach_error("mach_msg(MACH_RCV_MSG)", kr, KERN_SUCCESS);
				if (kr) test_stop();
			}
		} while (!kr && __sync_add_and_fetch(&received, 1));
		test_mach_debug_port(mp);
	});
	dispatch_source_set_cancel_handler(ds, ^{
		test_long("DISPATCH_SOURCE_TYPE_MACH_RECV canceled",
				dispatch_source_get_handle(ds), mp);
		test_mach_debug_port(mp);
		kern_return_t kr = mach_port_mod_refs(mach_task_self(), mp,
				MACH_PORT_RIGHT_RECEIVE, -1); // trigger dead name notification
		test_mach_error("mach_port_mod_refs", kr, KERN_SUCCESS);
		test_mach_debug_port(mp);
	});
	dispatch_resume(ds);
	dsp = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_SEND, mp,
			DISPATCH_MACH_SEND_POSSIBLE, dispatch_get_global_queue(0, 0));
	dispatch_source_set_registration_handler(dsp, ^{
		test_long("DISPATCH_MACH_SEND_POSSIBLE registered",
				dispatch_source_get_handle(dsp), mp);
		if (!send_until_timeout(mp)) {
			dispatch_source_cancel(dsp); // stop sending
			dispatch_release(dsp);
		}
	});
	dispatch_source_set_event_handler(dsp, ^{
		test_long("DISPATCH_MACH_SEND_POSSIBLE delivered",
				dispatch_source_get_handle(dsp), mp);
		if (!send_until_timeout(mp)) {
			dispatch_source_cancel(dsp); // stop sending
			dispatch_release(dsp);
		}
	});
	dispatch_source_set_cancel_handler(dsp, ^{
		test_long("DISPATCH_MACH_SEND_POSSIBLE canceled",
				dispatch_source_get_handle(dsp), mp);
		test_mach_debug_port(mp);
		dispatch_source_cancel(ds); // stop receving
		dispatch_release(ds);
	});
	dispatch_resume(dsp);
	dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
	test_long("DISPATCH_SOURCE_TYPE_MACH_SEND", sent, TEST_SP_MSGCOUNT);
	test_long("DISPATCH_SOURCE_TYPE_MACH_RECV", received, TEST_SP_MSGCOUNT);
}

#else
#define test_send_possible()
#endif

static boolean_t
test_mig_callback(mach_msg_header_t *message __attribute__((unused)),
		mach_msg_header_t *reply)
{
	__sync_add_and_fetch(&received, 1);
	reply->msgh_remote_port = 0;
	return false;
}

void
test_mig_server_large_msg(void) // rdar://problem/8422992
{
	dispatch_source_t ds;
	kern_return_t kr;
	mach_port_t mp;

	received = 0;
	dispatch_group_enter(g);
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &mp);
	test_mach_error("mach_port_allocate", kr, KERN_SUCCESS);
	kr = mach_port_insert_right(mach_task_self(), mp, mp,
			MACH_MSG_TYPE_MAKE_SEND);
	test_mach_error("mach_port_insert_right", kr, KERN_SUCCESS);
	ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, mp, 0,
			dispatch_get_global_queue(0, 0));
	dispatch_source_set_event_handler(ds, ^{
		mach_msg_return_t r = dispatch_mig_server(ds, sizeof(mach_msg_header_t),
				test_mig_callback);
		test_mach_error("dispatch_mig_server", r, MACH_RCV_TOO_LARGE);
		dispatch_group_leave(g);
	});
	dispatch_source_set_cancel_handler(ds, ^{
		kern_return_t kr = mach_port_mod_refs(mach_task_self(), mp,
				MACH_PORT_RIGHT_RECEIVE, -1);
		test_mach_error("mach_port_mod_refs", kr, KERN_SUCCESS);
		kr = mach_port_deallocate(mach_task_self(), mp);
		test_mach_error("mach_port_deallocate", kr, KERN_SUCCESS);
		dispatch_group_leave(g);
	});
	dispatch_resume(ds);

	struct { mach_msg_header_t header; char payload[4096]; } msg = {
		.header = {
			.msgh_bits = MACH_MSGH_BITS_REMOTE(MACH_MSG_TYPE_COPY_SEND),
			.msgh_size = sizeof(mach_msg_header_t) + 4096,
			.msgh_remote_port = mp,
			.msgh_id = 0xfeedface,
		}
	};
	kr = mach_msg_send(&msg.header);
	test_mach_error("mach_msg_send", kr, KERN_SUCCESS);
	dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
	dispatch_group_enter(g);
	dispatch_source_cancel(ds);
	dispatch_release(ds);
	test_long("DISPATCH_SOURCE_TYPE_MACH_RECV", received, 0);
}

int
main(void)
{
	dispatch_test_start("Dispatch dead-name and send-possible notifications");

	dispatch_async(dispatch_get_global_queue(0, 0), ^{
		g = dispatch_group_create();
		test_dead_name();
		test_register_already_dead_name();
		test_receive_and_dead_name();
		test_send_possible();
		test_mig_server_large_msg();
		test_stop();
	});

	dispatch_main();

	return 0;
}
