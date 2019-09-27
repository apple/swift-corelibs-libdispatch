
add_library(BlocksRuntime
  data.c
  runtime.c)
if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  target_sources(BlocksRuntime PRIVATE
    BlocksRuntime.def)

  if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(BlocksRuntime PRIVATE
      BlocksRuntime_STATIC)
  endif()
endif()

target_include_directories(BlocksRuntime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR})
if(HAVE_OBJC AND CMAKE_DL_LIBS)
  target_link_libraries(BlocksRuntime PUBLIC
    ${CMAKE_DL_LIBS})
endif()

set_target_properties(BlocksRuntime PROPERTIES
  POSITION_INDEPENDENT_CODE TRUE)

add_library(BlocksRuntime::BlocksRuntime ALIAS BlocksRuntime)

install(FILES Block.h
        DESTINATION ${INSTALL_BLOCK_HEADERS_DIR})
if(INSTALL_PRIVATE_HEADERS)
  install(FILES Block_private.h
          DESTINATION ${INSTALL_BLOCK_HEADERS_DIR})
endif()
set_property(GLOBAL APPEND PROPERTY DISPATCH_EXPORTS BlocksRuntime)
install(TARGETS BlocksRuntime
        EXPORT dispatchExports
        ARCHIVE DESTINATION ${INSTALL_TARGET_DIR}
        LIBRARY DESTINATION ${INSTALL_TARGET_DIR}
        RUNTIME DESTINATION bin)
