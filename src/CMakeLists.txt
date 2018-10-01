
include(SwiftSupport)
include(DTrace)

if(ENABLE_SWIFT)
    set(dispatch_targets dispatch dispatch_static dispatch_objects)
else()
   set(dispatch_targets dispatch)
endif()

function(dispatch_add_library)
    # Swift builds both shared and static libraries
    if(ENABLE_SWIFT)
        add_library(dispatch_objects OBJECT ${ARGV})
        set_property(TARGET dispatch_objects PROPERTY POSITION_INDEPENDENT_CODE ON)
        add_library(dispatch SHARED $<TARGET_OBJECTS:dispatch_objects>)
        set_target_properties(dispatch PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/swift/${SWIFT_OS}
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/swift/${SWIFT_OS}
        )
        add_library(dispatch_static STATIC $<TARGET_OBJECTS:dispatch_objects>)
        set_target_properties(dispatch_static PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/swift_static/${SWIFT_OS}
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/swift_static/${SWIFT_OS}
            OUTPUT_NAME dispatch
        )
    else()
        add_library(dispatch ${ARGV})
    endif()
endfunction()

function(dispatch_target_sources)
    if(ENABLE_SWIFT)
        target_sources(dispatch_objects ${ARGV})
    else()
        target_sources(dispatch ${ARGV})
    endif()
endfunction()

function(dispatch_target_include_directories)
    foreach(target IN LISTS dispatch_targets)
        target_include_directories(${target} ${ARGV})
    endforeach()
endfunction()

function(dispatch_target_compile_definitions)
    foreach(target IN LISTS dispatch_targets)
        target_compile_definitions(${target} ${ARGV})
    endforeach()
endfunction()

function(dispatch_target_compile_options)
    foreach(target IN LISTS dispatch_targets)
        target_compile_options(${target} ${ARGV})
    endforeach()
endfunction()

function(dispatch_target_link_libraries)
    foreach(target IN LISTS dispatch_targets)
        if(NOT target STREQUAL "dispatch_objects")
            target_link_libraries(${target} ${ARGV})
        endif()
    endforeach()
endfunction()

function(dispatch_target_set_property)
    foreach(target IN LISTS dispatch_targets)
        set_property(TARGET ${target} ${ARGV})
    endforeach()
endfunction()

dispatch_add_library(
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
              shims/yield.h)
if(UNIX)
  dispatch_target_sources(
                 PRIVATE
                   shims/generic_unix_stubs.c
                   shims/generic_unix_stubs.h)
elseif(WIN32)
  dispatch_target_sources(
                 PRIVATE
                   shims/generic_win_stubs.c
                   shims/generic_win_stubs.h)
endif()
if(DISPATCH_USE_INTERNAL_WORKQUEUE)
  dispatch_target_sources(
                 PRIVATE
                   event/workqueue.c
                   event/workqueue_internal.h)
endif()
dispatch_target_sources(
               PRIVATE
                 block.cpp)
if(HAVE_OBJC)
  dispatch_target_sources(dispatch
                 PRIVATE
                   data.m
                   object.m)
endif()
if(ENABLE_SWIFT)
  set(swift_optimization_flags)
  if(CMAKE_BUILD_TYPE MATCHES Release)
    set(swift_optimization_flags -O)
  endif()
  add_swift_library(swiftDispatch
                    MODULE_NAME
                      Dispatch
                    MODULE_LINK_NAME
                      dispatch
                    MODULE_PATH
                      ${CMAKE_CURRENT_BINARY_DIR}/swift/Dispatch.swiftmodule
                    OUTPUT
                      ${CMAKE_CURRENT_BINARY_DIR}/swiftDispatch.o
                    SOURCES
                      swift/Block.swift
                      swift/Data.swift
                      swift/Dispatch.swift
                      swift/IO.swift
                      swift/Private.swift
                      swift/Queue.swift
                      swift/Source.swift
                      swift/Time.swift
                      swift/Wrapper.swift
                    TARGET
                      ${CMAKE_C_COMPILER_TARGET}
                    CFLAGS
                      -fblocks
                      -fmodule-map-file=${CMAKE_SOURCE_DIR}/dispatch/module.modulemap
                    SWIFT_FLAGS
                      -I ${CMAKE_SOURCE_DIR}
                      -I/usr/include
                      ${swift_optimization_flags}
                    DEPENDS
                      ${CMAKE_SOURCE_DIR}/dispatch/module.modulemap)
  dispatch_target_sources(
                 PRIVATE
                   swift/DispatchStubs.cc)
  foreach(target dispatch dispatch_static)
    add_dependencies(${target} swiftDispatch)
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/swiftDispatch.o)
  endforeach()
