
add_library(BlocksRuntime
  data.c
  runtime.c)

if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  target_sources(BlocksRuntime PRIVATE
    BlocksRuntime.def)

  # NOTE(compnerd) use the `BUILD_SHARED_LIBS` variable to determine what type
  # of library to build.  If it is true, we will generate shared libraries,
  # otherwise we will generate static libraries.
  if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(BlocksRuntime PRIVATE
      BlocksRuntime_STATIC)
  endif()
endif()

set_target_properties(BlocksRuntime PROPERTIES
  POSITION_INDEPENDENT_CODE TRUE
  INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR})
if(HAVE_OBJC AND CMAKE_DL_LIBS)
  target_link_libraries(BlocksRuntime PUBLIC
    ${CMAKE_DL_LIBS})
endif()

install(FILES Block.h
  DESTINATION ${INSTALL_BLOCK_HEADERS_DIR})
if(INSTALL_PRIVATE_HEADERS)
  install(FILES Block_private.h
    DESTINATION ${INSTALL_BLOCK_HEADERS_DIR})
endif()
install(TARGETS BlocksRuntime
  ARCHIVE DESTINATION ${INSTALL_TARGET_DIR}
  LIBRARY DESTINATION ${INSTALL_TARGET_DIR}
  RUNTIME DESTINATION bin)

