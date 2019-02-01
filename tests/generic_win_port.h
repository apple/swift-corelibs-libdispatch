#pragma once

#include <stdint.h>
#include <Windows.h>

typedef int kern_return_t;
typedef int pid_t;

#if defined(_WIN64)
typedef long long ssize_t;
#else
typedef long ssize_t;
#endif

static inline int32_t
OSAtomicIncrement32(volatile int32_t *var)
{
	return __c11_atomic_fetch_add((_Atomic(int)*)var, 1, __ATOMIC_RELAXED)+1;
}

static inline int32_t
OSAtomicIncrement32Barrier(volatile int32_t *var)
{
	return __c11_atomic_fetch_add((_Atomic(int)*)var, 1, __ATOMIC_SEQ_CST)+1;
}

static inline int32_t
OSAtomicAdd32(int32_t val, volatile int32_t *var)
{
	return __c11_atomic_fetch_add((_Atomic(int)*)var, val, __ATOMIC_RELAXED)+val;
}

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

void
print_winapi_error(const char *function_name, DWORD error);

unsigned int
sleep(unsigned int seconds);

int
usleep(unsigned int usec);
