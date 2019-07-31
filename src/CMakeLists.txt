include(CheckCCompilerFlag)

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

set_target_properties(dispatch
                      PROPERTIES
                        POSITION_INDEPENDENT_CODE YES)

if(WIN32)
  target_sources(dispatch
                 PRIVATE
                   shims/generic_sys_queue.h
                   shims/generic_win_stubs.c
                   shims/generic_win_stubs.h
                   shims/getprogname.c)
endif()
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
  # TODO(compnerd) split DispatchStubs.cc into a separate component for the ObjC
  # registration and a separate component for the swift compiler's emission of a
  # call to the ObjC autorelease elision entry point.
  target_sources(dispatch
                 PRIVATE
                   data.m
                   object.m
                   swift/DispatchStubs.cc)
endif()
if(ENABLE_SWIFT)
  set(swift_optimization_flags)
  if(NOT CMAKE_BUILD_TYPE MATCHES Debug)
    set(swift_optimization_flags -O)
  endif()

  # NOTE(compnerd) Today regardless of whether or not ObjC interop is enabled,
  # swift will use an autoreleased return value convention for certain CF
  # functions (including some that are used/related to dispatch). This means
  # that the swift compiler in callers to such functions will call the function,
  # and then pass the result of the function to
  # objc_retainAutoreleasedReturnValue. In a context where we have ObjC interop
  # disabled, we do not have access to the objc runtime so an implementation of
  # objc_retainAutoreleasedReturnValue is not available. To work around this, we
  # provide a shim for objc_retainAutoreleasedReturnValue in DispatchStubs.cc
  # that just calls retain on the object. Once we fix the swift compiler to
  # switch to a different model for handling these arguments with objc-interop
  # disabled these shims can be eliminated.
  add_library(DispatchStubs
              STATIC
                swift/DispatchStubs.cc)
  target_include_directories(DispatchStubs
                             PRIVATE
                               ${PROJECT_SOURCE_DIR})
  set_target_properties(DispatchStubs
                        PROPERTIES
                          POSITION_INDEPENDENT_CODE YES)

  if(USE_LLD_LINKER)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL Windows)
      set(use_ld_flag -use-ld=lld.exe)
    else()
      set(use_ld_flag -use-ld=lld)
    endif()
  elseif(USE_GOLD_LINKER)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL Windows)
      set(use_ld_flag -use-ld=gold.exe)
    else()
      set(use_ld_flag -use-ld=gold)
    endif()
  endif()

  add_swift_library(swiftDispatch
                    CFLAGS
                      -fblocks
                      -fmodule-map-file=${PROJECT_SOURCE_DIR}/dispatch/module.modulemap
                    DEPENDS
                      module-maps
                      DispatchStubs
                    LINK_FLAGS
                      ${use_ld_flag}
                      -lDispatchStubs
                      -L $<TARGET_LINKER_FILE_DIR:BlocksRuntime>
                      -lBlocksRuntime
                      -L $<TARGET_LINKER_FILE_DIR:dispatch>
                      -ldispatch
                      $<$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>:-lmsvcrtd>
                      $<$<AND:$<PLATFORM_ID:Windows>,$<NOT:$<CONFIG:Debug>>>:-lmsvcrt>
                    MODULE_NAME
                      Dispatch
                    MODULE_LINK_NAME
                      swiftDispatch
                    MODULE_PATH
                      ${CMAKE_CURRENT_BINARY_DIR}/swift/Dispatch.swiftmodule
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
                    SWIFT_FLAGS
                      -I ${PROJECT_SOURCE_DIR}
                      ${swift_optimization_flags}
                      $<$<PLATFORM_ID:Windows>:-Xcc>
                      $<$<PLATFORM_ID:Windows>:-D_MT>
                      # TODO(compnerd) handle /MT builds
                      $<$<PLATFORM_ID:Windows>:-Xcc>
                      $<$<PLATFORM_ID:Windows>:-D_DLL>
                    TARGET
                      ${CMAKE_SWIFT_COMPILER_TARGET})
