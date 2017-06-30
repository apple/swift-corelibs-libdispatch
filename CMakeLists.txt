
cmake_minimum_required(VERSION 3.4.3)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

project(dispatch
        VERSION 1.3
        LANGUAGES C CXX)
enable_testing()

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_STANDARD 11)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

include(CheckCSourceCompiles)
include(CheckFunctionExists)
include(CheckIncludeFiles)
include(CheckLibraryExists)
include(CheckSymbolExists)
include(GNUInstallDirs)

set(WITH_BLOCKS_RUNTIME "" CACHE PATH "Path to blocks runtime")

include(DispatchAppleOptions)

option(ENABLE_DISPATCH_INIT_CONSTRUCTOR "enable libdispatch_init as a constructor" ON)
set(USE_LIBDISPATCH_INIT_CONSTRUCTOR ${ENABLE_DISPATCH_INIT_CONSTRUCTOR})

# TODO(compnerd) swift options

option(BUILD_SHARED_LIBS "build shared libraries" ON)

option(ENABLE_TESTING "build libdispatch tests" ON)

if(CMAKE_SYSTEM_NAME STREQUAL Linux OR
   CMAKE_SYSTEM_NAME STREQUAL Android)
  set(USE_GOLD_LINKER_DEFAULT ON)
else()
  set(USE_GOLD_LINKER_DEFAULT OFF)
endif()
option(USE_GOLD_LINKER "use the gold linker" ${USE_GOLD_LINKER_DEFAULT})

option(ENABLE_THREAD_LOCAL_STORAGE "enable usage of thread local storage via __thread" ON)
set(DISPATCH_USE_THREAD_LOCAL_STORAGE ${ENABLE_THREAD_LOCAL_STORAGE})

if(CMAKE_SYSTEM_NAME STREQUAL Linux OR
   CMAKE_SYSTEM_NAME STREQUAL Android OR
   CMAKE_SYSTEM_NAME STREQUAL Windows)
  set(ENABLE_INTERNAL_PTHREAD_WORKQUEUES_DEFAULT ON)
else()
  set(ENABLE_INTERNAL_PTHREAD_WORKQUEUES_DEFAULT OFF)
endif()
option(ENABLE_INTERNAL_PTHREAD_WORKQUEUES "use libdispatch's own implementation of pthread workqueues" ${ENABLE_INTERNAL_PTHREAD_WORKQUEUES_DEFAULT})
if(ENABLE_INTERNAL_PTHREAD_WORKQUEUES)
  set(DISPATCH_USE_INTERNAL_WORKQUEUE 1)
  set(HAVE_PTHREAD_WORKQUEUES 0)
else()
  check_include_files(pthread/workqueue_private.h HAVE_PTHREAD_WORKQUEUE_PRIVATE_H)
  check_include_files(pthread_workqueue.h HAVE_PTHREAD_WORKQUEUE_H)
  if(HAVE_PTHREAD_WORKQUEUE_PRIVATE_H AND HAVE_PTHREAD_WORKQUEUE_H)
    set(HAVE_PTHREAD_WORKQUEUES 1)
    set(DISPATCH_USE_INTERNAL_WORKQUEUE 0)
  else()
    set(HAVE_PTHREAD_WORKQUEUES 0)
    set(DISPATCH_USE_INTERNAL_WORKQUEUE 1)
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL Linux OR
   CMAKE_SYSTEM_NAME STREQUAL Android OR
   CMAKE_SYSTEM_NAME STREQUAL Windows)
  add_library(BlocksRuntime
              STATIC
                ${CMAKE_SOURCE_DIR}/src/BlocksRuntime/data.c
                ${CMAKE_SOURCE_DIR}/src/BlocksRuntime/runtime.c)
  set_target_properties(BlocksRuntime
                        PROPERTIES
                          POSITION_INDEPENDENT_CODE TRUE)
  if(HAVE_OBJC AND CMAKE_DL_LIBS)
    set_target_properties(BlocksRuntime
                          PROPERTIES
                            INTERFACE_LINK_LIBRARIES ${CMAKE_DL_LIBS})
  endif()
  set(WITH_BLOCKS_RUNTIME "${CMAKE_SOURCE_DIR}/src/BlocksRuntime" CACHE PATH "Path to blocks runtime" FORCE)
else()
  # TODO(compnerd) support system installed BlocksRuntime
  # find_package(BlocksRuntime REQUIRED)
endif()

check_symbol_exists(__GNU_LIBRARY__ "features.h" _GNU_SOURCE)
if(_GNU_SOURCE)
  set(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS} -D_GNU_SOURCE)
endif()

check_c_source_compiles("void __attribute__((__noreturn__)) main() { __builtin_trap(); }"
                        __BUILTIN_TRAP)
if(__BUILTIN_TRAP)
  set(HAVE_NORETURN_BUILTIN_TRAP 1)
