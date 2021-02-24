
if(CMAKE_SYSTEM_NAME STREQUAL Windows)
    execute_process(COMMAND
                      "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/private"
                      "${CMAKE_CURRENT_BINARY_DIR}/dispatch")
    execute_process(COMMAND
                      "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/leaks-wrapper.sh"
                      "${CMAKE_CURRENT_BINARY_DIR}/leaks-wrapper")
else()
    execute_process(COMMAND
                      "${CMAKE_COMMAND}" -E create_symlink "${PROJECT_SOURCE_DIR}/private"
                      "${CMAKE_CURRENT_BINARY_DIR}/dispatch")
    execute_process(COMMAND
                      "${CMAKE_COMMAND}" -E create_symlink "${CMAKE_CURRENT_SOURCE_DIR}/leaks-wrapper.sh"
                      "${CMAKE_CURRENT_BINARY_DIR}/leaks-wrapper")
endif()

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
                             ${PROJECT_SOURCE_DIR}
                           PUBLIC
                             # bsdtests.h needs config_ac.h
                             ${PROJECT_BINARY_DIR})
if (WIN32)
  target_sources(bsdtests
                 PRIVATE
                   generic_win_port.c)
  target_compile_definitions(bsdtests
                             PUBLIC
                               _CRT_NONSTDC_NO_WARNINGS
                               _CRT_SECURE_NO_WARNINGS
                               _USE_MATH_DEFINES)
  target_link_libraries(bsdtests
                        PUBLIC
                          bcrypt)
endif ()

add_executable(bsdtestharness
               bsdtestharness.c)
target_include_directories(bsdtestharness
                           PRIVATE
                             ${CMAKE_CURRENT_BINARY_DIR}
                             ${CMAKE_CURRENT_SOURCE_DIR}
                             ${PROJECT_SOURCE_DIR})
target_link_libraries(bsdtestharness
                      PRIVATE
                        bsdtests
                        dispatch)

function(add_unit_test name)
  set(options DISABLED_TEST)
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
                               ${PROJECT_SOURCE_DIR})
  if(ENABLE_SWIFT)
    # For testing in swift.org CI system; make deadlines lenient by default
    # to reduce probability of test failures due to machine load.
    target_compile_options(${name} PRIVATE -DLENIENT_DEADLINES=1)
  endif()
  target_include_directories(${name}
                             SYSTEM BEFORE PRIVATE
                               "${BlocksRuntime_INCLUDE_DIR}")
  if("${CMAKE_C_SIMULATE_ID}" STREQUAL "MSVC")
    target_compile_options(${name} PRIVATE -Xclang -fblocks)
    target_compile_options(${name} PRIVATE /W3 -Wno-deprecated-declarations)
  else()
    target_compile_options(${name} PRIVATE -fblocks)
    target_compile_options(${name} PRIVATE -Wall -Wno-deprecated-declarations)
  endif()
  # Without this flag, cross-compiling static test executables for Android armv7
  # fails with the multiple definition errors seen in android/ndk#176, so I
  # pulled in this workaround noted there. The tests build and run with this
  # flag applied.
  if(NOT BUILD_SHARED_LIBS AND CMAKE_SYSTEM_NAME STREQUAL Android AND
     CMAKE_SYSTEM_PROCESSOR STREQUAL armv7-a)
    target_link_options(${name} PRIVATE "LINKER:--allow-multiple-definition")
  endif()
  target_link_libraries(${name}
                        PRIVATE
                          dispatch
                          Threads::Threads
                          BlocksRuntime::BlocksRuntime)
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
    data
    io_muxed
    io_net
    io_pipe
    io_pipe_close
    select)

# Tests that usually pass, but occasionally fail.
# Excluded by default for purposes of Swift CI
if(EXTENDED_TEST_SUITE)
  # When dispatch_group is reenabled here, also remove the if(EXTENDED_TEST_SUITE) condition below
  list(APPEND DISPATCH_C_TESTS
       priority
       concur
       group
       read
       read2
       starfish
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

set_tests_properties(dispatch_io_pipe PROPERTIES TIMEOUT 15)
set_tests_properties(dispatch_io_pipe_close PROPERTIES TIMEOUT 5)

# test dispatch API for various C/CXX language variants
add_unit_test(dispatch_c99 SOURCES dispatch_c99.c)
add_unit_test(dispatch_plusplus SOURCES dispatch_plusplus.cpp)

# test-specific link options
if(WIN32)
  target_link_libraries(dispatch_io_muxed PRIVATE WS2_32)
  target_link_libraries(dispatch_io_net PRIVATE WS2_32)
else()
  # When dispatch_group is reenabled above, remove this
  if(EXTENDED_TEST_SUITE)
    target_link_libraries(dispatch_group PRIVATE m)
  endif()
  target_link_libraries(dispatch_timer_short PRIVATE m)
endif()

# test-specific compile options
set_target_properties(dispatch_c99 PROPERTIES C_STANDARD 99)
