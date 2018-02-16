
#ifndef __DISPATCH__STUBS__INTERNAL
#define __DISPATCH__STUBS__INTERNAL

#include <stdint.h>

#include <Windows.h>

#include <io.h>
#include <process.h>

/*
 * Stub out defines for some mach types and related macros
 */

typedef uint32_t mach_port_t;

#define MACH_PORT_NULL (0)

typedef uint32_t mach_msg_bits_t;
typedef void *mach_msg_header_t;

/*
 * Stub out defines for other missing types
 */

// SIZE_T_MAX should not be hardcoded like this here.
#ifndef SIZE_T_MAX
#define SIZE_T_MAX (~(size_t)0)
#endif

typedef __typeof__(_Generic((__SIZE_TYPE__)0,                                  \
			    unsigned long long int : (long long int)0,         \
			    unsigned long int : (long int)0,                   \
			    unsigned int : (int)0,                             \
			    unsigned short : (short)0,                         \
			    unsigned char : (signed char)0)) ssize_t;

#define S_ISDIR(mode)  (((mode) & S_IFMT) == S_IFDIR)
#define S_ISFIFO(mode) ((mode) & _S_IFIFO)
#define S_ISREG(mode)  ((mode) & _S_IFREG)
#define S_ISSOCK(mode) 0

#define O_NONBLOCK 04000

#define bzero(ptr,len) memset((ptr), 0, (len))
#define snprintf _snprintf

#endif