endif()
if(ENABLE_DTRACE)
  dtrace_usdt_probe(${CMAKE_CURRENT_SOURCE_DIR}/provider.d
                    OUTPUT_SOURCES
                      dispatch_dtrace_provider_headers)
  dispatch_target_sources(
                 PRIVATE
                   ${dispatch_dtrace_provider_headers})
endif()
dispatch_target_include_directories(
                           PRIVATE
                             ${CMAKE_BINARY_DIR}
                             ${CMAKE_SOURCE_DIR}
                             ${CMAKE_CURRENT_SOURCE_DIR}
                             ${CMAKE_CURRENT_BINARY_DIR}
                             ${CMAKE_SOURCE_DIR}/private)
if(WITH_BLOCKS_RUNTIME)
  dispatch_target_include_directories(
                             SYSTEM BEFORE PRIVATE
                               "${WITH_BLOCKS_RUNTIME}")
endif()
if(WIN32)
  dispatch_target_compile_definitions(
                             PRIVATE
                               _CRT_NONSTDC_NO_WARNINGS)
endif()
if("${CMAKE_C_SIMULATE_ID}" STREQUAL "MSVC")
  dispatch_target_compile_options(PRIVATE /EHsc-)
else()
  dispatch_target_compile_options(PRIVATE -fno-exceptions)
endif()
if(DISPATCH_ENABLE_ASSERTS)
  dispatch_target_compile_definitions(
                             PRIVATE
                               DISPATCH_DEBUG=1)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  dispatch_target_compile_definitions(
                             PRIVATE
                               _CRT_SECURE_NO_WARNINGS)
elseif(CMAKE_SYSTEM_NAME STREQUAL Android)
  dispatch_target_compile_options(
                         PRIVATE
                           -U_GNU_SOURCE)
endif()
if(BSD_OVERLAY_FOUND)
  dispatch_target_compile_options(
                         PRIVATE
                           ${BSD_OVERLAY_CFLAGS})
endif()
if("${CMAKE_C_SIMULATE_ID}" STREQUAL "MSVC")
  dispatch_target_compile_options(
                         PRIVATE
                           /W3)
else()
  dispatch_target_compile_options(
                         PRIVATE
                           -Wall)
endif()
# FIXME(compnerd) add check for -fblocks?
if("${CMAKE_C_SIMULATE_ID}" STREQUAL "MSVC")
  dispatch_target_compile_options(
                         PRIVATE
                           -Xclang -fblocks)
else()
  # FIXME(compnerd) add check for -momit-leaf-frame-pointer?
  dispatch_target_compile_options(
                         PRIVATE
                           -fblocks
                           -momit-leaf-frame-pointer)
endif()
if(BSD_OVERLAY_FOUND)
  dispatch_target_link_libraries(PRIVATE ${BSD_OVERLAY_LDFLAGS})
endif()
dispatch_target_link_libraries(PRIVATE Threads::Threads)
if(WITH_BLOCKS_RUNTIME)
  dispatch_target_link_libraries(PRIVATE BlocksRuntime)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  dispatch_target_link_libraries(
                        PRIVATE
                          WS2_32
                          WinMM
                          synchronization)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
  dispatch_target_set_property(
                 APPEND_STRING
                 PROPERTY LINK_FLAGS
                   "-Xlinker -compatibility_version -Xlinker 1"
                   "-Xlinker -current_version -Xlinker ${VERSION}"
                   "-Xlinker -dead_strip"
                   "-Xlinker -alias_list -Xlinker ${CMAKE_SOURCE_DIR}/xcodeconfig/libdispatch.aliases")
endif()
if(USE_GOLD_LINKER)
  dispatch_target_set_property(
                 APPEND_STRING
                 PROPERTY LINK_FLAGS
                   -fuse-ld=gold)
endif()
if(USE_LLD_LINKER)
  dispatch_target_set_property(
                 APPEND_STRING
                 PROPERTY LINK_FLAGS
                   -fuse-ld=lld)
endif()

install(TARGETS
          dispatch
        DESTINATION
          "${INSTALL_TARGET_DIR}")
if(ENABLE_SWIFT)
  install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/swift/Dispatch.swiftmodule
            ${CMAKE_CURRENT_BINARY_DIR}/swift/Dispatch.swiftdoc
          DESTINATION
            "${INSTALL_TARGET_DIR}/${CMAKE_SYSTEM_PROCESSOR}")
  install(TARGETS
            dispatch_static
          DESTINATION
            "${INSTALL_STATIC_TARGET_DIR}")
endif()

