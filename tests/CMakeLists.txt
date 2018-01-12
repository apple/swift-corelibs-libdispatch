
execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink "${CMAKE_SOURCE_DIR}/private" "${CMAKE_CURRENT_BINARY_DIR}/dispatch")
execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink "${CMAKE_CURRENT_SOURCE_DIR}/leaks-wrapper.sh" "${CMAKE_CURRENT_BINARY_DIR}/leaks-wrapper")

if(CMAKE_SYSTEM_NAME STREQUAL Linux)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lrt")
endif()

add_library(bsdtests
            STATIC
              bsdtests.c
              dispatch_test.c)
target_include_directories(bsdtests
                           PRIVATE
                             ${CMAKE_CURRENT_BINARY_DIR}
                             ${CMAKE_CURRENT_SOURCE_DIR}
                             ${CMAKE_SOURCE_DIR})
if(BSD_OVERLAY_FOUND)
  target_compile_options(bsdtests
                         PRIVATE
                           ${BSD_OVERLAY_CFLAGS})
endif()

add_executable(bsdtestharness
               bsdtestharness.c)
target_include_directories(bsdtestharness
                           PRIVATE
                             ${CMAKE_CURRENT_BINARY_DIR}
                             ${CMAKE_CURRENT_SOURCE_DIR}
                             ${CMAKE_SOURCE_DIR})
if(BSD_OVERLAY_FOUND)
  target_compile_options(bsdtestharness
                         PRIVATE
                           ${BSD_OVERLAY_CFLAGS})
endif()
target_link_libraries(bsdtestharness
                      PRIVATE
                        bsdtests
                        dispatch)
if(BSD_OVERLAY_FOUND)
  target_link_libraries(bsdtestharness
                        PRIVATE
                          ${BSD_OVERLAY_LDFLAGS})
endif()
if(ENABLE_SWIFT)
  target_link_libraries(bsdtestharness
                        PRIVATE
                          swiftCore
                          swiftSwiftOnoneSupport)
endif()

function(add_unit_test name)
  set(options DISABLED_TEST;NO_BSD_OVERLAY)
  set(single_value_args)
  set(multiple_value_args SOURCES)
  cmake_parse_arguments(AUT "${options}" "${single_value_args}" "${multiple_value_args}" ${ARGN})

  if(AUT_DISABLED_TEST)
    return()
  endif()

  add_executable(${name} ${AUT_SOURCES})
  target_include_directories(${name}
                             PRIVATE
                               ${CMAKE_CURRENT_BINARY_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}
                               ${CMAKE_SOURCE_DIR})
  if(ENABLE_SWIFT)
    # For testing in swift.org CI system; make deadlines lenient by default
    # to reduce probability of test failures due to machine load.
    target_compile_options(${name} PRIVATE -DLENIENT_DEADLINES=1)
    target_link_libraries(${name} PRIVATE swiftCore swiftSwiftOnoneSupport)
  endif()
  if(WITH_BLOCKS_RUNTIME)
    target_include_directories(${name}
                               SYSTEM BEFORE PRIVATE
                                 "${WITH_BLOCKS_RUNTIME}")
  endif()
  if(BSD_OVERLAY_FOUND AND NOT AUT_NO_BSD_OVERLAY)
    target_compile_options(${name}
                           PRIVATE
                             ${BSD_OVERLAY_CFLAGS})
  endif()
  target_compile_options(${name} PRIVATE -fblocks)
  # TODO(compnerd) make this portable
  target_compile_options(${name} PRIVATE -Wall -Wno-deprecated-declarations)
  target_link_libraries(${name} PRIVATE dispatch Threads::Threads)
  if(WITH_BLOCKS_RUNTIME)
    target_link_libraries(${name} PRIVATE BlocksRuntime)
  endif()
  if(BSD_OVERLAY_FOUND AND NOT AUT_NO_BSD_OVERLAY)
    target_link_libraries(${name}
                          PRIVATE
                            ${BSD_OVERLAY_LDFLAGS})
  endif()
  if(ENABLE_SWIFT)
    target_link_libraries(${name}
                          PRIVATE
                            swiftCore
                            swiftSwiftOnoneSupport)
  endif()
  target_link_libraries(${name} PRIVATE bsdtests)
  add_test(NAME ${name}
           COMMAND bsdtestharness $<TARGET_FILE:${name}>)
  set_tests_properties(${name}
                       PROPERTIES
                         TIMEOUT 120
                         DEPENDS bsdtestharness
                         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  if(NOT leaks_EXECUTABLE)
    set_tests_properties(${name}
                         PROPERTIES
                           ENVIRONMENT NOLEAKS=1)
  endif()
endfunction()

# Tests that reliably pass on all platforms
set(DISPATCH_C_TESTS
    apply
    api
    debug
    queue_finalizer
    group
    overcommit
    context_for_key
    after
    timer
    timer_short
    timer_timeout
    sema
    timer_bit31
    timer_bit63
    timer_set_time
    starfish
    data
    io_net
    select)

# Tests that usually pass, but occasionally fail.
# Excluded by default for purposes of Swift CI
if(EXTENDED_TEST_SUITE)
  list(APPEND DISPATCH_C_TESTS
	priority
	concur
	read
	read2
	suspend_timer
	pingpong
	drift
	readsync
	cascade
	io)
  # an oddball; dispatch_priority.c compiled with -DUSE_SET_TARGET_QUEUE=1
  add_unit_test(dispatch_priority2 SOURCES dispatch_priority.c)
  target_compile_options(dispatch_priority2 PRIVATE -DUSE_SET_TARGET_QUEUE=1)
endif()

# add C tests for platform-specific functionality when applicable
if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
  list(APPEND DISPATCH_C_TESTS
	deadname
	proc
	vm
	vnode)
endif()

foreach(test ${DISPATCH_C_TESTS})
  add_unit_test(dispatch_${test}
                SOURCES
                  dispatch_${test}.c)
endforeach()

# test dispatch API for various C/CXX language variants
add_unit_test(dispatch_c99 NO_BSD_OVERLAY SOURCES dispatch_c99.c)
add_unit_test(dispatch_plusplus SOURCES dispatch_plusplus.cpp)

# test-specific link options
target_link_libraries(dispatch_group PRIVATE m)
target_link_libraries(dispatch_timer_short PRIVATE m)

# test-specific compile options
target_compile_options(dispatch_c99 PRIVATE -std=c99)
