
cmake_minimum_required(VERSION 3.15.1)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules)

# NOTE(compnerd) enable CMP0091 - select MSVC runtime based on
# CMAKE_MSVC_RUNTIME_LIBRARY.  Requires CMake 3.15 or newer.
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

project(dispatch
  VERSION 1.3
  LANGUAGES C CXX)

if("${CMAKE_C_SIMULATE_ID}" STREQUAL "MSVC")
  include(ClangClCompileRules)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  include(DispatchWindowsSupport)
  dispatch_windows_arch_spelling(${CMAKE_SYSTEM_PROCESSOR} DISPATCH_MSVC_ARCH)
  dispatch_windows_include_for_arch(${DISPATCH_MSVC_ARCH} DISPATCH_INCLUDES)
  include_directories(BEFORE SYSTEM ${DISPATCH_INCLUDES})
  dispatch_windows_lib_for_arch(${CMAKE_SYSTEM_PROCESSOR} DISPATCH_LIBDIR)
  link_directories(${DISPATCH_LIBDIR})

  include(CheckCSourceCompiles)
  check_c_source_compiles([=[
#include <Windows.h>
int main(int argc, char *argv[]) {
  switch ((LOGICAL_PROCESSOR_RELATIONSHIP)0) {
  case RelationProcessorDie:
  case RelationNumaNodeEx:
    return 0;
  }
  return 0;
}
]=] DISPATCH_HAVE_EXTENDED_SLPI_20348)
  if(DISPATCH_HAVE_EXTENDED_SLPI_20348)
    add_compile_definitions(DISPATCH_HAVE_EXTENDED_SLPI_20348)
  endif()

  check_c_source_compiles([=[
#include <Windows.h>
int main(int argc, char *argv[]) {
  switch ((LOGICAL_PROCESSOR_RELATIONSHIP)0) {
  case RelationProcessorModule:
    return 0;
  }
  return 0;
}
]=] DISPATCH_HAVE_EXTENDED_SLPI_22000)
  if(DISPATCH_HAVE_EXTENDED_SLPI_22000)
    add_compile_definitions(DISPATCH_HAVE_EXTENDED_SLPI_22000)
  endif()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED YES)

set(CMAKE_CXX_STANDARD 11)

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_INLINES_HIDDEN YES)

# NOTE(compnerd) this is a horrible workaround for Windows to ensure that the
# tests can run as there is no rpath equivalent and `PATH` is used to lookup the
# libraries.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

include(CheckCCompilerFlag)
include(CheckCSourceCompiles)
include(CheckFunctionExists)
include(CheckIncludeFiles)
include(CheckLibraryExists)
include(CheckSymbolExists)
include(GNUInstallDirs)
include(CTest)

include(DispatchAppleOptions)
include(DispatchSanitization)
include(DispatchCompilerWarnings)
include(DTrace)
include(SwiftSupport)

# NOTE(abdulras) this is the CMake supported way to control whether we generate
# shared or static libraries.  This impacts the behaviour of `add_library` in
# what type of library it generates.
option(BUILD_SHARED_LIBS "build shared libraries" ON)

option(DISPATCH_ENABLE_ASSERTS "enable debug assertions" FALSE)

option(ENABLE_DISPATCH_INIT_CONSTRUCTOR "enable libdispatch_init as a constructor" ON)
set(USE_LIBDISPATCH_INIT_CONSTRUCTOR ${ENABLE_DISPATCH_INIT_CONSTRUCTOR})

option(ENABLE_DTRACE "enable dtrace support" "")

if(CMAKE_SYSTEM_NAME STREQUAL Darwin OR CMAKE_SYSTEM_NAME STREQUAL FreeBSD)
  set(ENABLE_INTERNAL_PTHREAD_WORKQUEUES_DEFAULT OFF)
else()
  set(ENABLE_INTERNAL_PTHREAD_WORKQUEUES_DEFAULT ON)
endif()
option(ENABLE_INTERNAL_PTHREAD_WORKQUEUES "use libdispatch's own implementation of pthread workqueues" ${ENABLE_INTERNAL_PTHREAD_WORKQUEUES_DEFAULT})
if(ENABLE_INTERNAL_PTHREAD_WORKQUEUES)
  set(DISPATCH_USE_INTERNAL_WORKQUEUE 1)
  set(HAVE_PTHREAD_WORKQUEUES 0)
