/*
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

#ifndef __OS_LINUX_BASE__
#define __OS_LINUX_BASE__

// #include <sys/event.h>

// marker for hacks we have made to make progress
#define __LINUX_PORT_HDD__ 1

/*
 * Stub out defines for some mach types and related macros
 */

typedef uint32_t mach_port_t;

#define  MACH_PORT_NULL (0)
#define  MACH_PORT_DEAD (-1)

#define EVFILT_MACHPORT (-8)

typedef uint32_t mach_error_t;

typedef uint32_t mach_vm_size_t;

typedef uint32_t mach_msg_return_t;

typedef uintptr_t mach_vm_address_t;

typedef uint32_t dispatch_mach_msg_t;

typedef uint32_t dispatch_mach_t;

typedef uint32_t dispatch_mach_reason_t;

typedef uint32_t voucher_activity_mode_t;

typedef uint32_t voucher_activity_trace_id_t;

typedef uint32_t voucher_activity_id_t;

typedef uint32_t _voucher_activity_buffer_hook_t;;

typedef uint32_t voucher_activity_flag_t;

typedef struct
{
} mach_msg_header_t;


typedef void (*dispatch_mach_handler_function_t)(void*, dispatch_mach_reason_t,
						 dispatch_mach_msg_t, mach_error_t);

typedef void (*dispatch_mach_msg_destructor_t)(void*);

typedef uint32_t voucher_activity_mode_t;

struct voucher_offsets_s {
  uint32_t vo_version;
};


/*
 * Stub out defines for other missing types
 */

// Pulled from OS X man page for kevent
struct kevent64_s {
    uint64_t        ident;          /* identifier for this event */
    int16_t         filter;         /* filter for event */
    uint16_t        flags;          /* general flags */
    uint32_t        fflags;         /* filter-specific flags */
    int64_t         data;           /* filter-specific data */
    uint64_t        udata;          /* opaque user data identifier */
    uint64_t        ext[2];         /* filter-specific extensions */
};


// PAGE_SIZE and SIZE_T_MAX should not be hardcoded like this here.
#define PAGE_SIZE (4096)
#define SIZE_T_MAX (0x7fffffff)

// Define to 0 the NOTE_ values that are not present on Linux.
// Revisit this...would it be better to ifdef out the uses instead??
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


// These and similar macros come from Availabilty.h on OS X
// Need a better way to do this long term.
#define __OSX_AVAILABLE_BUT_DEPRECATED(a,b,c,d) //
#define __OSX_AVAILABLE_BUT_DEPRECATED_MSG(a,b,c,d,msg) //


// Print a warning when an unported code path executes.
#define LINUX_PORT_ERROR()  do { printf("LINUX_PORT_ERROR_CALLED %s:%d: %s\n",__FILE__,__LINE__,__FUNCTION__); } while (0)


#endif /* __OS_LINUX_BASE__ */
