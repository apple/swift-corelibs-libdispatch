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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/errno.h>
#include <string.h>
#include <crt_externs.h>
#include <mach/mach_error.h>
#include <spawn.h>
#include <inttypes.h>
#include "bsdtests.h"

static int _test_exit_code;

#define _test_print(_file, _line, _desc, \
	_expr, _fmt1, _val1, _fmt2, _val2) do { \
	const char* _exprstr; \
	char _linestr[BUFSIZ]; \
	_linestr[0] = 0; \
	if (!(_expr)) { \
		_exprstr = "FAIL"; \
		_test_exit_code = 0xff; \
		if (_file && _file[0] != '\0') { \
			snprintf(_linestr, sizeof(_linestr), \
					 " (%s:%ld)", _file, _line); \
		} \
	} else { \
		_exprstr = "PASS"; \
	} \
	if (_fmt2 == 0) { \
		fprintf(stdout, "\n"			\
			"[BEGIN] %s\n"			\
			"\tValue: " _fmt1 "\n"		\
			"[%s] %s%s\n",			\
			_desc,				\
			_val1,				\
			_exprstr,			\
			_desc,				\
			_linestr);			\
	} else { \
		fprintf(stdout, "\n"		   \
			"[BEGIN] %s\n"		   \
			"\tActual: " _fmt1 "\n"	   \
			"\tExpected: " _fmt2 "\n"  \
			"[%s] %s%s\n",		   \
			_desc,			   \
			_val1,			   \
			_val2,			   \
			_exprstr,		   \
			_desc,			   \
			_linestr);		   \
	} \
	if (!_expr && _file && _file[0] != '\0') { \
		fprintf(stdout, "\t%s:%ld\n", _file, _line); \
	} \
	fflush(stdout); \
} while (0);

#define GENERATE_DESC	\
	char desc[BUFSIZ];	\
	va_list args;	\
	\
	va_start(args, format);	\
	vsnprintf(desc, sizeof(desc), format, args);	\
	va_end(args);

void
_test_ptr_null(const char* file, long line, const char* desc, const void* ptr)
{
	_test_print(file, line, desc,
		(ptr == NULL), "%p", ptr, "%p", (void*)0);
}

void
test_ptr_null_format(void *ptr, const char *format, ...)
{
	GENERATE_DESC
	_test_ptr_null(NULL, 0, desc, ptr);
}

void
_test_ptr_notnull(const char* file, long line, const char* desc, const void* ptr)
{
	_test_print(file, line, desc,
		(ptr != NULL), "%p", ptr, "%p", ptr ?: (void*)~0);
}

void
test_ptr_notnull_format(const void *ptr, const char *format, ...)
{
	GENERATE_DESC
	_test_ptr_notnull(NULL, 0, desc, ptr);
}

void
_test_ptr(const char* file, long line, const char* desc, const void* actual, const void* expected)
{
	_test_print(file, line, desc,
		(actual == expected), "%p", actual, "%p", expected);
}

void
test_ptr_format(const void* actual, const void* expected, const char* format, ...)
{
	GENERATE_DESC
	_test_ptr(NULL, 0, desc, actual, expected);
}

void
_test_uint32(const char* file, long line, const char* desc, uint32_t actual, uint32_t expected)
{
	_test_print(file, line, desc,
		(actual == expected), "%u", actual, "%u", expected);
}

void
test_uint32_format(long actual, long expected, const char *format, ...)
{
	GENERATE_DESC
	_test_uint32(NULL, 0, desc, actual, expected);
}

void
_test_int32(const char* file, long line, const char* desc, int32_t actual, int32_t expected)
{
	_test_print(file, line, desc,
		(actual == expected), "%d", actual, "%d", expected);
}

void
test_int32_format(int32_t actual, int32_t expected, const char* format, ...)
{
	GENERATE_DESC
	_test_int32(NULL, 0, desc, actual, expected);
}

