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

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dispatch/dispatch.h>

#include "dispatch_test.h"
#include <bsdtests.h>

static size_t actual;

void stage1(int stage);
void stage2(void);
void finish(void* cxt);

void
stage1(int stage)
{
	const char *path = "/dev/random";

	int fd = open(path, O_RDONLY);
	if (fd == -1)
	{
		perror(path);
		exit(EXIT_FAILURE);
	}

	dispatch_queue_t main_q = dispatch_get_main_queue();
	test_ptr_notnull("main_q", main_q);

	dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, 0, main_q);
	test_ptr_notnull("select source", source);

	dispatch_source_set_event_handler(source, ^{
		size_t buffer_size = 500*1024;
		char buffer[500*1024];
		size_t sz = read(fd, buffer, buffer_size);
		test_double_less_than_or_equal("kevent read 1", sz, buffer_size+1);
		dispatch_source_cancel(source);
	});

	dispatch_source_set_cancel_handler(source, ^{
		int res = close(fd);
		test_errno("close", res ==  -1 ? errno : 0, 0);
		dispatch_release(source);
		if (stage == 1)
		{
			dispatch_async(dispatch_get_main_queue(), ^{
				stage2();
			});
		}
	});

	if (stage == 3)
	{
		dispatch_set_context(source, source);
		dispatch_set_finalizer_f(source, finish);
	}

	dispatch_resume(source);
}

void
stage2(void)
{
	const char *path = "/usr/share/dict/words";
	struct stat sb;

	int fd = open(path, O_RDONLY);
	if (fd == -1)
	{
		perror(path);
		exit(EXIT_FAILURE);
	}

	if (!dispatch_test_check_evfilt_read_for_fd(fd)) {
		test_skip("EVFILT_READ kevent not firing for test file");
		close(fd);
		dispatch_async(dispatch_get_main_queue(), ^{
			stage1(3);
		});
		return;
	}

	if (fstat(fd, &sb) == -1)
	{
		perror(path);
		exit(EXIT_FAILURE);
	}

	size_t expected = sb.st_size;
	actual = 0;

	dispatch_queue_t main_q = dispatch_get_main_queue();
	test_ptr_notnull("main_q", main_q);

	dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, 0, main_q);
	test_ptr_notnull("kevent source", source);

	dispatch_source_set_event_handler(source, ^{
		size_t est = dispatch_source_get_data(source);
		test_double_less_than_or_equal("estimated", est, expected - actual);
		size_t buffer_size = 500*1024;
		char buffer[500*1024];
		size_t sz = read(fd, buffer, buffer_size);
		actual += sz;
		if (sz < buffer_size)
		{
			sz = read(fd, buffer, buffer_size);
			actual += sz;
			test_long("EOF", sz, 0);
			dispatch_source_cancel(source);
		}
	});

	dispatch_source_set_cancel_handler(source, ^{
		test_long("bytes read", actual, expected);
		int res = close(fd);
		test_errno("close", res ==  -1 ? errno : 0, 0);
		dispatch_release(source);
		dispatch_async(dispatch_get_main_queue(), ^{
			stage1(3);
		});
	});

	dispatch_resume(source);
}

void
finish(void* cxt)
{
	test_ptr("finish", cxt, cxt);
	test_stop();
}

int
main(void)
{
	dispatch_test_start("Dispatch select workaround test"); // <rdar://problem/7678012>

	dispatch_async(dispatch_get_main_queue(), ^{
		stage1(1);
	});

	dispatch_main();
}
