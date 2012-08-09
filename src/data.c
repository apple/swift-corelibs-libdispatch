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

#include "internal.h"

// Dispatch data objects are dispatch objects with standard retain/release
// memory management. A dispatch data object either points to a number of other
// dispatch data objects or is a leaf data object. A leaf data object contains
// a pointer to represented memory. A composite data object specifies the total
// size of data it represents and list of constituent records.
//
// A leaf data object has a single entry in records[], the object size is the
// same as records[0].length and records[0].from is always 0. In other words, a
// leaf data object always points to a full represented buffer, so a composite
// dispatch data object is needed to represent a subrange of a memory region.

#define _dispatch_data_retain(x) dispatch_retain(x)
#define _dispatch_data_release(x) dispatch_release(x)

#if DISPATCH_DATA_MOVABLE
#if DISPATCH_USE_RESOLVERS && !defined(DISPATCH_RESOLVED_VARIANT)
#error Resolved variant required for movable
#endif
static const dispatch_block_t _dispatch_data_destructor_unlock = ^{
	DISPATCH_CRASH("unlock destructor called");
};
#define DISPATCH_DATA_DESTRUCTOR_UNLOCK (_dispatch_data_destructor_unlock)
#endif

const dispatch_block_t _dispatch_data_destructor_free = ^{
	DISPATCH_CRASH("free destructor called");
};

const dispatch_block_t _dispatch_data_destructor_none = ^{
	DISPATCH_CRASH("none destructor called");
};

const dispatch_block_t _dispatch_data_destructor_vm_deallocate = ^{
	DISPATCH_CRASH("vmdeallocate destructor called");
};

struct dispatch_data_s _dispatch_data_empty = {
	.do_vtable = DISPATCH_VTABLE(data),
	.do_ref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_xref_cnt = DISPATCH_OBJECT_GLOBAL_REFCNT,
	.do_next = DISPATCH_OBJECT_LISTLESS,
};

