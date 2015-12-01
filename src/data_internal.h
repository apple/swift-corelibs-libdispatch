/*
 * Copyright (c) 2009-2012 Apple Inc. All rights reserved.
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
	dispatch_data_t data_object;
	size_t from;
	size_t length;
} range_record;

#if USE_OBJC
#if OS_OBJECT_USE_OBJC
@interface DISPATCH_CLASS(data) : NSObject <DISPATCH_CLASS(data)>
@end
#endif
DISPATCH_OBJC_CLASS_DECL(data);
DISPATCH_OBJC_CLASS_DECL(data_empty);
#define DISPATCH_DATA_CLASS DISPATCH_OBJC_CLASS(data)
#define DISPATCH_DATA_EMPTY_CLASS DISPATCH_OBJC_CLASS(data_empty)
#else // USE_OBJC
DISPATCH_CLASS_DECL(data);
#define DISPATCH_DATA_CLASS DISPATCH_VTABLE(data)
#define DISPATCH_DATA_EMPTY_CLASS DISPATCH_VTABLE(data)
#endif // USE_OBJC

struct dispatch_data_s {
#if USE_OBJC
	const void *do_vtable;
	dispatch_queue_t do_targetq;
	void *ctxt;
	void *finalizer;
#else // USE_OBJC
	DISPATCH_STRUCT_HEADER(data);
#endif // USE_OBJC
	const void *buf;
	dispatch_block_t destructor;
	size_t size, num_records;
	range_record records[0];
};

DISPATCH_ALWAYS_INLINE
static inline bool
_dispatch_data_leaf(struct dispatch_data_s *dd)
{
	return dd->num_records == 0;
}

/*
 * This is about the number of records required to hold that dispatch data
 * if it's not a leaf. Callers either want that value, or have to special
 * case the case when the dispatch data *is* a leaf before (and that the actual
 * embeded record count of that dispatch data is 0)
 */
DISPATCH_ALWAYS_INLINE
static inline size_t
_dispatch_data_num_records(struct dispatch_data_s *dd)
{
	return dd->num_records ?: 1;
}

typedef dispatch_data_t (*dispatch_transform_t)(dispatch_data_t data);

struct dispatch_data_format_type_s {
	uint64_t type;
	uint64_t input_mask;
	uint64_t output_mask;
	dispatch_transform_t decode;
	dispatch_transform_t encode;
};

void dispatch_data_init(dispatch_data_t data, const void *buffer, size_t size,
		dispatch_block_t destructor);
void _dispatch_data_dispose(dispatch_data_t data);
size_t _dispatch_data_debug(dispatch_data_t data, char* buf, size_t bufsiz);
const void*
_dispatch_data_get_flattened_bytes(struct dispatch_data_s *dd);

#if !defined(__cplusplus)
#if !__OBJC2__
const dispatch_block_t _dispatch_data_destructor_inline;
#define DISPATCH_DATA_DESTRUCTOR_INLINE (_dispatch_data_destructor_inline)
#endif // !__OBJC2__

/*
 * the out parameters are about seeing "through" trivial subranges
 * so for something like this: dd = { subrange [ dd1, offset1 ] },
 * this will return { dd1, offset + offset1 }
 *
 * If the dispatch object isn't a trivial subrange, it returns { dd, offset }
 */
DISPATCH_ALWAYS_INLINE
static inline const void*
_dispatch_data_map_direct(struct dispatch_data_s *dd, size_t offset,
		struct dispatch_data_s **dd_out, size_t *from_out)
{
	const void *buffer = NULL;

	dispatch_assert(dd->size);
	if (slowpath(!_dispatch_data_leaf(dd)) &&
			_dispatch_data_num_records(dd) == 1) {
		offset += dd->records[0].from;
		dd = (struct dispatch_data_s *)dd->records[0].data_object;
	}

	if (fastpath(_dispatch_data_leaf(dd))) {
		buffer = dd->buf + offset;
	} else {
		buffer = dispatch_atomic_load((void **)&dd->buf, relaxed);
		if (buffer) {
			buffer += offset;
		}
	}
	if (dd_out) *dd_out = dd;
	if (from_out) *from_out = offset;
	return buffer;
}

#endif // !defined(__cplusplus)

#endif // __DISPATCH_DATA_INTERNAL__
