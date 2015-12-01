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

#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <dispatch/dispatch.h>

#include <bsdtests.h>
#include "dispatch_test.h"

static size_t bytes_total;
static size_t bytes_read;

void
test_fin(void *cxt)
{
	test_ptr("test_fin run", cxt, cxt);
	test_stop();
}

int
main(void)
{
	const char *path = "/usr/share/dict/words";
	struct stat sb;

	dispatch_test_start("Dispatch Source Read");

	int infd = open(path, O_RDONLY);
	if (infd == -1) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	if (fstat(infd, &sb) == -1) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	bytes_total = sb.st_size;

	if (fcntl(infd, F_SETFL, O_NONBLOCK) != 0) {
		perror(path);
		exit(EXIT_FAILURE);
	}

	if (!dispatch_test_check_evfilt_read_for_fd(infd)) {
		test_skip("EVFILT_READ kevent not firing for test file");
		test_fin(NULL);
	}

	dispatch_queue_t main_q = dispatch_get_main_queue();
	test_ptr_notnull("dispatch_get_main_queue", main_q);

	dispatch_source_t reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, infd, 0, main_q);
	test_ptr_notnull("dispatch_source_create", reader);
	assert(reader);

	dispatch_source_set_event_handler(reader, ^{
		size_t estimated = dispatch_source_get_data(reader);
		fprintf(stderr, "bytes available: %zu\n", estimated);
		test_double_less_than_or_equal("estimated", estimated, bytes_total - bytes_read);
		const ssize_t bufsiz = 1024*500; // 500 KB buffer
		static char buffer[1024*500];	// 500 KB buffer
		ssize_t actual = read(infd, buffer, sizeof(buffer));
		bytes_read += actual;
		printf("bytes read: %zd\n", actual);
		if (actual < bufsiz) {
			actual = read(infd, buffer, sizeof(buffer));
			bytes_read += actual;
			// confirm EOF condition
			test_long("EOF", actual, 0);
			dispatch_source_cancel(reader);
		}
	});

	dispatch_source_set_cancel_handler(reader, ^{
		test_long("Bytes read", bytes_read, bytes_total);
		int res = close(infd);
		test_errno("close", res == -1 ? errno : 0, 0);
		dispatch_release(reader);
	});

	dispatch_set_context(reader, reader);
	dispatch_set_finalizer_f(reader, test_fin);

	dispatch_resume(reader);

	dispatch_main();
}
