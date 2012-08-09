/*
 * Copyright (c) 2011-2012 Apple Inc. All rights reserved.
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

#include <bsdtests.h>

#if DISPATCH_API_VERSION >= 20111008 && !TARGET_OS_EMBEDDED

#include <Security/Security.h>

#include <dispatch/dispatch.h>
#include <dispatch/private.h>
#include <fcntl.h>

#define printf_data(p, s) ({ \
	typeof(s) _i; \
	for (_i=0; _i<s; _i++) { \
		printf("%c", ((uint8_t *)p)[_i]); \
	} \
	printf("\n"); \
})

#define test_data_equal(a, b, c)	({ \
	const void * ptr, * ptr2; \
	size_t size, size2; \
	dispatch_data_t map = dispatch_data_create_map(b, &ptr, &size); \
	assert(map); \
	dispatch_data_t map2 = dispatch_data_create_map(c, &ptr2, &size2); \
	assert(map); \
	test_long(a ": length", size, size2); \
	test_long(a ": memcmp", memcmp(ptr, ptr2, size), 0); \
	if (size != size2 || (memcmp(ptr, ptr2, size) != 0)) { \
		printf_data(ptr, size); \
		printf_data(ptr2, size2); \
	} \
	dispatch_release(map); \
	dispatch_release(map2); \
})

static bool
dispatch_data_equal(dispatch_data_t a, dispatch_data_t b)
{
	const void * ptr, * ptr2;
	size_t size, size2;
	bool equal = true;

	dispatch_data_t map = dispatch_data_create_map(a, &ptr, &size); \
	assert(map);
	dispatch_data_t map2 = dispatch_data_create_map(b, &ptr2, &size2); \
	assert(map2);

	if (size == size2) {
		if (memcmp(ptr, ptr2, size) != 0) {
			equal = false;
		}
	} else {
		equal = false;
	}
	dispatch_release(map);
	dispatch_release(map2);
	return equal;
}

static dispatch_data_t
execute_sectransform(SecTransformRef transformRef, dispatch_data_t data)
{
	const void * bytes;
	size_t size;

	dispatch_data_t map = dispatch_data_create_map(data, &bytes, &size);
	assert(map);

	CFDataRef dataRef = CFDataCreate(kCFAllocatorDefault, bytes, size);
	assert(dataRef);

	dispatch_release(map);

	SecTransformSetAttribute(transformRef, kSecTransformInputAttributeName, dataRef, NULL);

	CFDataRef transformedDataRef = SecTransformExecute(transformRef, NULL);
	assert(transformedDataRef);

	CFRelease(dataRef);

	dispatch_data_t output = dispatch_data_create(CFDataGetBytePtr(transformedDataRef), CFDataGetLength(transformedDataRef), dispatch_get_main_queue(), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	CFRelease(transformedDataRef);

	return output;
}

#pragma mark - UTF tests

static uint8_t utf8[] = {
	0x53, 0x6f, 0x20, 0x6c, 0x6f, 0x6e, 0x67, 0x20, 0x61, 0x6e, 0x64, 0x20, 0x74, 0x68, 0x61, 0x6e, 0x6b, 0x73, 0x20, 0x66, 0x6f,
	0x72, 0x20, 0x61, 0x6c, 0x6c, 0x20, 0x74, 0x68, 0x65, 0x20, 0x66, 0x69, 0x73, 0x68, 0x2e, 0x20, 0xeb, 0x84, 0x88, 0xeb, 0xac,
	0xb4, 0x20, 0xec, 0x98, 0xa4, 0xeb, 0x9e, 0x98, 0x20, 0xea, 0xb7, 0xb8, 0xeb, 0xa6, 0xac, 0xea, 0xb3, 0xa0, 0x20, 0xea, 0xb7,
	0xb8, 0x20, 0xeb, 0x8f, 0x99, 0xec, 0x95, 0x88, 0x20, 0xeb, 0xa7, 0x9b, 0xec, 0x9e, 0x88, 0xeb, 0x8a, 0x94, 0x20, 0xec, 0x83,
	0x9d, 0xec, 0x84, 0xa0, 0xec, 0x9d, 0x80, 0x20, 0xea, 0xb3, 0xa0, 0xeb, 0xa7, 0x88, 0xec, 0x9b, 0xa0, 0xec, 0x96, 0xb4, 0x2e,
	0x20, 0xf0, 0x9f, 0x98, 0x84, 0xf0, 0x9f, 0x98, 0x8a, 0xf0, 0x9f, 0x98, 0x83, 0xe2, 0x98, 0xba, 0xf0, 0x9f, 0x98, 0x89, 0xf0,
	0x9f, 0x98, 0x8d, 0xf0, 0x9f, 0x92, 0xa8, 0xf0, 0x9f, 0x92, 0xa9, 0xf0, 0x9f, 0x91, 0x8e, 0x2e,
};

static uint16_t utf16[] = {
	0xfeff, 0x53, 0x6f, 0x20, 0x6c, 0x6f, 0x6e, 0x67, 0x20, 0x61, 0x6e, 0x64, 0x20, 0x74, 0x68, 0x61, 0x6e, 0x6b, 0x73, 0x20, 0x66,
	0x6f, 0x72, 0x20, 0x61, 0x6c, 0x6c, 0x20, 0x74, 0x68, 0x65, 0x20, 0x66, 0x69, 0x73, 0x68, 0x2e, 0x20, 0xb108, 0xbb34, 0x20,
	0xc624, 0xb798, 0x20, 0xadf8, 0xb9ac, 0xace0, 0x20, 0xadf8, 0x20, 0xb3d9, 0xc548, 0x20, 0xb9db, 0xc788, 0xb294, 0x20, 0xc0dd,
	0xc120, 0xc740, 0x20, 0xace0, 0xb9c8, 0xc6e0, 0xc5b4, 0x2e, 0x20, 0xd83d, 0xde04, 0xd83d, 0xde0a, 0xd83d, 0xde03, 0x263a, 0xd83d,
	0xde09, 0xd83d, 0xde0d, 0xd83d, 0xdca8, 0xd83d, 0xdca9, 0xd83d, 0xdc4e, 0x2e,
};

static uint16_t utf16be[] = {
	0xfffe, 0x5300, 0x6f00, 0x2000, 0x6c00, 0x6f00, 0x6e00, 0x6700, 0x2000, 0x6100, 0x6e00, 0x6400, 0x2000, 0x7400, 0x6800, 0x6100,
	0x6e00, 0x6b00, 0x7300, 0x2000, 0x6600, 0x6f00, 0x7200, 0x2000, 0x6100, 0x6c00, 0x6c00, 0x2000, 0x7400, 0x6800, 0x6500, 0x2000,
	0x6600, 0x6900, 0x7300, 0x6800, 0x2e00, 0x2000, 0x8b1, 0x34bb, 0x2000, 0x24c6, 0x98b7, 0x2000, 0xf8ad, 0xacb9, 0xe0ac, 0x2000,
	0xf8ad, 0x2000, 0xd9b3, 0x48c5, 0x2000, 0xdbb9, 0x88c7, 0x94b2, 0x2000, 0xddc0, 0x20c1, 0x40c7, 0x2000, 0xe0ac, 0xc8b9, 0xe0c6,
	0xb4c5, 0x2e00, 0x2000, 0x3dd8, 0x4de, 0x3dd8, 0xade, 0x3dd8, 0x3de, 0x3a26, 0x3dd8, 0x9de, 0x3dd8, 0xdde, 0x3dd8, 0xa8dc,
	0x3dd8, 0xa9dc, 0x3dd8, 0x4edc, 0x2e00,
};

// Invalid due to half missing surrogate
static uint16_t utf16le_invalid[] = {
	0xfeff, 0x53, 0x6f, 0x20, 0x6c, 0x6f, 0x6e, 0x67, 0x20, 0x61, 0x6e, 0x64, 0x20, 0x74, 0x68, 0x61, 0x6e, 0x6b, 0x73, 0x20, 0x66,
	0x6f, 0x72, 0x20, 0x61, 0x6c, 0x6c, 0x20, 0x74, 0x68, 0x65, 0x20, 0x66, 0x69, 0x73, 0x68, 0x2e, 0x20, 0xb108, 0xbb34, 0x20,
	0xc624, 0xb798, 0x20, 0xadf8, 0xb9ac, 0xace0, 0x20, 0xadf8, 0x20, 0xb3d9, 0xc548, 0x20, 0xb9db, 0xc788, 0xb294, 0x20, 0xc0dd,
	0xc120, 0xc740, 0x20, 0xace0, 0xb9c8, 0xc6e0, 0xc5b4, 0x2e, 0x20, 0xd83d, 0xde04, 0xd83d, 0xde0a, 0xd83d, 0xde03, 0x263a, 0xd83d,
	0xde09, 0xd83d, 0xde0d, 0xd83d, 0xdca8, 0xd83d, 0xd83d, 0xdc4e, 0x2e,
};

void
invalid_utf8_test(void * context)
{
	dispatch_data_t utf8_data = dispatch_data_create(utf8 + sizeof(utf8) - 8, 8, NULL, ^{});

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf8_data, DISPATCH_DATA_FORMAT_TYPE_UTF8, DISPATCH_DATA_FORMAT_TYPE_UTF16LE);
	test_ptr_null("dispatch_data_create_with_transform (UTF8 (invalid start) -> UTF16LE)", transformed);

	dispatch_release(utf8_data);

	(void)context;
}

void
truncated_utf8_test(void * context)
{
	dispatch_data_t utf8_data = dispatch_data_create(utf8, sizeof(utf8) - 3, NULL, ^{});

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf8_data, DISPATCH_DATA_FORMAT_TYPE_UTF8, DISPATCH_DATA_FORMAT_TYPE_UTF16LE);
	test_ptr_null("dispatch_data_create_with_transform (UTF8 (truncated) -> UTF16LE)", transformed);

	dispatch_release(utf8_data);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, invalid_utf8_test);
}

void
invalid_utf16le_surrogate_test(void * context)
{
	dispatch_data_t utf8_data = dispatch_data_create(utf8, sizeof(utf8), NULL, ^{});
	dispatch_data_t utf16_data = dispatch_data_create(utf16le_invalid, sizeof(utf16le_invalid), NULL, ^{});

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf16_data, DISPATCH_DATA_FORMAT_TYPE_UTF16LE, DISPATCH_DATA_FORMAT_TYPE_UTF8);
	test_ptr_null("dispatch_data_create_with_transform (UTF16LE (missing surrogate) -> UTF8)", transformed);

	dispatch_release(utf16_data);
	dispatch_release(utf8_data);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, truncated_utf8_test);
}

void
invalid_utf16le_test(void * context)
{
	dispatch_data_t utf8_data = dispatch_data_create(utf8, sizeof(utf8), NULL, ^{});
	dispatch_data_t utf16_data = dispatch_data_create(utf16, (sizeof(utf16) % 2) + 1, NULL, ^{});

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf16_data, DISPATCH_DATA_FORMAT_TYPE_UTF16LE, DISPATCH_DATA_FORMAT_TYPE_UTF8);
	test_ptr_null("dispatch_data_create_with_transform (UTF16LE (invalid) -> UTF8)", transformed);

	dispatch_release(utf16_data);
	dispatch_release(utf8_data);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, invalid_utf16le_surrogate_test);
}

void
utf16le_bytes_to_utf8_test(void * context)
{
	dispatch_data_t utf16_data = dispatch_data_empty;
	dispatch_data_t utf8_data = dispatch_data_create(utf8, sizeof(utf8), NULL, ^{});

	size_t i;
	for (i=0; i<sizeof(utf16); i++) {
		dispatch_data_t new = dispatch_data_create((char*)utf16 + i, 1, NULL, ^{});
		dispatch_data_t concat = dispatch_data_create_concat(utf16_data, new);
		dispatch_release(new);
		dispatch_release(utf16_data);
		utf16_data = concat;
	}

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf16_data, DISPATCH_DATA_FORMAT_TYPE_UTF_ANY, DISPATCH_DATA_FORMAT_TYPE_UTF8);
	test_ptr_notnull("dispatch_data_create_with_transform (UTF16LE (any, single bytes) -> UTF8)", transformed);
	test_data_equal("utf16le_bytes_to_utf8_test", transformed, utf8_data);

	dispatch_release(transformed);
	dispatch_release(utf8_data);
	dispatch_release(utf16_data);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, invalid_utf16le_test);
}

void
utf8_bytes_to_utf16le_test(void * context)
{
	dispatch_data_t utf8_data = dispatch_data_empty;
	dispatch_data_t utf16_data = dispatch_data_create(utf16, sizeof(utf16), NULL, ^{});

	size_t i;
	for (i=0; i<sizeof(utf8); i++) {
		dispatch_data_t new = dispatch_data_create(utf8 + i, 1, NULL, ^{});
		dispatch_data_t concat = dispatch_data_create_concat(utf8_data, new);
		dispatch_release(new);
		dispatch_release(utf8_data);
		utf8_data = concat;
	}

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf8_data, DISPATCH_DATA_FORMAT_TYPE_UTF_ANY, DISPATCH_DATA_FORMAT_TYPE_UTF16LE);
	test_ptr_notnull("dispatch_data_create_with_transform (UTF8 (any, single bytes) -> UTF16LE)", transformed);
	test_data_equal("utf8_bytes_to_utf16le_test", transformed, utf16_data);

	dispatch_release(transformed);
	dispatch_release(utf8_data);
	dispatch_release(utf16_data);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, utf16le_bytes_to_utf8_test);
}

void
utf16be_detect_to_utf16le_test(void * context)
{
	dispatch_data_t utf16be_data = dispatch_data_create(utf16be, sizeof(utf16be), NULL, ^{});
	dispatch_data_t utf16_data = dispatch_data_create(utf16, sizeof(utf16), NULL, ^{});

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf16be_data, DISPATCH_DATA_FORMAT_TYPE_UTF_ANY, DISPATCH_DATA_FORMAT_TYPE_UTF16LE);
	test_ptr_notnull("dispatch_data_create_with_transform (UTF16BE (any) -> UTF16LE)", transformed);
	test_data_equal("utf16be_detect_to_utf16le_test", transformed, utf16_data);

	dispatch_release(transformed);
	dispatch_release(utf16be_data);
	dispatch_release(utf16_data);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, utf8_bytes_to_utf16le_test);
}

void
utf16be_detect_to_utf8_test(void * context)
{
	dispatch_data_t utf8_data = dispatch_data_create(utf8, sizeof(utf8), NULL, ^{});
	dispatch_data_t utf16_data = dispatch_data_create(utf16be, sizeof(utf16be), NULL, ^{});

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf16_data, DISPATCH_DATA_FORMAT_TYPE_UTF_ANY, DISPATCH_DATA_FORMAT_TYPE_UTF8);
	test_ptr_notnull("dispatch_data_create_with_transform (UTF16BE (any) -> UTF8)", transformed);
	test_data_equal("utf16be_detect_to_utf8_test", transformed, utf8_data);

	dispatch_release(transformed);
	dispatch_release(utf16_data);
	dispatch_release(utf8_data);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, utf16be_detect_to_utf16le_test);
}

void
utf16le_detect_to_utf8_test(void * context)
{
	dispatch_data_t utf8_data = dispatch_data_create(utf8, sizeof(utf8), NULL, ^{});
	dispatch_data_t utf16_data = dispatch_data_create(utf16, sizeof(utf16), NULL, ^{});

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf16_data, DISPATCH_DATA_FORMAT_TYPE_UTF_ANY, DISPATCH_DATA_FORMAT_TYPE_UTF8);
	test_ptr_notnull("dispatch_data_create_with_transform (UTF16LE (any) -> UTF8)", transformed);
	test_data_equal("utf16le_detect_to_utf8_test", transformed, utf8_data);

	dispatch_release(transformed);
	dispatch_release(utf16_data);
	dispatch_release(utf8_data);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, utf16be_detect_to_utf8_test);
}

void
utf16be_to_utf8_test(void * context)
{
	dispatch_data_t utf8_data = dispatch_data_create(utf8, sizeof(utf8), NULL, ^{});
	dispatch_data_t utf16_data = dispatch_data_create(utf16be, sizeof(utf16be), NULL, ^{});

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf16_data, DISPATCH_DATA_FORMAT_TYPE_UTF16BE, DISPATCH_DATA_FORMAT_TYPE_UTF8);
	test_ptr_notnull("dispatch_data_create_with_transform (UTF16BE -> UTF8)", transformed);
	test_data_equal("utf16be_to_utf8_test", transformed, utf8_data);

	dispatch_release(transformed);
	dispatch_release(utf16_data);
	dispatch_release(utf8_data);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, utf16le_detect_to_utf8_test);
}

void
utf16le_to_utf8_test(void * context)
{
	dispatch_data_t utf8_data = dispatch_data_create(utf8, sizeof(utf8), NULL, ^{});
	dispatch_data_t utf16_data = dispatch_data_create(utf16, sizeof(utf16), NULL, ^{});

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf16_data, DISPATCH_DATA_FORMAT_TYPE_UTF16LE, DISPATCH_DATA_FORMAT_TYPE_UTF8);
	test_ptr_notnull("dispatch_data_create_with_transform (UTF16LE -> UTF8)", transformed);
	test_data_equal("utf16le_to_utf8_test", transformed, utf8_data);

	dispatch_release(transformed);
	dispatch_release(utf16_data);
	dispatch_release(utf8_data);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, utf16be_to_utf8_test);
}

void
utf8_to_utf16be_test(void * context)
{
	dispatch_data_t utf8_data = dispatch_data_create(utf8, sizeof(utf8), NULL, ^{});
	dispatch_data_t utf16_data = dispatch_data_create(utf16be, sizeof(utf16be), NULL, ^{});

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf8_data, DISPATCH_DATA_FORMAT_TYPE_UTF8, DISPATCH_DATA_FORMAT_TYPE_UTF16BE);
	test_ptr_notnull("dispatch_data_create_with_transform (UTF8 -> UTF16BE)", transformed);
	test_data_equal("utf8_to_utf16be_test", transformed, utf16_data);

	dispatch_release(transformed);
	dispatch_release(utf16_data);
	dispatch_release(utf8_data);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, utf16le_to_utf8_test);
}

void
utf8_to_utf16le_test(void * context)
{
	dispatch_data_t utf8_data = dispatch_data_create(utf8, sizeof(utf8), NULL, ^{});
	dispatch_data_t utf16_data = dispatch_data_create(utf16, sizeof(utf16), NULL, ^{});

	dispatch_data_t transformed = dispatch_data_create_with_transform(utf8_data, DISPATCH_DATA_FORMAT_TYPE_UTF8, DISPATCH_DATA_FORMAT_TYPE_UTF16LE);
	test_ptr_notnull("dispatch_data_create_with_transform (UTF8 -> UTF16LE)", transformed);
	test_data_equal("utf8_to_utf16le_test", transformed, utf16_data);

	dispatch_release(transformed);
	dispatch_release(utf16_data);
	dispatch_release(utf8_data);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, utf8_to_utf16be_test);
}

#pragma mark - base32 tests

void
decode32_corrupt_test(void * context)
{
	dispatch_group_enter((dispatch_group_t)context);

	int fd = open("/dev/random", O_RDONLY);
	assert(fd >= 0);

	dispatch_read(fd, 4096, dispatch_get_main_queue(), ^(dispatch_data_t data, int error) {
		assert(error == 0);

		SecTransformRef transformRef = SecEncodeTransformCreate(kSecBase32Encoding, NULL);
		assert(transformRef);

		dispatch_data_t sectransform_data = execute_sectransform(transformRef, data);
		assert(sectransform_data);
		CFRelease(transformRef);

		void * corrupt_buffer = malloc(dispatch_data_get_size(sectransform_data));
		const void * source;
		size_t size;

		dispatch_data_t map = dispatch_data_create_map(sectransform_data, &source, &size);
		memcpy(corrupt_buffer, source, size);

		size_t i;
		for (i=0; i<size; i += (arc4random() % (int)(size * 0.05))) {
			char x = arc4random() & 0xff;
			while ((x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z') || (x >= '0' && x <= '9') || x == '/' || x == '+' || x == '=') {
				x = arc4random() & 0xff;
			}

			((char*)corrupt_buffer)[i] = x;
		}

		dispatch_release(map);
		dispatch_release(sectransform_data);

		dispatch_data_t corrupt_data = dispatch_data_create(corrupt_buffer, size, dispatch_get_main_queue(), DISPATCH_DATA_DESTRUCTOR_FREE);

		dispatch_data_t transform_data = dispatch_data_create_with_transform(corrupt_data, DISPATCH_DATA_FORMAT_TYPE_BASE32, DISPATCH_DATA_FORMAT_TYPE_NONE);
		test_ptr_null("decode32_corrupt_test: dispatch_data_create_with_transform", transform_data);

		dispatch_release(corrupt_data);

		close(fd);

		dispatch_group_async_f(context, dispatch_get_main_queue(), context, utf8_to_utf16le_test);
		dispatch_group_leave((dispatch_group_t)context);
	});
}

void
chunking_decode32_test(void * context)
{
	(void)context;

	int fd = open("/dev/random", O_RDONLY);
	assert(fd >= 0);

	dispatch_data_t __block data = dispatch_data_empty;

	int i;
	dispatch_group_t group = dispatch_group_create();
	dispatch_queue_t queue = dispatch_queue_create("read", 0);
	for (i=0; i<4096; i++) {
		dispatch_group_enter(group);

		dispatch_read(fd, 1, queue, ^(dispatch_data_t d, int error) {
			assert(error == 0);

			dispatch_data_t concat = dispatch_data_create_concat(data, d);
			dispatch_release(data);
			data = concat;
			dispatch_group_leave(group);
		});
	}
	dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
	dispatch_release(queue);
	dispatch_release(group);

	SecTransformRef transformRef = SecEncodeTransformCreate(kSecBase32Encoding, NULL);
	assert(transformRef);

	dispatch_data_t sectransform_data = execute_sectransform(transformRef, data);
	assert(sectransform_data);
	CFRelease(transformRef);

	dispatch_data_t transformed_data = dispatch_data_create_with_transform(sectransform_data, DISPATCH_DATA_FORMAT_TYPE_BASE32, DISPATCH_DATA_FORMAT_TYPE_NONE);
	test_ptr_notnull("chunking_decode32_test: dispatch_data_create_with_transform", transformed_data);
	test_data_equal("chunking_decode32_test", transformed_data, data);

	dispatch_release(sectransform_data);
	dispatch_release(transformed_data);
	dispatch_release(data);

	close(fd);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, decode32_corrupt_test);
}

void
chunking_encode32_test(void * context)
{
	(void)context;

	int fd = open("/dev/random", O_RDONLY);
	assert(fd >= 0);

	dispatch_data_t __block data = dispatch_data_empty;

	int i;
	dispatch_group_t group = dispatch_group_create();
	dispatch_queue_t queue = dispatch_queue_create("read", 0);
	for (i=0; i<4096; i++) {
		dispatch_group_enter(group);

		dispatch_read(fd, 1, queue, ^(dispatch_data_t d, int error) {
			assert(error == 0);

			dispatch_data_t concat = dispatch_data_create_concat(data, d);
			dispatch_release(data);
			data = concat;
			dispatch_group_leave(group);
		});
	}
	dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
	dispatch_release(queue);
	dispatch_release(group);

	SecTransformRef transformRef = SecEncodeTransformCreate(kSecBase32Encoding, NULL);
	assert(transformRef);

	dispatch_data_t sectransform_data = execute_sectransform(transformRef, data);
	assert(sectransform_data);
	CFRelease(transformRef);

	dispatch_data_t transformed_data = dispatch_data_create_with_transform(data, DISPATCH_DATA_FORMAT_TYPE_NONE, DISPATCH_DATA_FORMAT_TYPE_BASE32);
	test_ptr_notnull("chunking_encode32_test: dispatch_data_create_with_transform", transformed_data);
	test_data_equal("chunking_encode32_test", transformed_data, sectransform_data);

	dispatch_release(sectransform_data);
	dispatch_release(transformed_data);
	dispatch_release(data);

	close(fd);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, chunking_decode32_test);
}

void
decode32_test(void * context)
{
	dispatch_group_enter((dispatch_group_t)context);

	int fd = open("/dev/random", O_RDONLY);
	assert(fd >= 0);

	dispatch_read(fd, 4096, dispatch_get_main_queue(), ^(dispatch_data_t data, int error) {
		assert(error == 0);

		SecTransformRef transformRef = SecEncodeTransformCreate(kSecBase32Encoding, NULL);
		assert(transformRef);

		dispatch_data_t sectransform_data = execute_sectransform(transformRef, data);
		assert(sectransform_data);
		CFRelease(transformRef);

		dispatch_data_t transform_data = dispatch_data_create_with_transform(sectransform_data, DISPATCH_DATA_FORMAT_TYPE_BASE32, DISPATCH_DATA_FORMAT_TYPE_NONE);
		test_ptr_notnull("decode32_test: dispatch_data_create_with_transform", transform_data);
		test_data_equal("decode32_test", transform_data, data);

		dispatch_release(sectransform_data);
		dispatch_release(transform_data);

		close(fd);

		dispatch_group_async_f((dispatch_group_t)context, dispatch_get_main_queue(), context, chunking_encode32_test);
		dispatch_group_leave((dispatch_group_t)context);
	});
}

void
encode32_test(void * context)
{
	dispatch_group_enter((dispatch_group_t)context);

	int fd = open("/dev/random", O_RDONLY);
	assert(fd >= 0);

	dispatch_read(fd, 4096, dispatch_get_main_queue(), ^(dispatch_data_t data, int error) {
		assert(error == 0);

		SecTransformRef transformRef = SecEncodeTransformCreate(kSecBase32Encoding, NULL);
		assert(transformRef);

		dispatch_data_t sectransform_data = execute_sectransform(transformRef, data);
		assert(sectransform_data);
		CFRelease(transformRef);

		dispatch_data_t transformed_data = dispatch_data_create_with_transform(data, DISPATCH_DATA_FORMAT_TYPE_NONE, DISPATCH_DATA_FORMAT_TYPE_BASE32);
		test_ptr_notnull("encode32_test: dispatch_data_create_with_transform", transformed_data);
		test_data_equal("encode32_test", transformed_data, sectransform_data);

		dispatch_release(sectransform_data);
		dispatch_release(transformed_data);

		close(fd);

		dispatch_group_async_f((dispatch_group_t)context, dispatch_get_main_queue(), context, decode32_test);
		dispatch_group_leave((dispatch_group_t)context);
	});
}

#pragma mark - base64 tests

void
decode64_loop_test(void * context)
{
	if (getenv("LOOP_SKIP") == NULL)
	{
		int fd = open("/dev/random", O_RDONLY);
		assert(fd >= 0);

		dispatch_semaphore_t sema = dispatch_semaphore_create(0);
		size_t i, __block tests = 0;

		for (i=1; i<4097; i++) {
			dispatch_read(fd, i, dispatch_get_global_queue(0, 0), ^(dispatch_data_t data, int error) {
				assert(error == 0);

				SecTransformRef transformRef = SecEncodeTransformCreate(kSecBase64Encoding, NULL);
				assert(transformRef);

				dispatch_data_t sectransform_data = execute_sectransform(transformRef, data);
				assert(sectransform_data);
				CFRelease(transformRef);

				dispatch_data_t transform_data = dispatch_data_create_with_transform(sectransform_data, DISPATCH_DATA_FORMAT_TYPE_BASE64, DISPATCH_DATA_FORMAT_TYPE_NONE);
				if (dispatch_data_equal(transform_data, data)) {
					tests++;
				}

				dispatch_release(sectransform_data);
				dispatch_release(transform_data);

				dispatch_semaphore_signal(sema);
			});
			dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
		}
		dispatch_release(sema);
		close(fd);
		test_long("decode64_loop_test", tests, 4096);
	}
	dispatch_group_async_f((dispatch_group_t)context, dispatch_get_main_queue(), context, encode32_test);
}

void
decode64_corrupt_test(void * context)
{
	dispatch_group_enter((dispatch_group_t)context);

	int fd = open("/dev/random", O_RDONLY);
	assert(fd >= 0);

	dispatch_read(fd, 4096, dispatch_get_main_queue(), ^(dispatch_data_t data, int error) {
		assert(error == 0);

		SecTransformRef transformRef = SecEncodeTransformCreate(kSecBase64Encoding, NULL);
		assert(transformRef);

		dispatch_data_t sectransform_data = execute_sectransform(transformRef, data);
		assert(sectransform_data);
		CFRelease(transformRef);

		void * corrupt_buffer = malloc(dispatch_data_get_size(sectransform_data));
		const void * source;
		size_t size;

		dispatch_data_t map = dispatch_data_create_map(sectransform_data, &source, &size);
		memcpy(corrupt_buffer, source, size);

		size_t i;
		for (i=0; i<size; i += (arc4random() % (int)(size * 0.05))) {
			char x = arc4random() & 0xff;
			while ((x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z') || (x >= '0' && x <= '9') || x == '/' || x == '+' || x == '=') {
				x = arc4random() & 0xff;
			}

			((char*)corrupt_buffer)[i] = x;
		}

		dispatch_release(map);
		dispatch_release(sectransform_data);

		dispatch_data_t corrupt_data = dispatch_data_create(corrupt_buffer, size, dispatch_get_main_queue(), DISPATCH_DATA_DESTRUCTOR_FREE);

		dispatch_data_t transform_data = dispatch_data_create_with_transform(corrupt_data, DISPATCH_DATA_FORMAT_TYPE_BASE64, DISPATCH_DATA_FORMAT_TYPE_NONE);
		test_ptr_null("decode64_corrupt_test: dispatch_data_create_with_transform", transform_data);

		dispatch_release(corrupt_data);

		close(fd);

		dispatch_group_async_f((dispatch_group_t)context, dispatch_get_main_queue(), context, decode64_loop_test);
		dispatch_group_leave((dispatch_group_t)context);
	});
}

void
chunking_decode64_test(void * context)
{
	(void)context;

	int fd = open("/dev/random", O_RDONLY);
	assert(fd >= 0);

	dispatch_data_t __block data = dispatch_data_empty;

	int i;
	dispatch_group_t group = dispatch_group_create();
	dispatch_queue_t queue = dispatch_queue_create("read", 0);
	for (i=0; i<4096; i++) {
		dispatch_group_enter(group);

		dispatch_read(fd, 1, queue, ^(dispatch_data_t d, int error) {
			assert(error == 0);

			dispatch_data_t concat = dispatch_data_create_concat(data, d);
			dispatch_release(data);
			data = concat;
			dispatch_group_leave(group);
		});
	}
	dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
	dispatch_release(queue);
	dispatch_release(group);

	SecTransformRef transformRef = SecEncodeTransformCreate(kSecBase64Encoding, NULL);
	assert(transformRef);

	dispatch_data_t sectransform_data = execute_sectransform(transformRef, data);
	assert(sectransform_data);
	CFRelease(transformRef);

	dispatch_data_t transformed_data = dispatch_data_create_with_transform(sectransform_data, DISPATCH_DATA_FORMAT_TYPE_BASE64, DISPATCH_DATA_FORMAT_TYPE_NONE);
	test_ptr_notnull("chunking_decode64_test: dispatch_data_create_with_transform", transformed_data);
	test_data_equal("chunking_decode64_test", transformed_data, data);

	dispatch_release(sectransform_data);
	dispatch_release(transformed_data);
	dispatch_release(data);

	close(fd);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, decode64_corrupt_test);
}

void
chunking_encode64_test(void * context)
{
	(void)context;

	int fd = open("/dev/random", O_RDONLY);
	assert(fd >= 0);

	dispatch_data_t __block data = dispatch_data_empty;

	int i;
	dispatch_group_t group = dispatch_group_create();
	dispatch_queue_t queue = dispatch_queue_create("read", 0);
	for (i=0; i<4097; i++) {
		dispatch_group_enter(group);

		dispatch_read(fd, 1, queue, ^(dispatch_data_t d, int error) {
			assert(error == 0);

			dispatch_data_t concat = dispatch_data_create_concat(data, d);
			dispatch_release(data);
			data = concat;
			dispatch_group_leave(group);
		});
	}
	dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
	dispatch_release(queue);
	dispatch_release(group);

	SecTransformRef transformRef = SecEncodeTransformCreate(kSecBase64Encoding, NULL);
	assert(transformRef);

	dispatch_data_t sectransform_data = execute_sectransform(transformRef, data);
	assert(sectransform_data);
	CFRelease(transformRef);

	dispatch_data_t transformed_data = dispatch_data_create_with_transform(data, DISPATCH_DATA_FORMAT_TYPE_NONE, DISPATCH_DATA_FORMAT_TYPE_BASE64);
	test_ptr_notnull("chunking_encode64_test: dispatch_data_create_with_transform", transformed_data);
	test_data_equal("chunking_encode64_test", transformed_data, sectransform_data);

	dispatch_release(sectransform_data);
	dispatch_release(transformed_data);
	dispatch_release(data);

	close(fd);

	dispatch_group_async_f(context, dispatch_get_main_queue(), context, chunking_decode64_test);
}

void
decode64_test(void * context)
{
	dispatch_group_enter((dispatch_group_t)context);

	int fd = open("/dev/random", O_RDONLY);
	assert(fd >= 0);

	dispatch_read(fd, 4096, dispatch_get_main_queue(), ^(dispatch_data_t data, int error) {
		assert(error == 0);

		SecTransformRef transformRef = SecEncodeTransformCreate(kSecBase64Encoding, NULL);
		assert(transformRef);

		dispatch_data_t sectransform_data = execute_sectransform(transformRef, data);
		assert(sectransform_data);
		CFRelease(transformRef);

		dispatch_data_t transform_data = dispatch_data_create_with_transform(sectransform_data, DISPATCH_DATA_FORMAT_TYPE_BASE64, DISPATCH_DATA_FORMAT_TYPE_NONE);
		test_ptr_notnull("decode64_test: dispatch_data_create_with_transform", transform_data);
		test_data_equal("decode64_test", transform_data, data);

		dispatch_release(sectransform_data);
		dispatch_release(transform_data);

		close(fd);

		dispatch_group_async_f((dispatch_group_t)context, dispatch_get_main_queue(), context, chunking_encode64_test);
		dispatch_group_leave((dispatch_group_t)context);
	});
}

void
encode64_test(void * context)
{
	dispatch_group_enter((dispatch_group_t)context);

	int fd = open("/dev/random", O_RDONLY);
	assert(fd >= 0);

	dispatch_read(fd, 4096, dispatch_get_main_queue(), ^(dispatch_data_t data, int error) {
		assert(error == 0);

		SecTransformRef transformRef = SecEncodeTransformCreate(kSecBase64Encoding, NULL);
		assert(transformRef);

		dispatch_data_t sectransform_data = execute_sectransform(transformRef, data);
		assert(sectransform_data);
		CFRelease(transformRef);

		dispatch_data_t transformed_data = dispatch_data_create_with_transform(data, DISPATCH_DATA_FORMAT_TYPE_NONE, DISPATCH_DATA_FORMAT_TYPE_BASE64);
		test_ptr_notnull("encode64_test: dispatch_data_create_with_transform", transformed_data);
		test_data_equal("encode64_test", transformed_data, sectransform_data);

		dispatch_release(sectransform_data);
		dispatch_release(transformed_data);

		close(fd);

		dispatch_group_async_f((dispatch_group_t)context, dispatch_get_main_queue(), context, decode64_test);
		dispatch_group_leave((dispatch_group_t)context);
	});
}

#pragma mark - main

int
main(void)
{
	test_start("Dispatch data transforms test");

	dispatch_group_t group = dispatch_group_create();
	dispatch_group_async_f(group, dispatch_get_main_queue(), group, encode64_test);

	dispatch_group_notify(group, dispatch_get_main_queue(), ^{
		dispatch_release(group);
		test_stop();
		exit(0);
	});

	dispatch_main();
	return 0;
}

#else

int
main(void)
{
  test_skip("Dispatch data transforms test");
  return 0;
}

#endif

