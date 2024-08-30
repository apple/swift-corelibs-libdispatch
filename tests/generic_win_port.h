#pragma once

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>
#include <Windows.h>

typedef int kern_return_t;
#ifndef HAVE_PID_T
typedef int pid_t;
#endif

#if defined(_WIN64)
typedef long long ssize_t;
#else
typedef long ssize_t;
#endif

struct mach_timebase_info {
	uint32_t numer;
	uint32_t denom;
};

typedef struct mach_timebase_info *mach_timebase_info_t;
typedef struct mach_timebase_info mach_timebase_info_data_t;

WCHAR *
argv_to_command_line(char **argv);

int
asprintf(char **strp, const char *format, ...);

void
filetime_to_timeval(struct timeval *tp, const FILETIME *ft);

pid_t
getpid(void);

int
gettimeofday(struct timeval *tp, void *tzp);

uint64_t
mach_absolute_time(void);

static inline
int
mach_timebase_info(mach_timebase_info_t tbi)
{
	tbi->numer = 1;
	tbi->denom = 1;
	return 0;
}

#ifndef HAVE_MKSTEMP
dispatch_fd_t
mkstemp(char *tmpl);
#endif

void
print_winapi_error(const char *function_name, DWORD error);

intptr_t
random(void);

unsigned int
sleep(unsigned int seconds);

int
usleep(unsigned int usec);