else()
  check_include_files(pthread/workqueue_private.h HAVE_PTHREAD_WORKQUEUE_PRIVATE_H)
  if(HAVE_PTHREAD_WORKQUEUE_PRIVATE_H)
    set(HAVE_PTHREAD_WORKQUEUES 1)
    set(DISPATCH_USE_INTERNAL_WORKQUEUE 0)
  else()
    set(HAVE_PTHREAD_WORKQUEUES 0)
    set(DISPATCH_USE_INTERNAL_WORKQUEUE 1)
  endif()
endif()

option(INSTALL_PRIVATE_HEADERS "installs private headers in the same location as the public ones" OFF)

option(ENABLE_SWIFT "enable libdispatch swift overlay" OFF)
if(ENABLE_SWIFT)
  enable_language(Swift)
endif()

option(ENABLE_THREAD_LOCAL_STORAGE "enable usage of thread local storage via _Thread_local" ON)
set(DISPATCH_USE_THREAD_LOCAL_STORAGE ${ENABLE_THREAD_LOCAL_STORAGE})


check_symbol_exists(__GNU_LIBRARY__ "features.h" _GNU_SOURCE)
if(_GNU_SOURCE)
  set(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS} -D_GNU_SOURCE)
endif()

check_c_source_compiles("void __attribute__((__noreturn__)) main() { __builtin_trap(); }"
                        __BUILTIN_TRAP)
if(__BUILTIN_TRAP)
  set(HAVE_NORETURN_BUILTIN_TRAP 1)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL Android)
  find_package(LibRT)
endif()

check_function_exists(_pthread_workqueue_init HAVE__PTHREAD_WORKQUEUE_INIT)
check_function_exists(getprogname HAVE_GETPROGNAME)
check_function_exists(mach_absolute_time HAVE_MACH_ABSOLUTE_TIME)
check_function_exists(mach_approximate_time HAVE_MACH_APPROXIMATE_TIME)
check_function_exists(mach_port_construct HAVE_MACH_PORT_CONSTRUCT)
check_function_exists(malloc_create_zone HAVE_MALLOC_CREATE_ZONE)
check_function_exists(posix_fadvise HAVE_POSIX_FADVISE)
check_function_exists(posix_spawnp HAVE_POSIX_SPAWNP)
check_function_exists(pthread_key_init_np HAVE_PTHREAD_KEY_INIT_NP)
check_function_exists(pthread_attr_setcpupercent_np HAVE_PTHREAD_ATTR_SETCPUPERCENT_NP)
check_function_exists(pthread_yield_np HAVE_PTHREAD_YIELD_NP)
check_function_exists(pthread_main_np HAVE_PTHREAD_MAIN_NP)
check_function_exists(pthread_workqueue_setdispatch_np HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP)
check_function_exists(strlcpy HAVE_STRLCPY)
check_function_exists(sysconf HAVE_SYSCONF)
check_function_exists(arc4random HAVE_ARC4RANDOM)

check_include_files("TargetConditionals.h" HAVE_TARGETCONDITIONALS_H)
check_include_files("dlfcn.h" HAVE_DLFCN_H)
check_include_files("fcntl.h" HAVE_FCNTL_H)
check_include_files("inttypes.h" HAVE_INTTYPES_H)
check_include_files("libkern/OSAtomic.h" HAVE_LIBKERN_OSATOMIC_H)
check_include_files("libkern/OSCrossEndian.h" HAVE_LIBKERN_OSCROSSENDIAN_H)
check_include_files("libproc_internal.h" HAVE_LIBPROC_INTERNAL_H)
check_include_files("mach/mach.h" HAVE_MACH)
if(HAVE_MACH)
  set(__DARWIN_NON_CANCELABLE 1)
else()
  set(__DARWIN_NON_CANCELABLE 0)
