
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

#define EVFILT_MACHPORT -8

typedef uint32_t mach_error_t;

typedef uint32_t mach_vm_size_t;

typedef uint32_t mach_msg_return_t;

typedef uintptr_t mach_vm_address_t;

typedef uint32_t dispatch_mach_msg_t;

typedef uint32_t dispatch_mach_t;

typedef uint32_t dispatch_mach_reason_t;


typedef struct
{
} mach_msg_header_t;


typedef void (*dispatch_mach_handler_function_t)(void*, dispatch_mach_reason_t,
						 dispatch_mach_msg_t, mach_error_t);

typedef void (*dispatch_mach_msg_destructor_t)(void*);

/*
 * Stub out defines for other missing types
 */

struct kevent64_s
{
    uint64_t        ident;
    int16_t         filter;
    uint16_t        flags;
    uint32_t        fflags;
    int64_t         data;
    uint64_t        udata;
    uint64_t        ext[2];
};

typedef uint32_t voucher_activity_mode_t;

struct voucher_offsets_s {
  uint32_t vo_version;
};


// bogus...
#define PAGE_SIZE 4096
#define SIZE_T_MAX 0x7fffffff

#define NOTE_VM_PRESSURE 0
#define NOTE_ABSOLUTE 0
#define NOTE_NSECONDS 0
#define NOTE_LEEWAY 0
#define NOTE_CRITICAL 0
#define NOTE_BACKGROUND 0

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


#endif /* __OS_LINUX_BASE__ */
