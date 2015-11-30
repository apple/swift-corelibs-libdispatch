
#ifndef __OS_LINUX_BASE__
#define __OS_LINUX_BASE__


// marker for hacks we have made to make progress
#define __LINUX_PORT_HDD__ 1 

/*
 * Stub out defines for some mach types and related macros
 */

typedef uint32_t mach_port_t;

#define  MACH_PORT_NULL 0
#define  MACH_PORT_DEAD -1


typedef uint32_t mach_error_t;

typedef uint32_t dispatch_mach_msg_t;

typedef uint32_t dispatch_mach_t;

typedef uint32_t dispatch_mach_reason_t;

typedef uint32_t mach_vm_size_t;

typedef uintptr_t mach_vm_address_t;

typedef struct
{
} mach_msg_header_t;


typedef void (*dispatch_mach_handler_function_t)(void*);
typedef void (*dispatch_mach_msg_destructor_t)(void*);

/*
 * Stub out defines for other missing types
 */

struct kevent64_s
{
};

/*
 * Stub out misc linking and compilation attributes
 */

#ifdef OS_EXPORT
#undef OS_EXPORT
#endif
#define OS_EXPORT

#ifdef DISPATCH_EXPORT
#undef DISPATCH_EXPORT
#endif 
#define DISPATCH_EXPORT

#ifdef DISPATCH_NONNULL_ALL 
#undef DISPATCH_NONNULL_ALL 
#endif
#define DISPATCH_NONNULL_ALL 

#ifdef OS_WARN_RESULT_NEEDS_RELEASE 
#undef OS_WARN_RESULT_NEEDS_RELEASE  
#endif

#ifdef OS_WARN_RESULT
#undef OS_WARN_RESULT
#endif
#define OS_WARN_RESULT

#ifdef OS_NOTHROW
#undef OS_NOTHROW
#endif
#define OS_NOTHROW


// NOTE (Dave), these and similar macros come from Availabity.h on OS X
#define __OSX_AVAILABLE_BUT_DEPRECATED(a,b,c,d) //
#define __OSX_AVAILABLE_BUT_DEPRECATED_MSG(a,b,c,d,msg) // 


// Print a warning when an unported code path executes.
#define LINUX_PORT_ERROR()  do { printf("LINUX_PORT_ERROR_CALLED %s:%d: %s\n",__FILE__,__LINE__,__FUNCTION__); } while (0)


// Functions we are stubbing out
#include<os/stubs.h>


#endif /* __OS_LINUX_BASE__ */