void
_test_long(const char* file, long line, const char* desc, long actual, long expected)
{
	_test_print(file, line, desc,
		(actual == expected), "%ld", actual, "%ld", expected);
}

void
test_long_format(long actual, long expected, const char* format, ...)
{
	GENERATE_DESC
	_test_long(NULL, 0, desc, actual, expected);
}

void
_test_uint64(const char* file, long line, const char* desc, uint64_t actual, uint64_t expected)
{
	_test_print(file, line, desc,
							(actual == expected), "%" PRIu64, actual, "%" PRIu64, expected);
}

void
test_uint64_format(uint64_t actual, uint64_t expected, const char* format, ...)
{
	GENERATE_DESC
	_test_uint64(NULL, 0, desc, actual, expected);
}

void
_test_int64(const char* file, long line, const char* desc, int64_t actual, int64_t expected)
{
	_test_print(file, line, desc,
							(actual == expected), "%" PRId64, actual, "%" PRId64, expected);
}

void
test_int64_format(int64_t actual, int64_t expected, const char* format, ...)
{
	GENERATE_DESC
	_test_int64(NULL, 0, desc, actual, expected);
}

void
_test_long_less_than(const char* file, long line, const char* desc, long actual, long expected_max)
{
	_test_print(file, line, desc, (actual < expected_max), "%ld", actual, "<%ld", expected_max);
}

void
test_long_less_than_format(long actual, long expected_max, const char* format, ...)
{
	GENERATE_DESC
	_test_long_less_than(NULL, 0, desc, actual, expected_max);
}

void
_test_long_greater_than_or_equal(const char* file, long line, const char* desc, long actual, long expected_min)
{
	_test_print(file, line, desc, (actual >= expected_min), "%ld", actual, ">=%ld", expected_min);
}

void
test_long_greater_than_or_equal_format(long actual, long expected_max, const char* format, ...)
{
	GENERATE_DESC
	_test_long_greater_than_or_equal(NULL, 0, desc, actual, expected_max);
}

void
_test_double_less_than(const char* file, long line, const char* desc, double val, double max_expected)
{
	_test_print(file, line, desc, (val < max_expected), "%f", val, "<%f", max_expected);
}

void
test_double_less_than_format(double val, double max_expected, const char* format, ...)
{
	GENERATE_DESC
	_test_double_less_than(NULL, 0, desc, val, max_expected);
}

void
_test_double_less_than_or_equal(const char* file, long line, const char* desc, double val, double max_expected)
{
	_test_print(file, line, desc, (val <= max_expected), "%f", val, "<=%f", max_expected);
}

void
test_double_less_than_or_equal_format(double val, double max_expected, const char *format, ...)
{
	GENERATE_DESC
	_test_double_less_than_or_equal(NULL, 0, desc, val, max_expected);
}

void
_test_errno(const char* file, long line, const char* desc, long actual, long expected)
{
	char* actual_str;
	char* expected_str;
	asprintf(&actual_str, "%ld\t%s", actual, actual ? strerror(actual) : "");
	asprintf(&expected_str, "%ld\t%s", expected, expected ? strerror(expected) : "");
	_test_print(file, line, desc,
		(actual == expected), "%s", actual_str, "%s", expected_str);
	free(actual_str);
	free(expected_str);
}

void
test_errno_format(long actual, long expected, const char *format, ...)
{
	GENERATE_DESC
	_test_errno(NULL, 0, desc, actual, expected);
}

void
_test_mach_error(const char* file, long line, const char* desc,
		mach_error_t actual, mach_error_t expected)
{
	char* actual_str;
	char* expected_str;
	asprintf(&actual_str, "%d %s", actual, actual ? mach_error_string(actual) : "");
	asprintf(&expected_str, "%d %s", expected, expected ? mach_error_string(expected) : "");
	_test_print(file, line, desc,
		(actual == expected), "%s", actual_str, "%s", expected_str);
	free(actual_str);
	free(expected_str);
}

