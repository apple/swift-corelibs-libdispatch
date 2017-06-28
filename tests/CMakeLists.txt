
execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink "${CMAKE_SOURCE_DIR}/private" "${CMAKE_CURRENT_BINARY_DIR}/dispatch")
execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink "${CMAKE_CURRENT_SOURCE_DIR}/leaks-wrapper.sh" "${CMAKE_CURRENT_BINARY_DIR}/leaks-wrapper")

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
                               ${CMAKE_SOURCE_DIR})
  if(WITH_BLOCKS_RUNTIME)
    target_include_directories(${name}
                               SYSTEM BEFORE PRIVATE
                                 "${WITH_BLOCKS_RUNTIME}")
  endif()
  if(WITH_PTHREAD_WORKQUEUES)
    target_include_directories(${name}
                               SYSTEM BEFORE PRIVATE
                                 "${WITH_PTHREAD_WORKQUEUES}/include")
  endif()
  if(BSD_OVERLAY_FOUND)
    target_compile_options(${name}
                           PRIVATE
                             ${BSD_OVERLAY_CFLAGS})
  endif()
  target_compile_options(${name} PRIVATE -fblocks)
  # TODO(compnerd) make this portable
  target_compile_options(${name} PRIVATE -Wall -Wno-deprecated-declarations)
  target_link_libraries(${name} PRIVATE dispatch Threads::Threads)
  if(WITH_PTHREAD_WORKQUEUES)
    target_link_libraries(${name} PRIVATE PTHREAD::workqueue)
  endif()
  if(WITH_BLOCKS_RUNTIME)
    target_link_libraries(${name} PRIVATE BlocksRuntime)
  endif()
  if(BSD_OVERLAY_FOUND)
    target_link_libraries(${name}
                          PRIVATE
                            ${BSD_OVERLAY_LDFLAGS})
  endif()
  target_link_libraries(${name} PRIVATE bsdtests)
  add_test(NAME ${name}
           COMMAND bsdtestharness $<TARGET_FILE:${name}>)
  set_tests_properties(${name}
                       PROPERTIES
                         TIMEOUT 600
                         DEPENDS bsdtestharness
                         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  if(NOT leaks_EXECUTABLE)
    set_tests_properties(${name}
                         PROPERTIES
                           ENVIRONMENT NOLEAKS=1)
  endif()
endfunction()

set(DISPATCH_C_TESTS
    apply
    api
    c99
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
set(DISPATCH_CPP_TESTS
    plusplus)

foreach(test ${DISPATCH_C_TESTS})
  add_unit_test(dispatch_${test}
                SOURCES
                  dispatch_${test}.c)
endforeach()
foreach(test ${DISPATCH_CPP_TESTS})
  add_unit_test(dispatch_${test}
                SOURCES
                  dispatch_${test}.cpp)
endforeach()
target_link_libraries(dispatch_group PRIVATE m)
target_link_libraries(dispatch_timer_short PRIVATE m)