static dispatch_data_t
_dispatch_data_init(size_t n)
{
	dispatch_data_t data = _dispatch_alloc(DISPATCH_VTABLE(data),
			sizeof(struct dispatch_data_s) + n * sizeof(range_record));
	data->num_records = n;
	data->do_targetq = dispatch_get_global_queue(
			DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
	data->do_next = DISPATCH_OBJECT_LISTLESS;
	return data;
}

static void
_dispatch_data_destroy_buffer(const void* buffer, size_t size,
		dispatch_queue_t queue, dispatch_block_t destructor)
{
	if (destructor == DISPATCH_DATA_DESTRUCTOR_FREE) {
		free((void*)buffer);
	} else if (destructor == DISPATCH_DATA_DESTRUCTOR_NONE) {
		// do nothing
	} else if (destructor == DISPATCH_DATA_DESTRUCTOR_VM_DEALLOCATE) {
		vm_deallocate(mach_task_self(), (vm_address_t)buffer, size);
	} else {
		if (!queue) {
			queue = dispatch_get_global_queue(
					DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
		}
		dispatch_async_f(queue, destructor, _dispatch_call_block_and_release);
	}
}

dispatch_data_t
dispatch_data_create(const void* buffer, size_t size, dispatch_queue_t queue,
		dispatch_block_t destructor)
{
	dispatch_data_t data;
	if (!buffer || !size) {
		// Empty data requested so return the singleton empty object. Call
		// destructor immediately in this case to ensure any unused associated
		// storage is released.
		if (destructor) {
			_dispatch_data_destroy_buffer(buffer, size, queue,
					_dispatch_Block_copy(destructor));
		}
		return dispatch_data_empty;
	}
	data = _dispatch_data_init(1);
	// Leaf objects always point to the entirety of the memory region
	data->leaf = true;
	data->size = size;
	data->records[0].from = 0;
	data->records[0].length = size;
	if (destructor == DISPATCH_DATA_DESTRUCTOR_DEFAULT) {
		// The default destructor was provided, indicating the data should be
		// copied.
		void *data_buf = malloc(size);
		if (slowpath(!data_buf)) {
			free(data);
			return NULL;
		}
		buffer = memcpy(data_buf, buffer, size);
		data->destructor = DISPATCH_DATA_DESTRUCTOR_FREE;
	} else {
		data->destructor = _dispatch_Block_copy(destructor);
#if DISPATCH_DATA_MOVABLE
		// A non-default destructor was provided, indicating the system does not
		// own the buffer. Mark the object as locked since the application has
		// direct access to the buffer and it cannot be reallocated/moved.
		data->locked = 1;
#endif
	}
	data->records[0].data_object = (void*)buffer;
	if (queue) {
		_dispatch_retain(queue);
		data->do_targetq = queue;
	}
	return data;
}

void
_dispatch_data_dispose(dispatch_data_t dd)
{
	dispatch_block_t destructor = dd->destructor;
	if (destructor == NULL) {
		size_t i;
		for (i = 0; i < dd->num_records; ++i) {
			_dispatch_data_release(dd->records[i].data_object);
		}
#if DISPATCH_DATA_MOVABLE
	} else if (destructor == DISPATCH_DATA_DESTRUCTOR_UNLOCK) {
		dispatch_data_t data = (dispatch_data_t)dd->records[0].data_object;
		(void)dispatch_atomic_dec2o(data, locked);
		_dispatch_data_release(data);
#endif
	} else {
		_dispatch_data_destroy_buffer(dd->records[0].data_object,
				dd->records[0].length, dd->do_targetq, destructor);
	}
}

size_t
_dispatch_data_debug(dispatch_data_t dd, char* buf, size_t bufsiz)
{
	size_t offset = 0;
	if (dd->leaf) {
		offset += snprintf(&buf[offset], bufsiz - offset,
				"leaf: %d, size: %zd, data: %p", dd->leaf, dd->size,
				dd->records[0].data_object);
	} else {
		offset += snprintf(&buf[offset], bufsiz - offset,
				"leaf: %d, size: %zd, num_records: %zd", dd->leaf,
				dd->size, dd->num_records);
		size_t i;
		for (i = 0; i < dd->num_records; ++i) {
			range_record r = dd->records[i];
			offset += snprintf(&buf[offset], bufsiz - offset,
					"records[%zd] from: %zd, length %zd, data_object: %p", i,
					r.from, r.length, r.data_object);
		}
	}
	return offset;
}

size_t
dispatch_data_get_size(dispatch_data_t dd)
{
	return dd->size;
}

dispatch_data_t
dispatch_data_create_concat(dispatch_data_t dd1, dispatch_data_t dd2)
{
	dispatch_data_t data;
	if (!dd1->size) {
		_dispatch_data_retain(dd2);
		return dd2;
	}
	if (!dd2->size) {
		_dispatch_data_retain(dd1);
		return dd1;
	}
	data = _dispatch_data_init(dd1->num_records + dd2->num_records);
	data->size = dd1->size + dd2->size;
	// Copy the constituent records into the newly created data object
	memcpy(data->records, dd1->records, dd1->num_records *
			sizeof(range_record));
	memcpy(data->records + dd1->num_records, dd2->records, dd2->num_records *
			sizeof(range_record));
	// Reference leaf objects as sub-objects
	if (dd1->leaf) {
		data->records[0].data_object = dd1;
	}
	if (dd2->leaf) {
		data->records[dd1->num_records].data_object = dd2;
	}
	size_t i;
	for (i = 0; i < data->num_records; ++i) {
		_dispatch_data_retain(data->records[i].data_object);
	}
	return data;
}

dispatch_data_t
dispatch_data_create_subrange(dispatch_data_t dd, size_t offset,
		size_t length)
{
	dispatch_data_t data;
	if (offset >= dd->size || !length) {
		return dispatch_data_empty;
	} else if ((offset + length) > dd->size) {
		length = dd->size - offset;
	} else if (length == dd->size) {
		_dispatch_data_retain(dd);
		return dd;
	}
	if (dd->leaf) {
		data = _dispatch_data_init(1);
		data->size = length;
		data->records[0].from = offset;
		data->records[0].length = length;
		data->records[0].data_object = dd;
		_dispatch_data_retain(dd);
		return data;
	}
	// Subrange of a composite dispatch data object: find the record containing
	// the specified offset
	data = dispatch_data_empty;
	size_t i = 0, bytes_left = length;
	while (i < dd->num_records && offset >= dd->records[i].length) {
		offset -= dd->records[i++].length;
	}
	while (i < dd->num_records) {
		size_t record_len = dd->records[i].length - offset;
		if (record_len > bytes_left) {
			record_len = bytes_left;
		}
		dispatch_data_t subrange = dispatch_data_create_subrange(
				dd->records[i].data_object, dd->records[i].from + offset,
				record_len);
		dispatch_data_t concat = dispatch_data_create_concat(data, subrange);
		_dispatch_data_release(data);
		_dispatch_data_release(subrange);
		data = concat;
		bytes_left -= record_len;
		if (!bytes_left) {
			return data;
		}
		offset = 0;
		i++;
	}
	// Crashing here indicates memory corruption of passed in data object
	DISPATCH_CRASH("dispatch_data_create_subrange out of bounds");
	return NULL;
}

// When mapping a leaf object or a subrange of a leaf object, return a direct
// pointer to the represented buffer. For all other data objects, copy the
// represented buffers into a contiguous area. In the future it might
// be possible to relocate the buffers instead (if not marked as locked).
dispatch_data_t
dispatch_data_create_map(dispatch_data_t dd, const void **buffer_ptr,
		size_t *size_ptr)
{
	dispatch_data_t data = dd;
	void *buffer = NULL;
	size_t size = dd->size, offset = 0;
	if (!size) {
		data = dispatch_data_empty;
		goto out;
	}
	if (!dd->leaf && dd->num_records == 1 &&
			((dispatch_data_t)dd->records[0].data_object)->leaf) {
		offset = dd->records[0].from;
		dd = (dispatch_data_t)(dd->records[0].data_object);
	}
	if (dd->leaf) {
#if DISPATCH_DATA_MOVABLE
		data = _dispatch_data_init(1);
		// Make sure the underlying leaf object does not move the backing buffer
		(void)dispatch_atomic_inc2o(dd, locked);
		data->size = size;
		data->destructor = DISPATCH_DATA_DESTRUCTOR_UNLOCK;
		data->records[0].data_object = dd;
		data->records[0].from = offset;
		data->records[0].length = size;
		_dispatch_data_retain(dd);
#else
		_dispatch_data_retain(data);
#endif
		buffer = dd->records[0].data_object + offset;
		goto out;
	}
	// Composite data object, copy the represented buffers
	buffer = malloc(size);
	if (!buffer) {
		data = NULL;
		size = 0;
		goto out;
	}
	dispatch_data_apply(dd, ^(dispatch_data_t region DISPATCH_UNUSED,
			size_t off, const void* buf, size_t len) {
		memcpy(buffer + off, buf, len);
		return (bool)true;
	});
	data = dispatch_data_create(buffer, size, NULL,
			DISPATCH_DATA_DESTRUCTOR_FREE);
out:
	if (buffer_ptr) {
		*buffer_ptr = buffer;
	}
	if (size_ptr) {
		*size_ptr = size;
	}
	return data;
}

static bool
_dispatch_data_apply(dispatch_data_t dd, size_t offset, size_t from,
		size_t size, dispatch_data_applier_t applier)
{
	bool result = true;
	dispatch_data_t data = dd;
	const void *buffer;
	dispatch_assert(dd->size);
#if DISPATCH_DATA_MOVABLE
	if (dd->leaf) {
		data = _dispatch_data_init(1);
		// Make sure the underlying leaf object does not move the backing buffer
		(void)dispatch_atomic_inc2o(dd, locked);
		data->size = size;
		data->destructor = DISPATCH_DATA_DESTRUCTOR_UNLOCK;
		data->records[0].data_object = dd;
		data->records[0].from = from;
		data->records[0].length = size;
		_dispatch_data_retain(dd);
		buffer = dd->records[0].data_object + from;
		result = applier(data, offset, buffer, size);
		_dispatch_data_release(data);
		return result;
	}
#else
	if (!dd->leaf && dd->num_records == 1 &&
			((dispatch_data_t)dd->records[0].data_object)->leaf) {
		from = dd->records[0].from;
		dd = (dispatch_data_t)(dd->records[0].data_object);
	}
	if (dd->leaf) {
		buffer = dd->records[0].data_object + from;
		return applier(data, offset, buffer, size);
	}
#endif
	size_t i;
	for (i = 0; i < dd->num_records && result; ++i) {
		result = _dispatch_data_apply(dd->records[i].data_object,
				offset, dd->records[i].from, dd->records[i].length,
				applier);
		offset += dd->records[i].length;
	}
	return result;
}

bool
dispatch_data_apply(dispatch_data_t dd, dispatch_data_applier_t applier)
{
	if (!dd->size) {
		return true;
	}
	return _dispatch_data_apply(dd, 0, 0, dd->size, applier);
}

// Returs either a leaf object or an object composed of a single leaf object
dispatch_data_t
dispatch_data_copy_region(dispatch_data_t dd, size_t location,
		size_t *offset_ptr)
{
	if (location >= dd->size) {
		*offset_ptr = 0;
		return dispatch_data_empty;
	}
	dispatch_data_t data;
	size_t size = dd->size, offset = 0, from = 0;
	while (true) {
		if (dd->leaf) {
			_dispatch_data_retain(dd);
			*offset_ptr = offset;
			if (size == dd->size) {
				return dd;
			} else {
				// Create a new object for the requested subrange of the leaf
				data = _dispatch_data_init(1);
				data->size = size;
				data->records[0].from = from;
				data->records[0].length = size;
				data->records[0].data_object = dd;
				return data;
			}
		} else {
			// Find record at the specified location
			size_t i, pos;
			for (i = 0; i < dd->num_records; ++i) {
				pos = offset + dd->records[i].length;
				if (location < pos) {
					size = dd->records[i].length;
					from = dd->records[i].from;
					data = (dispatch_data_t)(dd->records[i].data_object);
					if (dd->num_records == 1 && data->leaf) {
						// Return objects composed of a single leaf node
						*offset_ptr = offset;
						_dispatch_data_retain(dd);
						return dd;
					} else {
						// Drill down into other objects
						dd = data;
						break;
					}
				} else {
					offset = pos;
				}
			}
		}
	}
}
