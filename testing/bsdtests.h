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

#ifndef __BSD_TEST_H__
#define __BSD_TEST_H__

#include <errno.h>
#include <mach/error.h>
#include <CoreFoundation/CoreFoundation.h>

static inline const char*
__BASENAME__(const char *_str_)
{
	const char *_s_ = strrchr(_str_, '/');
	return (_s_ ? _s_ : _str_ - 1) + 1;
}
#define __SOURCE_FILE__	__BASENAME__(__FILE__)

__BEGIN_DECLS

/**
 * test_start() provides the TEST token. Use this once per test "tool"
 */
void test_start(const char* desc);

/**
 * Explicitly runs the 'leaks' test without stopping the process.
 */
void test_leaks_pid(const char *name, pid_t pid);
void test_leaks(const char *name);

/**
 * test_stop() checks for leaks during the tests using leaks-wrapper. Use this at the end of each "tool"
 */
void test_stop(void) __attribute__((__noreturn__));
void test_stop_after_delay(void *delay) __attribute__((__noreturn__));

/**
 * Each test "tool" can used one or more of these functions to perform actual
 * testing and provide a PASS/FAIL token. All APIs take a descriptive string
 * that is printed after the token.
 */
void _test_ptr_null(const char* file, long line, const char* desc, const void* ptr);
#define test_ptr_null(a,b) _test_ptr_null(__SOURCE_FILE__, __LINE__, a, b)
void test_ptr_null_format(void *ptr, const char *format, ...);

void _test_ptr_notnull(const char* file, long line, const char* desc, const void* ptr);
#define test_ptr_notnull(a,b) _test_ptr_notnull(__SOURCE_FILE__, __LINE__, a, b)
void test_ptr_notnull_format(const void *ptr, const char *format, ...) __printflike(2, 3);

void _test_ptr_not(const char* file, long line, const char* desc, const void* actual, const void* expected);
#define test_ptr_not(a, b, c) _test_ptr_not(__SOURCE_FILE__, __LINE__, a, b, c)
void test_ptr_not_format(const void* actual, const void* expected, const char *format, ...);

void _test_ptr(const char* file, long line, const char* desc, const void* actual, const void* expected);
#define test_ptr(a,b,c) _test_ptr(__SOURCE_FILE__, __LINE__, a, b, c)
void test_ptr_format(const void* actual, const void* expected, const char *format, ...) __printflike(3,4);

void _test_uint32(const char* file, long line, const char* desc, uint32_t actual, uint32_t expected);
#define test_uint32(a,b,c) _test_uint32(__SOURCE_FILE__, __LINE__, a, b, c)
void test_uint32_format(long actual, long expected, const char *format, ...) __printflike(3,4);

void _test_int32(const char* file, long line, const char* desc, int32_t actual, int32_t expected);
#define test_int32(a,b,c) _test_int32(__SOURCE_FILE__, __LINE__, a, b, c)
void test_sint32_format(int32_t actual, int32_t expected, const char* format, ...) __printflike(3,4);

void _test_long(const char* file, long line, const char* desc, long actual, long expected);
#define test_long(a,b,c) _test_long(__SOURCE_FILE__, __LINE__, a, b, c)
void test_long_format(long actual, long expected, const char *format, ...) __printflike(3,4);

void _test_uint64(const char* file, long line, const char* desc, uint64_t actual, uint64_t expected);
#define test_uint64(a,b,c) _test_uint64(__SOURCE_FILE__, __LINE__, a, b, c)
void test_uint64_format(uint64_t actual, uint64_t expected, const char* desc, ...);

void _test_int64(const char* file, long line, const char* desc, int64_t actual, int64_t expected);
#define test_int64(a,b,c) _test_uint64(__SOURCE_FILE__, __LINE__, a, b, c)
void test_int64_format(int64_t actual, int64_t expected, const char* desc, ...);

void _test_long_less_than(const char* file, long line, const char* desc, long actual, long max_expected);
#define test_long_less_than(a,b,c) _test_long_less_than(__SOURCE_FILE__, __LINE__, a, b, c)
void test_long_less_than_format(long actual, long max_expected, const char *format, ...) __printflike(3,4);

void _test_long_less_than_or_equal(const char* file, long line, const char* desc, long actual, long max_expected);
#define test_long_less_than_or_equal(a,b,c) _test_long_less_than_or_equal(__SOURCE_FILE__, __LINE__, a, b, c)
void test_long_less_than_or_equal_format(long actual, long max_expected, const char *format, ...) __printflike(3,4);

void _test_long_greater_than_or_equal(const char* file, long line, const char* desc, long actual, long expected_min);
#define test_long_greater_than_or_equal(a,b,c) _test_long_greater_than_or_equal(__SOURCE_FILE__, __LINE__, a, b, c)
void test_long_greater_than_or_equal_format(long actual, long expected_min, const char *format, ...) __printflike(3,4);

void _test_double_less_than_or_equal(const char* file, long line, const char* desc, double val, double max_expected);
#define test_double_less_than_or_equal(d, v, m) _test_double_less_than_or_equal(__SOURCE_FILE__, __LINE__, d, v, m)
void test_double_less_than_or_equal_format(double val, double max_expected, const char *format, ...) __printflike(3,4);

void _test_double_less_than(const char* file, long line, const char* desc, double val, double max_expected);
#define test_double_less_than(d, v, m) _test_double_less_than(__SOURCE_FILE__, __LINE__, d, v, m)
void test_double_less_than_format(double val, double max_expected, const char *format, ...) __printflike(3,4);

void _test_double_equal(const char* file, long line, const char* desc, double val, double expected);
#define test_double_equal(d, v, m) _test_double_equal(__SOURCE_FILE__, __LINE__, d, v, m)
void test_double_equal_format(double val, double expected, const char *format, ...) __printflike(3,4);

void _test_errno(const char* file, long line, const char* desc, long actual, long expected);
#define test_errno(a,b,c) _test_errno(__SOURCE_FILE__, __LINE__, a, b, c)
void test_errno_format(long actual, long expected, const char *format, ...) __printflike(3,4);

void _test_mach_error(const char* file, long line, const char* desc, mach_error_t actual, mach_error_t expected);
#define test_mach_error(a,b,c) _test_mach_error(__SOURCE_FILE__, __LINE__, a, b, c)
void test_mach_error_format(mach_error_t actual, mach_error_t expected, const char *format, ...) __printflike(3,4);

void test_cferror(const char* desc, CFErrorRef actual, CFIndex expectedCode);
void test_cferror_format(CFErrorRef actual, CFIndex expectedCode, const char *format, ...) __printflike(3,4);

void _test_skip(const char* file, long line, const char* desc);
#define test_skip(m) _test_skip(__SOURCE_FILE__, __LINE__, m)
#define test_skip2(m) _test_skip("", 0, m)
void test_skip_format(const char *format, ...) __printflike(1,2);

__END_DECLS

#endif /* __BSD_TEST_H__ */
