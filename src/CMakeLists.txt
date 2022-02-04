
if(NOT CMAKE_SYSTEM_NAME STREQUAL Darwin)
  add_subdirectory(BlocksRuntime)
endif()

add_library(dispatch
  allocator.c
  apply.c
  benchmark.c
  data.c
  init.c
  introspection.c
  io.c
  mach.c
  object.c
  once.c
  queue.c
  semaphore.c
  source.c
  time.c
  transform.c
  voucher.c
  shims.c
  protocol.defs
  provider.d
  allocator_internal.h
  data_internal.h
  inline_internal.h
  internal.h
  introspection_internal.h
  io_internal.h
  mach_internal.h
  object_internal.h
  queue_internal.h
  semaphore_internal.h
  shims.h
  source_internal.h
  trace.h
  voucher_internal.h
  event/event.c
  event/event_config.h
  event/event_epoll.c
  event/event_internal.h
  event/event_kevent.c
  event/event_windows.c
  firehose/firehose_internal.h
  shims/android_stubs.h
  shims/atomic.h
  shims/atomic_sfb.h
  shims/getprogname.h
  shims/hw_config.h
  shims/lock.c
  shims/lock.h
  shims/perfmon.h
  shims/time.h
  shims/tsd.h
  shims/yield.c
  shims/yield.h)

if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  target_sources(dispatch PRIVATE
    shims/generic_sys_queue.h
    shims/generic_win_stubs.c
    shims/generic_win_stubs.h
    shims/getprogname.c)
endif()

if(DISPATCH_USE_INTERNAL_WORKQUEUE)
  target_sources(dispatch PRIVATE
    event/workqueue.c
    event/workqueue_internal.h)
endif()

target_sources(dispatch PRIVATE
  block.cpp)

if(ENABLE_DTRACE)
  dtrace_usdt_probe(${CMAKE_CURRENT_SOURCE_DIR}/provider.d OUTPUT_SOURCES
    dispatch_dtrace_provider_headers)
  target_sources(dispatch PRIVATE
    ${dispatch_dtrace_provider_headers})
endif()

if(HAVE_OBJC)
  # TODO(compnerd) split DispatchStubs.cc into a separate component for the ObjC
  # registration and a separate component for the swift compiler's emission of a
  # call to the ObjC autorelease elision entry point.
  target_sources(dispatch PRIVATE
    data.m
    object.m
    swift/DispatchStubs.cc)
endif()


set_target_properties(dispatch PROPERTIES
  POSITION_INDEPENDENT_CODE YES)

target_include_directories(dispatch PUBLIC
  ${PROJECT_BINARY_DIR}
  ${PROJECT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(dispatch PRIVATE
  ${PROJECT_SOURCE_DIR}/private)

if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  target_compile_definitions(dispatch PRIVATE
    _CRT_NONSTDC_NO_WARNINGS
    _CRT_SECURE_NO_WARNINGS)
elseif(CMAKE_SYSTEM_NAME STREQUAL Android)
  target_compile_options(dispatch PRIVATE
    -U_GNU_SOURCE)
endif()
if(DISPATCH_ENABLE_ASSERTS)
  target_compile_definitions(dispatch PRIVATE
    DISPATCH_DEBUG=1)
endif()

if("${CMAKE_C_SIMULATE_ID}" STREQUAL "MSVC")
  target_compile_options(dispatch PRIVATE /EHs-c-)
  target_compile_options(dispatch PRIVATE /W3)
else()
  target_compile_options(dispatch PRIVATE -fno-exceptions)
  target_compile_options(dispatch PRIVATE -Wall)
endif()

# FIXME(compnerd) add check for -fblocks?
target_compile_options(dispatch PRIVATE -fblocks)

check_c_compiler_flag("-momit-leaf-frame-pointer -Werror -Wall -O3" C_SUPPORTS_OMIT_LEAF_FRAME_POINTER)
if (C_SUPPORTS_OMIT_LEAF_FRAME_POINTER)
  target_compile_options(dispatch PRIVATE -momit-leaf-frame-pointer)
endif()

if(LibRT_FOUND)
  target_link_libraries(dispatch PRIVATE RT::rt)
endif()
target_link_libraries(dispatch PRIVATE
  Threads::Threads)
target_link_libraries(dispatch PUBLIC
  BlocksRuntime::BlocksRuntime)
if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  target_link_libraries(dispatch PRIVATE
    ShLwApi
    WS2_32
    WinMM
    synchronization)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
  set_property(TARGET dispatch APPEND_STRING PROPERTY LINK_FLAGS
    "-Xlinker -compatibility_version -Xlinker 1"
    "-Xlinker -current_version -Xlinker ${VERSION}"
    "-Xlinker -dead_strip"
    "-Xlinker -alias_list -Xlinker ${PROJECT_SOURCE_DIR}/xcodeconfig/libdispatch.aliases")
endif()

if(NOT CMAKE_SYSTEM_NAME MATCHES "Darwin|Windows")
  set_target_properties(dispatch PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

if(ENABLE_SWIFT)
  add_subdirectory(swift)
endif()

set_property(GLOBAL APPEND PROPERTY DISPATCH_EXPORTS dispatch)
install(TARGETS dispatch
        EXPORT dispatchExports
        ARCHIVE DESTINATION ${INSTALL_TARGET_DIR}
        LIBRARY DESTINATION ${INSTALL_TARGET_DIR}
        RUNTIME DESTINATION bin)
