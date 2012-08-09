/*
 * Copyright (c) 2011 Apple Inc. All rights reserved.
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

#ifndef __DISPATCH_DATA_PRIVATE__
#define __DISPATCH_DATA_PRIVATE__

#ifndef __DISPATCH_INDIRECT__
#error "Please #include <dispatch/dispatch.h> instead of this file directly."
#include <dispatch/base.h> // for HeaderDoc
#endif

__BEGIN_DECLS

#ifdef __BLOCKS__

/*!
 * @const DISPATCH_DATA_DESTRUCTOR_NONE
 * @discussion The destructor for dispatch data objects that require no
 * management. This can be used to allow a data object to efficiently
 * encapsulate data that should not be copied or freed by the system.
 */
#define DISPATCH_DATA_DESTRUCTOR_NONE (_dispatch_data_destructor_none)
__OSX_AVAILABLE_STARTING(__MAC_10_8, __IPHONE_6_0)
DISPATCH_EXPORT const dispatch_block_t _dispatch_data_destructor_none;

/*!
 * @const DISPATCH_DATA_DESTRUCTOR_VM_DEALLOCATE
 * @discussion The destructor for dispatch data objects that have been created
 * from buffers that require deallocation using vm_deallocate.
 */
#define DISPATCH_DATA_DESTRUCTOR_VM_DEALLOCATE \
		(_dispatch_data_destructor_vm_deallocate)
__OSX_AVAILABLE_STARTING(__MAC_10_8, __IPHONE_6_0)
DISPATCH_EXPORT const dispatch_block_t _dispatch_data_destructor_vm_deallocate;

/*!
 * @typedef dispatch_data_format_type_t
 *
 * @abstract
 * Data formats are used to specify the input and output types of data supplied
 * to dispatch_data_create_transform.
 */
typedef const struct dispatch_data_format_type_s *dispatch_data_format_type_t;

/*!
 * @const DISPATCH_DATA_FORMAT_TYPE_NONE
 * @discussion A data format denoting that the given input or output format is,
 * or should be, comprised of raw data bytes with no given encoding.
 */
#define DISPATCH_DATA_FORMAT_TYPE_NONE (&_dispatch_data_format_type_none)
__OSX_AVAILABLE_STARTING(__MAC_10_8, __IPHONE_6_0)
DISPATCH_EXPORT
const struct dispatch_data_format_type_s _dispatch_data_format_type_none;

/*!
 * @const DISPATCH_DATA_FORMAT_TYPE_BASE32
 * @discussion A data format denoting that the given input or output format is,
 * or should be, encoded in Base32 (RFC 4648) format. On input, this format will
 * skip whitespace characters. Cannot be used in conjunction with UTF format
 * types.
 */
#define DISPATCH_DATA_FORMAT_TYPE_BASE32 (&_dispatch_data_format_type_base32)
__OSX_AVAILABLE_STARTING(__MAC_10_8, __IPHONE_6_0)
DISPATCH_EXPORT
const struct dispatch_data_format_type_s _dispatch_data_format_type_base32;

/*!
 * @const DISPATCH_DATA_FORMAT_TYPE_BASE64
 * @discussion A data format denoting that the given input or output format is,
 * or should be, encoded in Base64 (RFC 4648) format. On input, this format will
 * skip whitespace characters. Cannot be used in conjunction with UTF format
 * types.
 */
#define DISPATCH_DATA_FORMAT_TYPE_BASE64 (&_dispatch_data_format_type_base64)
__OSX_AVAILABLE_STARTING(__MAC_10_8, __IPHONE_6_0)
DISPATCH_EXPORT
const struct dispatch_data_format_type_s _dispatch_data_format_type_base64;

/*!
 * @const DISPATCH_DATA_FORMAT_TYPE_UTF8
 * @discussion A data format denoting that the given input or output format is,
 * or should be, encoded in UTF-8 format. Is only valid when used in conjunction
 * with other UTF format types.
 */
#define DISPATCH_DATA_FORMAT_TYPE_UTF8 (&_dispatch_data_format_type_utf8)
__OSX_AVAILABLE_STARTING(__MAC_10_8, __IPHONE_6_0)
DISPATCH_EXPORT
const struct dispatch_data_format_type_s _dispatch_data_format_type_utf8;

/*!
 * @const DISPATCH_DATA_FORMAT_TYPE_UTF16LE
 * @discussion A data format denoting that the given input or output format is,
 * or should be, encoded in UTF-16LE format. Is only valid when used in
 * conjunction with other UTF format types.
 */
#define DISPATCH_DATA_FORMAT_TYPE_UTF16LE (&_dispatch_data_format_type_utf16le)
__OSX_AVAILABLE_STARTING(__MAC_10_8, __IPHONE_6_0)
DISPATCH_EXPORT
const struct dispatch_data_format_type_s _dispatch_data_format_type_utf16le;

/*!
 * @const DISPATCH_DATA_FORMAT_TYPE_UTF16BE
 * @discussion A data format denoting that the given input or output format is,
 * or should be, encoded in UTF-16BE format. Is only valid when used in
 * conjunction with other UTF format types.
 */
#define DISPATCH_DATA_FORMAT_TYPE_UTF16BE (&_dispatch_data_format_type_utf16be)
__OSX_AVAILABLE_STARTING(__MAC_10_8, __IPHONE_6_0)
DISPATCH_EXPORT
const struct dispatch_data_format_type_s _dispatch_data_format_type_utf16be;

/*!
 * @const DISPATCH_DATA_FORMAT_TYPE_UTFANY
 * @discussion A data format denoting that dispatch_data_create_transform should
 * attempt to automatically detect the input type based on the presence of a
 * byte order mark character at the beginning of the data. In the absence of a
 * BOM, the data will be assumed to be in UTF-8 format. Only valid as an input
 * format.
 */
#define DISPATCH_DATA_FORMAT_TYPE_UTF_ANY (&_dispatch_data_format_type_utf_any)
__OSX_AVAILABLE_STARTING(__MAC_10_8, __IPHONE_6_0)
DISPATCH_EXPORT
const struct dispatch_data_format_type_s _dispatch_data_format_type_utf_any;

/*!
 * @function dispatch_data_create_transform
 * Returns a new dispatch data object after transforming the given data object
 * from the supplied format, into the given output format.
 *
 * @param data
 * The data object representing the region(s) of memory to transform.
 * @param input_type
 * Flags specifying the input format of the source dispatch_data_t
 *
 * @param output_type
 * Flags specifying the expected output format of the resulting transfomation.
 *
 * @result
 * A newly created dispatch data object, dispatch_data_empty if no has been
 * produced, or NULL if an error occurred.
 */

__OSX_AVAILABLE_STARTING(__MAC_10_8, __IPHONE_6_0)
DISPATCH_EXPORT DISPATCH_NONNULL_ALL DISPATCH_RETURNS_RETAINED
DISPATCH_WARN_RESULT DISPATCH_NOTHROW
dispatch_data_t
dispatch_data_create_with_transform(dispatch_data_t data,
	dispatch_data_format_type_t input_type,
	dispatch_data_format_type_t output_type);

#endif /* __BLOCKS__ */

__END_DECLS

#endif // __DISPATCH_DATA_PRIVATE__
