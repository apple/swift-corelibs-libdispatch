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

#include <stdlib.h>

#include <bsdtests.h>
#include "dispatch_test.h"

void
work(void *context __attribute__((unused)))
{
	test_stop();
	exit(0);
}

int
main(void)
{
	dispatch_queue_t q;
	dispatch_test_start("Dispatch (Public) API");
	q = dispatch_get_main_queue();
	test_ptr_notnull("dispatch_get_main_queue", q);

	dispatch_async_f(dispatch_get_main_queue(), NULL, work);
	dispatch_main();
	return 0;
}