endif()
check_include_files("malloc/malloc.h" HAVE_MALLOC_MALLOC_H)
check_include_files("memory.h" HAVE_MEMORY_H)
check_include_files("pthread/qos.h" HAVE_PTHREAD_QOS_H)
check_include_files("pthread/workqueue_private.h" HAVE_PTHREAD_WORKQUEUE_PRIVATE_H)
check_include_files("pthread_machdep.h" HAVE_PTHREAD_MACHDEP_H)
check_include_files("pthread_np.h" HAVE_PTHREAD_NP_H)
check_include_files("pthread_workqueue.h" HAVE_PTHREAD_WORKQUEUE_H)
check_include_files("stdint.h" HAVE_STDINT_H)
check_include_files("stdlib.h" HAVE_STDLIB_H)
check_include_files("string.h" HAVE_STRING_H)
check_include_files("strings.h" HAVE_STRINGS_H)
check_include_files("sys/guarded.h" HAVE_SYS_GUARDED_H)
check_include_files("sys/stat.h" HAVE_SYS_STAT_H)
check_include_files("sys/types.h" HAVE_SYS_TYPES_H)
check_include_files("objc/objc-internal.h" HAVE_OBJC)

if(HAVE_MACH)
  set(USE_MACH_SEM 1)
else()
  set(USE_MACH_SEM 0)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  add_compile_definitions($<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:USE_WIN32_SEM>)
endif()
check_library_exists(pthread sem_init "" USE_POSIX_SEM)
# NOTE: android has not always provided a libpthread, but uses the pthreads API
if(CMAKE_SYSTEM_NAME STREQUAL Android)
  set(USE_POSIX_SEM 1)
endif()

check_symbol_exists(CLOCK_UPTIME "time.h" HAVE_DECL_CLOCK_UPTIME)
check_symbol_exists(CLOCK_UPTIME_FAST "time.h" HAVE_DECL_CLOCK_UPTIME_FAST)
check_symbol_exists(CLOCK_MONOTONIC "time.h" HAVE_DECL_CLOCK_MONOTONIC)
check_symbol_exists(CLOCK_REALTIME "time.h" HAVE_DECL_CLOCK_REALTIME)
check_symbol_exists(CLOCK_MONOTONIC_COARSE "time.h" HAVE_DECL_CLOCK_MONOTONIC_COARSE)
check_symbol_exists(FD_COPY "sys/select.h" HAVE_DECL_FD_COPY)
check_symbol_exists(NOTE_LOWAT "sys/event.h" HAVE_DECL_NOTE_LOWAT)
check_symbol_exists(NOTE_NONE "sys/event.h" HAVE_DECL_NOTE_NONE)
check_symbol_exists(NOTE_REAP "sys/event.h" HAVE_DECL_NOTE_REAP)
check_symbol_exists(NOTE_REVOKE "sys/event.h" HAVE_DECL_NOTE_REVOKE)
check_symbol_exists(NOTE_SIGNAL "sys/event.h" HAVE_DECL_NOTE_SIGNAL)
check_symbol_exists(POSIX_SPAWN_START_SUSPENDED "sys/spawn.h" HAVE_DECL_POSIX_SPAWN_START_SUSPENDED)
check_symbol_exists(SIGEMT "signal.h" HAVE_DECL_SIGEMT)
check_symbol_exists(VQ_DESIRED_DISK "sys/mount.h" HAVE_DECL_VQ_DESIRED_DISK)
check_symbol_exists(VQ_NEARLOWDISK "sys/mount.h" HAVE_DECL_VQ_NEARLOWDISK)
check_symbol_exists(VQ_QUOTA "sys/mount.h" HAVE_DECL_VQ_QUOTA)
check_symbol_exists(VQ_UPDATE "sys/mount.h" HAVE_DECL_VQ_UPDATE)
check_symbol_exists(VQ_VERYLOWDISK "sys/mount.h" HAVE_DECL_VQ_VERYLOWDISK)
check_symbol_exists(VQ_FREE_SPACE_CHANGE "sys/mount.h" HAVE_DECL_VQ_FREE_SPACE_CHANGE)
check_symbol_exists(strlcpy "string.h" HAVE_STRLCPY)
check_symbol_exists(program_invocation_name "errno.h" HAVE_DECL_PROGRAM_INVOCATION_SHORT_NAME)
if (HAVE_DECL_PROGRAM_INVOCATION_SHORT_NAME)
  add_compile_definitions($<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:_GNU_SOURCE=1>)
endif()
check_symbol_exists(__printflike "bsd/sys/cdefs.h" HAVE_PRINTFLIKE)

