#include <generic_win_port.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wchar.h>
#include <Windows.h>

static bool
expand_wstr(WCHAR **str, size_t *capacity, size_t needed)
{
	if (*capacity >= needed) {
		return true;
	}
	if (needed > UNICODE_STRING_MAX_CHARS) {
		return false;
	}
	size_t new_capacity = *capacity ?: needed;
	while (new_capacity < needed) {
		new_capacity *= 2;
	}
	WCHAR *new_str = realloc(*str, new_capacity * sizeof(WCHAR));
	if (!new_str) {
		return false;
	}
	*str = new_str;
	*capacity = new_capacity;
	return true;
}

static bool
append_wstr(WCHAR **str, size_t *capacity, size_t *len, WCHAR *suffix)
{
	size_t suffix_len = wcslen(suffix);
	if (!expand_wstr(str, capacity, *len + suffix_len)) {
		return false;
	}
	memcpy(*str + *len, suffix, suffix_len * sizeof(WCHAR));
	*len += suffix_len;
	return true;
}

WCHAR *
argv_to_command_line(char **argv)
{
	// This is basically the reverse of CommandLineToArgvW(). We want to convert
	// an argv array into a command-line compatible with CreateProcessW().
	//
	// See also:
	// <https://docs.microsoft.com/en-us/windows/desktop/api/shellapi/nf-shellapi-commandlinetoargvw>
	// <https://blogs.msdn.microsoft.com/twistylittlepassagesallalike/2011/04/23/everyone-quotes-command-line-arguments-the-wrong-way/>
	size_t len = 0, capacity = 0;
	WCHAR *cmdline = NULL;
	if (!expand_wstr(&cmdline, &capacity, 256)) {
		goto error;
	}
	for (size_t i = 0; argv[i]; i++) {
		// Separate arguments with spaces.
		if (i > 0 && !append_wstr(&cmdline, &capacity, &len, L" ")) {
			goto error;
		}
		// Surround the argument with quotes if it's empty or contains special
		// characters.
		char *cur = argv[i];
		bool quoted = (*cur == '\0' || cur[strcspn(cur, " \t\n\v\"")] != '\0');
		if (quoted && !append_wstr(&cmdline, &capacity, &len, L"\"")) {
			goto error;
		}
		while (*cur != '\0') {
			if (*cur == '"') {
				// Quotes must be escaped with a backslash.
				if (!append_wstr(&cmdline, &capacity, &len, L"\\\"")) {
					goto error;
				}
				cur++;
			} else if (*cur == '\\') {
				// Windows treats backslashes differently depending on whether
				// they're followed by a quote. If the backslashes aren't
				// followed by a quote, then all slashes are copied into the
				// argument string. Otherwise, only n/2 slashes are included.
				// Count the number of slashes and double them if they're
				// followed by a quote.
				size_t backslashes = strspn(cur, "\\");
				cur += backslashes;
				// If the argument needs to be surrounded with quotes, we must
				// also check if the backslashes are at the end of the argument
				// because the added quote will follow them.
				if (*cur == '"' || (quoted && *cur == '\0')) {
					backslashes *= 2;
				}
				if (!expand_wstr(&cmdline, &capacity, len + backslashes)) {
					goto error;
				}
				wmemset(&cmdline[len], L'\\', backslashes);
				len += backslashes;
			} else {
				// Widen as many characters as possible.
				size_t mb_len = strcspn(cur, "\"\\");
				int wide_len = MultiByteToWideChar(CP_UTF8, 0, cur, mb_len,
						NULL, 0);
				if (wide_len == 0) {
					goto error;
				}
				if (!expand_wstr(&cmdline, &capacity, len + wide_len)) {
					goto error;
				}
				wide_len = MultiByteToWideChar(CP_UTF8, 0, cur, mb_len,
						&cmdline[len], wide_len);
				if (wide_len == 0) {
					goto error;
				}
				cur += mb_len;
				len += wide_len;
			}
		}
		if (quoted && !append_wstr(&cmdline, &capacity, &len, L"\"")) {
			goto error;
		}
	}
	if (!expand_wstr(&cmdline, &capacity, len + 1)) {
		goto error;
	}
	cmdline[len] = L'\0';
	return cmdline;
error:
	free(cmdline);
	return NULL;
}

int
asprintf(char **strp, const char *format, ...)
{
	va_list arg1;
	va_start(arg1, format);
	int len = vsnprintf(NULL, 0, format, arg1);
	va_end(arg1);
	if (len >= 0) {
		size_t size = (size_t)len + 1;
		*strp = malloc(size);
		if (!*strp) {
			return -1;
		}
		va_list arg2;
		va_start(arg2, format);
		len = vsnprintf(*strp, size, format, arg2);
		va_end(arg2);
	}
	return len;
}

void
filetime_to_timeval(struct timeval *tp, const FILETIME *ft)
{
	int64_t ticks = ft->dwLowDateTime | (((int64_t)ft->dwHighDateTime) << 32);
	static const int64_t ticks_per_sec = 10LL * 1000LL * 1000LL;
	static const int64_t ticks_per_usec = 10LL;
	if (ticks >= 0) {
		tp->tv_sec = (long)(ticks / ticks_per_sec);
		tp->tv_usec = (long)((ticks % ticks_per_sec) / ticks_per_usec);
	} else {
		tp->tv_sec = (long)((ticks + 1) / ticks_per_sec - 1);
		tp->tv_usec = (long)((ticks_per_sec - 1 + (ticks + 1) % ticks_per_sec) / ticks_per_usec);
	}
}

pid_t
getpid(void)
{
	return (pid_t)GetCurrentProcessId();
}

int
gettimeofday(struct timeval *tp, void *tzp)
{
	(void)tzp;
	FILETIME ft;
	GetSystemTimePreciseAsFileTime(&ft);
	int64_t ticks = ft.dwLowDateTime | (((int64_t)ft.dwHighDateTime) << 32);
	ticks -= 116444736000000000LL;  // Convert to Unix time
	FILETIME unix_ft = {.dwLowDateTime = (DWORD)ticks, .dwHighDateTime = ticks >> 32};
	filetime_to_timeval(tp, &unix_ft);
	return 0;
}

void
print_winapi_error(const char *function_name, DWORD error)
{
	char *message = NULL;
	DWORD len = FormatMessageA(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
			NULL,
			error,
			0,
			(LPSTR)&message,
			0,
			NULL);
	if (len > 0) {
		// Note: FormatMessage includes a newline at the end of the message
		fprintf(stderr, "%s: %s", function_name, message);
		LocalFree(message);
	} else {
		fprintf(stderr, "%s: error %lu\n", function_name, error);
	}
}

unsigned int
sleep(unsigned int seconds)
{
	Sleep(seconds * 1000);
	return 0;
}

int
usleep(unsigned int usec)
{
	DWORD ms = usec / 1000;
	if (ms == 0 && usec != 0) {
		ms = 1;
	}
	Sleep(ms);
	return 0;
}