endif()
if(ENABLE_DTRACE)
  dtrace_usdt_probe(${CMAKE_CURRENT_SOURCE_DIR}/provider.d
                    OUTPUT_SOURCES
                      dispatch_dtrace_provider_headers)
  target_sources(dispatch
                 PRIVATE
                   ${dispatch_dtrace_provider_headers})
endif()
target_include_directories(dispatch
                           PRIVATE
                             ${PROJECT_BINARY_DIR}
                             ${PROJECT_SOURCE_DIR}
                             ${CMAKE_CURRENT_SOURCE_DIR}
                             ${CMAKE_CURRENT_BINARY_DIR}
                             ${PROJECT_SOURCE_DIR}/private)
target_include_directories(dispatch
                           SYSTEM BEFORE PRIVATE
                             "${BlocksRuntime_INCLUDE_DIR}")
if(WIN32)
  target_compile_definitions(dispatch
                             PRIVATE
                               _CRT_NONSTDC_NO_WARNINGS)
endif()
if("${CMAKE_C_SIMULATE_ID}" STREQUAL "MSVC")
  target_compile_options(dispatch PRIVATE /EHs-c-)
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
elseif(CMAKE_SYSTEM_NAME STREQUAL Android)
  target_compile_options(dispatch
                         PRIVATE
                           -U_GNU_SOURCE)
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
  check_c_compiler_flag("-momit-leaf-frame-pointer -Werror -Wall -O3" C_SUPPORTS_OMIT_LEAF_FRAME_POINTER)
  target_compile_options(dispatch PRIVATE -fblocks)
  if (C_SUPPORTS_OMIT_LEAF_FRAME_POINTER)
    target_compile_options(dispatch PRIVATE -momit-leaf-frame-pointer)
  endif()
endif()
if(LibRT_FOUND)
  target_link_libraries(dispatch PRIVATE RT::rt)
endif()
target_link_libraries(dispatch
                      PRIVATE
                        Threads::Threads
                        BlocksRuntime::BlocksRuntime)
if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  target_link_libraries(dispatch
                        PRIVATE
                          ShLwApi
                          WS2_32
                          WinMM
                          synchronization)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
  set_property(TARGET dispatch
               APPEND_STRING
               PROPERTY LINK_FLAGS
                 "-Xlinker -compatibility_version -Xlinker 1"
                 "-Xlinker -current_version -Xlinker ${VERSION}"
                 "-Xlinker -dead_strip"
                 "-Xlinker -alias_list -Xlinker ${PROJECT_SOURCE_DIR}/xcodeconfig/libdispatch.aliases")
endif()
dispatch_set_linker(dispatch)

install(TARGETS
          dispatch
        ARCHIVE DESTINATION ${INSTALL_TARGET_DIR}
        LIBRARY DESTINATION ${INSTALL_TARGET_DIR}
        RUNTIME DESTINATION bin)

if(ENABLE_SWIFT)
  install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/swift/Dispatch.swiftmodule
            ${CMAKE_CURRENT_BINARY_DIR}/swift/Dispatch.swiftdoc
          DESTINATION
            ${INSTALL_TARGET_DIR}/${swift_arch})

  if(BUILD_SHARED_LIBS)
    set(library_kind SHARED)
  else()
    set(library_kind STATIC)
  endif()

  set(swiftDispatch_OUTPUT_FILE
      ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_${library_kind}_LIBRARY_PREFIX}swiftDispatch${CMAKE_${library_kind}_LIBRARY_SUFFIX})

  if(CMAKE_SYSTEM_NAME STREQUAL Windows AND BUILD_SHARED_LIBS)
    install(FILES
              ${swiftDispatch_OUTPUT_FILE}
            DESTINATION
              bin)
    install(FILES
              ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_IMPORT_LIBRARY_PREFIX}swiftDispatch${CMAKE_IMPORT_LIBRARY_SUFFIX}
            DESTINATION
              ${INSTALL_TARGET_DIR})
  else()
    install(FILES
              ${swiftDispatch_OUTPUT_FILE}
            DESTINATION
              ${INSTALL_TARGET_DIR})
  endif()

  if(NOT BUILD_SHARED_LIBS)
    install(FILES
              $<TARGET_FILE:DispatchStubs>
            DESTINATION
              ${INSTALL_TARGET_DIR})
  endif()
endif()

