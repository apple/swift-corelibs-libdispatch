/*
 * Copyright (c) 2015 Apple Inc. All rights reserved.
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

#ifndef __FIREHOSE_SERVER_INTERNAL__
#define __FIREHOSE_SERVER_INTERNAL__

OS_OBJECT_CLASS_DECL(firehose_client, object);
#define FIREHOSE_CLIENT_CLASS OS_OBJECT_VTABLE(firehose_client)

typedef struct firehose_snapshot_s *firehose_snapshot_t;
struct firehose_snapshot_s {
	firehose_snapshot_handler_t handler;
	dispatch_group_t fs_group;
};

struct firehose_client_s {
	union {
		_OS_OBJECT_HEADER(void *os_obj_isa, os_obj_ref_cnt, os_obj_xref_cnt);
		struct _os_object_s fc_as_os_object;
	};
	TAILQ_ENTRY(firehose_client_s) fc_entry;

	firehose_buffer_t	fc_buffer;
	uint64_t volatile	fc_mem_sent_flushed_pos;
	uint64_t volatile	fc_mem_flushed_pos;
	uint64_t volatile	fc_io_sent_flushed_pos;
	uint64_t volatile	fc_io_flushed_pos;

	void *volatile		fc_ctxt;

	union {
		dispatch_mach_t	fc_mach_channel;
		dispatch_source_t fc_kernel_source;
	};
	dispatch_source_t	fc_io_source;
	dispatch_source_t	fc_mem_source;
	mach_port_t			fc_recvp;
	mach_port_t			fc_sendp;
	os_unfair_lock      fc_lock;
	pid_t				fc_pid;
	int					fc_pidversion;
	uid_t				fc_euid;
	bool				fc_use_notifs;
	bool				fc_memory_corrupted;
	bool				fc_needs_io_snapshot;
	bool				fc_needs_mem_snapshot;
};

void
_firehose_client_xref_dispose(struct firehose_client_s *fc);
void
_firehose_client_dispose(struct firehose_client_s *fc);

extern unsigned char __libfirehose_serverVersionString[];
extern double __libfirehose_serverVersionNumber;

#endif // __FIREHOSE_SERVER_INTERNAL__
