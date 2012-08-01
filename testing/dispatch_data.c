/*
 * Copyright (c) 2009-2011 Apple Inc. All rights reserved.
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

#include <dispatch/dispatch.h>
#include <TargetConditionals.h>

#include <bsdtests.h>
#include "dispatch_test.h"

#ifndef DISPATCHTEST_DATA
#if DISPATCH_API_VERSION >= 20100226 && DISPATCH_API_VERSION != 20101110
#define DISPATCHTEST_DATA 1
#endif
#endif

dispatch_group_t g;

#if DISPATCHTEST_DATA
static void
test_concat(void)
{
	dispatch_group_enter(g);
	dispatch_async(dispatch_get_main_queue(), ^{
		char* buffer1 = "This is buffer1 ";
		size_t size1 = 17;
		char* buffer2 = "This is buffer2 ";
		size_t size2 = 17;
		__block bool buffer2_destroyed = false;

		dispatch_data_t data1 = dispatch_data_create(buffer1, size1, NULL, NULL);
		dispatch_data_t data2 = dispatch_data_create(buffer2, size2,
					dispatch_get_main_queue(), ^{
			buffer2_destroyed = true;
		});
		dispatch_data_t concat = dispatch_data_create_concat(data1, data2);

		dispatch_release(data1);
		dispatch_release(data2);

		test_long("Data size of concatenated dispatch data",
				dispatch_data_get_size(concat), 34);

		const void* contig;
		size_t contig_size;
		dispatch_data_t contig_data =
			dispatch_data_create_map(concat, &contig, &contig_size);

		dispatch_release(concat);
		dispatch_release(contig_data);
		test_long("Contiguous memory size", contig_size, 34);
		dispatch_async(dispatch_get_main_queue(), ^{
			test_long("buffer2 destroyed", buffer2_destroyed, true);
			dispatch_group_leave(g);
		});
	});
}

static void
test_cleanup(void) // <rdar://problem/9843440>
{
	dispatch_group_enter(g);
	dispatch_async(dispatch_get_main_queue(), ^{
		void *buffer3 = malloc(1024);
		dispatch_data_t data3 = dispatch_data_create(buffer3, 0,
				dispatch_get_main_queue(), DISPATCH_DATA_DESTRUCTOR_FREE);
		__block bool buffer4_destroyed = false;
		dispatch_data_t data4 = dispatch_data_create(NULL, 1024,
				dispatch_get_main_queue(), ^{
			buffer4_destroyed = true;
		});
		dispatch_release(data3);
		dispatch_release(data4);
		dispatch_async(dispatch_get_main_queue(), ^{
			test_long("buffer4 destroyed", buffer4_destroyed, true);
			dispatch_group_leave(g);
		});
	});
}
#endif

int
main(void)
{
	dispatch_test_start("Dispatch Data");
	g = dispatch_group_create();

#if DISPATCHTEST_DATA
	test_concat();
	test_cleanup();
#endif

	dispatch_group_notify(g, dispatch_get_main_queue(), ^{
		dispatch_release(g);
		test_stop();
	});
	dispatch_main();
}
