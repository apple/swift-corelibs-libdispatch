
cmake_minimum_required(VERSION 3.4.3)
project(BlocksRuntime
        LANGUAGES C)

include(CheckIncludeFiles)
include(GNUInstallDirs)

if(CMAKE_C_SIMULATE_ID STREQUAL MSVC)
  # clang-cl interprets paths starting with /U as macro undefines, so we need to
  # put a -- before the input file path to force it to be treated as a path.
  string(REPLACE "-c <SOURCE>" "-c -- <SOURCE>" CMAKE_C_COMPILE_OBJECT "${CMAKE_C_COMPILE_OBJECT}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED YES)

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_INLINES_HIDDEN YES)

check_include_files("objc/objc-internal.h" HAVE_OBJC)

option(INSTALL_PRIVATE_HEADERS "install private headers in the same location as the public ones" NO)

add_library(BlocksRuntime
            ${PROJECT_SOURCE_DIR}/data.c
            ${PROJECT_SOURCE_DIR}/runtime.c)
set_target_properties(BlocksRuntime
                      PROPERTIES
                        POSITION_INDEPENDENT_CODE TRUE)
if(HAVE_OBJC AND CMAKE_DL_LIBS)
  target_link_libraries(BlocksRuntime
                        PUBLIC
                          ${CMAKE_DL_LIBS})
endif()

install(FILES
          ${PROJECT_SOURCE_DIR}/Block.h
        DESTINATION
          ${CMAKE_INSTALL_INCLUDEDIR})
if(INSTALL_PRIVATE_HEADERS)
  install(FILES
            ${PROJECT_SOURCE_DIR}/Block_private.h
          DESTINATION
            ${CMAKE_INSTALL_INCLUDEDIR})
endif()
install(TARGETS
          BlocksRuntime
        DESTINATION
          ${CMAKE_INSTALL_LIBDIR})