if(CMAKE_SYSTEM_NAME STREQUAL Android)
  set(ENABLE_DTRACE_DEFAULT OFF)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL FreeBSD)
  add_compile_definitions($<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:_WITH_DPRINTF>)
endif()

if(ENABLE_DTRACE)
  find_program(dtrace_EXECUTABLE dtrace)
  if(NOT dtrace_EXECUTABLE AND NOT ENABLE_DTRACE STREQUAL "")
    message(FATAL_ERROR "dtrace not found but explicitly requested")
  endif()
endif()

if(dtrace_EXECUTABLE)
  add_compile_definitions($<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:DISPATCH_USE_DTRACE=1>)
else()
  add_compile_definitions($<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:DISPATCH_USE_DTRACE=0>)
endif()

find_program(leaks_EXECUTABLE leaks)
if(leaks_EXECUTABLE)
  set(HAVE_LEAKS TRUE)
endif()


if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
  add_custom_command(OUTPUT
                       "${PROJECT_SOURCE_DIR}/dispatch/module.modulemap"
                       "${PROJECT_SOURCE_DIR}/private/module.modulemap"
                     COMMAND
                       ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/dispatch/darwin/module.modulemap" "${PROJECT_SOURCE_DIR}/dispatch/module.modulemap"
                     COMMAND
                       ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/private/darwin/module.modulemap" "${PROJECT_SOURCE_DIR}/private/module.modulemap")
else()
  add_custom_command(OUTPUT
                       "${PROJECT_SOURCE_DIR}/dispatch/module.modulemap"
                       "${PROJECT_SOURCE_DIR}/private/module.modulemap"
                     COMMAND
                       ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/dispatch/generic/module.modulemap" "${PROJECT_SOURCE_DIR}/dispatch/module.modulemap"
                     COMMAND
                       ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/private/generic/module.modulemap" "${PROJECT_SOURCE_DIR}/private/module.modulemap")
endif()
add_custom_target(module-maps ALL
                  DEPENDS
                     "${PROJECT_SOURCE_DIR}/dispatch/module.modulemap"
                     "${PROJECT_SOURCE_DIR}/private/module.modulemap")

configure_file("${PROJECT_SOURCE_DIR}/cmake/config.h.in"
               "${PROJECT_BINARY_DIR}/config/config_ac.h")
add_compile_definitions($<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:HAVE_CONFIG_H>)


if(ENABLE_SWIFT)
  set(INSTALL_TARGET_DIR "${CMAKE_INSTALL_LIBDIR}/swift$<$<NOT:$<BOOL:${BUILD_SHARED_LIBS}>>:_static>/$<LOWER_CASE:${CMAKE_SYSTEM_NAME}>" CACHE PATH "Path where the libraries will be installed")
  set(INSTALL_DISPATCH_HEADERS_DIR "${CMAKE_INSTALL_LIBDIR}/swift$<$<NOT:$<BOOL:${BUILD_SHARED_LIBS}>>:_static>/dispatch" CACHE PATH "Path where the headers will be installed for libdispatch")
  set(INSTALL_BLOCK_HEADERS_DIR "${CMAKE_INSTALL_LIBDIR}/swift$<$<NOT:$<BOOL:${BUILD_SHARED_LIBS}>>:_static>/Block" CACHE PATH "Path where the headers will be installed for the blocks runtime")
  set(INSTALL_OS_HEADERS_DIR "${CMAKE_INSTALL_LIBDIR}/swift$<$<NOT:$<BOOL:${BUILD_SHARED_LIBS}>>:_static>/os" CACHE PATH "Path where the os/ headers will be installed")
else()
  set(INSTALL_TARGET_DIR "${CMAKE_INSTALL_LIBDIR}" CACHE PATH "Path where the libraries will be installed")
  set(INSTALL_DISPATCH_HEADERS_DIR "include/dispatch" CACHE PATH "Path where the headers will be installed")
  set(INSTALL_BLOCK_HEADERS_DIR "include" CACHE PATH "Path where the headers will be installed for the blocks runtime")
  set(INSTALL_OS_HEADERS_DIR "include/os" CACHE PATH "Path where the headers will be installed")
endif()


add_subdirectory(dispatch)
add_subdirectory(man)
add_subdirectory(os)
add_subdirectory(private)
add_subdirectory(src)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

add_subdirectory(cmake/modules)