endif()

find_package(LibRT)

check_function_exists(_pthread_workqueue_init HAVE__PTHREAD_WORKQUEUE_INIT)
check_function_exists(getprogname HAVE_GETPROGNAME)
check_function_exists(mach_absolute_time HAVE_MACH_ABSOLUTE_TIME)
check_function_exists(mach_approximate_time HAVE_MACH_APPROXIMATE_TIME)
check_function_exists(mach_port_construct HAVE_MACH_PORT_CONSTRUCT)
check_function_exists(malloc_create_zone HAVE_MALLOC_CREATE_ZONE)
check_function_exists(pthread_key_init_np HAVE_PTHREAD_KEY_INIT_NP)
check_function_exists(pthread_main_np HAVE_PTHREAD_MAIN_NP)
check_function_exists(pthread_workqueue_setdispatch_np HAVE_PTHREAD_WORKQUEUE_SETDISPATCH_NP)
check_function_exists(strlcpy HAVE_STRLCPY)
check_function_exists(sysconf HAVE_SYSCONF)

if(NOT HAVE_STRLCPY AND NOT HAVE_GETPROGNAME)
  include(FindPkgConfig)
  pkg_check_modules(BSD_OVERLAY libbsd-overlay)
  if(BSD_OVERLAY_FOUND)
    set(HAVE_STRLCPY 1 CACHE INTERNAL "Have function strlcpy" FORCE)
    set(HAVE_GETPROGNAME 1 CACHE INTERNAL "Have function getprogname" FORCE)
  endif()
endif()

find_package(Threads REQUIRED)

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
  set(USE_MACH_SEM 1)
else()
  set(__DARWIN_NON_CANCELABLE 0)
  set(USE_MACH_SEM 0)
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
check_include_files("sys/cdefs.h" HAVE_SYS_CDEFS_H)
check_include_files("sys/guarded.h" HAVE_SYS_GUARDED_H)
check_include_files("sys/stat.h" HAVE_SYS_STAT_H)
check_include_files("sys/types.h" HAVE_SYS_TYPES_H)
check_include_files("unistd.h" HAVE_UNISTD_H)
check_include_files("objc/objc-internal.h" HAVE_OBJC)

check_library_exists(pthread sem_init "" USE_POSIX_SEM)
if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  add_definitions(-DTARGET_OS_WIN32)
  add_definitions(-DUSE_WIN32_SEM)
endif()

check_symbol_exists(CLOCK_UPTIME "time.h" HAVE_DECL_CLOCK_UPTIME)
check_symbol_exists(CLOCK_UPTIME_FAST "time.h" HAVE_DECL_CLOCK_UPTIME_FAST)
check_symbol_exists(CLOCK_MONOTONIC "time.h" HAVE_DECL_CLOCK_MONOTONIC)
check_symbol_exists(CLOCK_REALTIME "time.h" HAVE_DECL_CLOCK_REALTIME)
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

check_symbol_exists(program_invocation_name "errno.h" HAVE_DECL_PROGRAM_INVOCATION_SHORT_NAME)

find_program(dtrace_EXECUTABLE dtrace)
if(dtrace_EXECUTABLE)
  add_definitions(-DDISPATCH_USE_DTRACE=1)
else()
  add_definitions(-DDISPATCH_USE_DTRACE=0)
endif()

find_program(leaks_EXECUTABLE leaks)
if(leaks_EXECUTABLE)
  set(HAVE_LEAKS TRUE)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
  add_custom_command(OUTPUT
                       "${CMAKE_SOURCE_DIR}/dispatch/module.modulemap"
                       "${CMAKE_SOURCE_DIR}/private/module.modulemap"
                     COMMAND
                       ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/darwin/module.modulemap" "${CMAKE_SOURCE_DIR}/dispatch/module.modulemap"
                     COMMAND
                       ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/darwin/module.modulemap" "${CMAKE_SOURCE_DIR}/private/module.modulemap")
else()
  add_custom_command(OUTPUT
                       "${CMAKE_SOURCE_DIR}/dispatch/module.modulemap"
                       "${CMAKE_SOURCE_DIR}/private/module.modulemap"
                     COMMAND
                       ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/generic/module.modulemap" "${CMAKE_SOURCE_DIR}/dispatch/module.modulemap"
                     COMMAND
                       ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/generic/module.modulemap" "${CMAKE_SOURCE_DIR}/private/module.modulemap")
endif()
configure_file("${CMAKE_SOURCE_DIR}/cmake/config.h.in"
               "${CMAKE_BINARY_DIR}/config/config_ac.h")
add_definitions(-DHAVE_CONFIG_H)

add_subdirectory(dispatch)
add_subdirectory(man)
add_subdirectory(os)
add_subdirectory(private)
add_subdirectory(src)
if(ENABLE_TESTING)
  add_subdirectory(tests)
endif()

