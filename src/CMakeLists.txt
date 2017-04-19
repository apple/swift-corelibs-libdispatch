
set(dispatch_BLOCK_SOURCES block.cpp)
if(HAVE_OBJC)
  list(APPEND dispatch_BLOCK_SOURCES data.m object.m)
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
              shims/yield.h
              ${dispatch_BLOCK_SOURCES})
target_include_directories(dispatch
                           PRIVATE
                             ${CMAKE_BINARY_DIR}
                             ${CMAKE_SOURCE_DIR}
                             ${CMAKE_CURRENT_SOURCE_DIR}
                             ${CMAKE_SOURCE_DIR}/private)
if(WITH_PTHREAD_WORKQUEUES)
  target_include_directories(dispatch
                             SYSTEM BEFORE PRIVATE
                               "${WITH_PTHREAD_WORKQUEUES}/include")
endif()
if(WITH_BLOCKS_RUNTIME)
  target_include_directories(dispatch
                             SYSTEM BEFORE PRIVATE
                               "${WITH_BLOCKS_RUNTIME}")
endif()
# TODO(compnerd) make this portable
target_compile_options(dispatch PRIVATE -fno-exceptions)
if(DISPATCH_ENABLE_ASSERTS)
  target_compile_definitions(dispatch
                             PRIVATE
                               -DDISPATCH_DEBUG=1)
endif()
if(BSD_OVERLAY_FOUND)
  target_compile_options(dispatch
                         PRIVATE
                           ${BSD_OVERLAY_CFLAGS})
endif()
# FIXME(compnerd) add check for -momit-leaf-frame-pointer?
target_compile_options(dispatch
                       PRIVATE
                         -Wall
                         -fblocks
                         -momit-leaf-frame-pointer)
if(BSD_OVERLAY_FOUND)
  target_link_libraries(dispatch PRIVATE ${BSD_OVERLAY_LDFLAGS})
endif()
target_link_libraries(dispatch PRIVATE Threads::Threads)
if(WITH_PTHREAD_WORKQUEUES)
  target_link_libraries(dispatch PRIVATE PTHREAD::workqueue)
endif()
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

install(TARGETS
          dispatch
        DESTINATION
          "${CMAKE_INSTALL_FULL_LIBDIR}")