void
test_mach_error_format(mach_error_t actual, mach_error_t expected, const char *format, ...)
{
	GENERATE_DESC
	_test_mach_error(NULL, 0, desc, actual, expected);
}

void
_test_skip(const char* file, long line, const char* desc)
{
	if (file != NULL && file[0] != '\0') {
		fprintf(stdout, "[SKIP] %s (%s:%ld)\n", desc, file, line);
	} else {
		fprintf(stdout, "[SKIP] %s\n", desc);
	}
	fflush(stdout);
}

void
test_skip_format(const char *format, ...)
{
	GENERATE_DESC
	fprintf(stdout, "[SKIP] %s\n", desc);
}

#if USE_COREFOUNDATION
void
test_cferror(const char *desc, CFErrorRef actual, CFIndex expectedCode)
{
	if (actual != NULL) {
		CFStringRef errDesc = CFErrorCopyDescription(actual);
		CFIndex code = CFErrorGetCode(actual);
		char* actual_str;
		char* expected_str;

		if (code != expectedCode) {
			char buffer[BUFSIZ];
			CFStringGetCString(errDesc, buffer, sizeof(buffer), kCFStringEncodingUTF8);
			asprintf(&actual_str, "%ld\t%s", code, buffer);
		} else {
			asprintf(&actual_str, "%ld", code);
		}

		asprintf(&expected_str, "%ld", expectedCode);
		_test_print("", (long) 0, desc,
					(code == expectedCode), "%s", actual_str, "%s", expected_str);

		free(actual_str);
		free(expected_str);

		CFRelease(errDesc);
	} else {
		_test_print("", (long) 0, desc, (0 == expectedCode), "%ld", (long) 0, "%ld", (long) expectedCode);
	}
}

void
test_cferror_format(CFErrorRef actual, CFIndex expectedCode, const char *format, ...)
{
	GENERATE_DESC
	test_cferror(desc, actual, expectedCode);
}
#endif

void
test_start(const char* desc)
{
	if (desc) {
		fprintf(stdout, "\n==================================================\n");
		fprintf(stdout, "[TEST] %s\n", desc);
		fprintf(stdout, "[PID] %d\n", getpid());
		fprintf(stdout, "==================================================\n\n");
		fflush(stdout);
	}
	_test_exit_code = EXIT_SUCCESS;
	usleep(100000);	// give 'gdb --waitfor=' a chance to find this proc
}

void
test_leaks_pid(const char *name, pid_t pid)
{
	int res;
	char pidstr[10];

	if (getenv("NOLEAKS")) {
		return;
	}

	if (!name) {
		name = "Leaks";
	}

	/* leaks doesn't work against debug variant malloc */
	const char *dyld_image_suffix = getenv("DYLD_IMAGE_SUFFIX");
	if (dyld_image_suffix && strstr(dyld_image_suffix, "_debug")) {
		return;
	}

	char *inserted_libs = getenv("DYLD_INSERT_LIBRARIES");
	if (inserted_libs && strstr(inserted_libs, "/usr/lib/libgmalloc.dylib")) {
		return;
	}

	unsetenv("MallocStackLogging");
	unsetenv("MallocStackLoggingNoCompact");

	snprintf(pidstr, sizeof(pidstr), "%d", pid);

	char* args[] = { "./leaks-wrapper", pidstr, NULL };
	res = posix_spawnp(&pid, args[0], NULL, NULL, args, * _NSGetEnviron());
	if (res == 0 && pid > 0) {
		int status;
		waitpid(pid, &status, 0);
		test_long(name, status, 0);
	} else {
		perror(args[0]);
	}
}

void
test_leaks(const char *name)
{
	test_leaks_pid(name, getpid());
}

void
test_stop_after_delay(void *delay)
{
	if (delay != NULL) {
		sleep((int)(intptr_t)delay);
	}

	test_leaks(NULL);

	fflush(stdout);
	_exit(_test_exit_code);
}

void
test_stop(void)
{
	test_stop_after_delay((void *)(intptr_t)0);
}
