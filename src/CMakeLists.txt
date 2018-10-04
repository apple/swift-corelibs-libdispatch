
include(SwiftSupport)
include(DTrace)

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
              shims/linux_stubs.c
              shims/linux_stubs.h
              shims/lock.c
              shims/lock.h
              shims/perfmon.h
              shims/time.h
              shims/tsd.h
              shims/yield.h)
if(DISPATCH_USE_INTERNAL_WORKQUEUE)
  target_sources(dispatch
                 PRIVATE
                   event/workqueue.c
                   event/workqueue_internal.h)
endif()
target_sources(dispatch
               PRIVATE
                 block.cpp)
if(HAVE_OBJC)
  target_sources(dispatch
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
                    CFLAGS
                      -fblocks
                      -fmodule-map-file=${CMAKE_SOURCE_DIR}/dispatch/module.modulemap
                    SWIFT_FLAGS
                      -I ${CMAKE_SOURCE_DIR}
                      ${swift_optimization_flags})
  add_dependencies(swiftDispatch
                     module-map-symlinks)
  target_sources(dispatch
                 PRIVATE
                   swift/DispatchStubs.cc
                   ${CMAKE_CURRENT_BINARY_DIR}/swiftDispatch.o)
endif()
if(dtrace_EXECUTABLE)
  dtrace_usdt_probe(${CMAKE_CURRENT_SOURCE_DIR}/provider.d
                    OUTPUT_SOURCES
                      dispatch_dtrace_provider_headers)
  target_sources(dispatch
                 PRIVATE
                   ${dispatch_dtrace_provider_headers})
endif()
target_include_directories(dispatch
                           PRIVATE
                             ${CMAKE_BINARY_DIR}
                             ${CMAKE_SOURCE_DIR}
                             ${CMAKE_CURRENT_SOURCE_DIR}
                             ${CMAKE_CURRENT_BINARY_DIR}
                             ${CMAKE_SOURCE_DIR}/private)
if(WITH_BLOCKS_RUNTIME)
  target_include_directories(dispatch
                             SYSTEM BEFORE PRIVATE
                               "${WITH_BLOCKS_RUNTIME}")
endif()
if("${CMAKE_C_SIMULATE_ID}" STREQUAL "MSVC")
  target_compile_options(dispatch PRIVATE /EHsc-)
else()
  target_compile_options(dispatch PRIVATE -fno-exceptions)
endif()
if(DISPATCH_ENABLE_ASSERTS)
  target_compile_definitions(dispatch
                             PRIVATE
                               -DDISPATCH_DEBUG=1)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  target_compile_definitions(dispatch
                             PRIVATE
                               -D_CRT_SECURE_NO_WARNINGS)
endif()
if(BSD_OVERLAY_FOUND)
  target_compile_options(dispatch
                         PRIVATE
                           ${BSD_OVERLAY_CFLAGS})
endif()
if("${CMAKE_C_SIMULATE_ID}" STREQUAL "MSVC")
  target_compile_options(dispatch
                         PRIVATE
                           /W3)
else()
  target_compile_options(dispatch
                         PRIVATE
                           -Wall)
endif()
# FIXME(compnerd) add check for -fblocks?
if("${CMAKE_C_SIMULATE_ID}" STREQUAL "MSVC")
  target_compile_options(dispatch
                         PRIVATE
                           -Xclang -fblocks)
else()
  # FIXME(compnerd) add check for -momit-leaf-frame-pointer?
  target_compile_options(dispatch
                         PRIVATE
                           -fblocks
                           -momit-leaf-frame-pointer)
endif()
if(BSD_OVERLAY_FOUND)
  target_link_libraries(dispatch PRIVATE ${BSD_OVERLAY_LDFLAGS})
endif()
target_link_libraries(dispatch PRIVATE Threads::Threads)
if(WITH_BLOCKS_RUNTIME)
  target_link_libraries(dispatch PRIVATE BlocksRuntime)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
  set_property(TARGET dispatch
               APPEND_STRING
               PROPERTY LINK_FLAGS
                 "-Xlinker -compatibility_version -Xlinker 1"
                 "-Xlinker -current_version -Xlinker ${VERSION}"
                 "-Xlinker -dead_strip"
                 "-Xlinker -alias_list -Xlinker ${CMAKE_SOURCE_DIR}/xcodeconfig/libdispatch.aliases")
endif()
if(USE_GOLD_LINKER)
  set_property(TARGET dispatch
               APPEND_STRING
               PROPERTY LINK_FLAGS
                 -fuse-ld=gold)
endif()

# Temporary staging; the various swift projects that depend on libdispatch
# all expect libdispatch.so to be in src/.libs/libdispatch.so
# So for now, make a copy so we don't have to do a coordinated commit across
# all the swift projects to change this assumption.
add_custom_command(TARGET dispatch POST_BUILD
                   COMMAND cmake -E make_directory .libs
                   COMMAND cmake -E copy $<TARGET_FILE:dispatch> .libs
                   COMMENT "Copying libdispatch to .libs")

install(TARGETS
          dispatch
        DESTINATION
          "${CMAKE_INSTALL_LIBDIR}")
if(ENABLE_SWIFT)
  install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/swift/Dispatch.swiftmodule
            ${CMAKE_CURRENT_BINARY_DIR}/swift/Dispatch.swiftdoc
          DESTINATION
            "${CMAKE_INSTALL_LIBDIR}/swift/${SWIFT_OS}/${CMAKE_SYSTEM_PROCESSOR}")
endif()

