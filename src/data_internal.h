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

/*
 * IMPORTANT: This header file describes INTERNAL interfaces to libdispatch
 * which are subject to change in future releases of Mac OS X. Any applications
 * relying on these interfaces WILL break.
 */

#ifndef __DISPATCH_DATA_INTERNAL__
#define __DISPATCH_DATA_INTERNAL__

#ifndef __DISPATCH_INDIRECT__
#error "Please #include <dispatch/dispatch.h> instead of this file directly."
#include <dispatch/base.h> // for HeaderDoc
#endif

typedef struct range_record_s {
	void* data_object;
	size_t from;
	size_t length;
} range_record;

DISPATCH_CLASS_DECL(data);
struct dispatch_data_s {
	DISPATCH_STRUCT_HEADER(data);
#if DISPATCH_DATA_MOVABLE
	unsigned int locked;
#endif
	bool leaf;
	dispatch_block_t destructor;
	size_t size, num_records;
	range_record records[];
};

typedef dispatch_data_t (*dispatch_transform_t)(dispatch_data_t data);

struct dispatch_data_format_type_s {
	uint64_t type;
	uint64_t input_mask;
	uint64_t output_mask;
	dispatch_transform_t decode;
	dispatch_transform_t encode;
};

void _dispatch_data_dispose(dispatch_data_t data);
size_t _dispatch_data_debug(dispatch_data_t data, char* buf, size_t bufsiz);

#endif // __DISPATCH_DATA_INTERNAL__
