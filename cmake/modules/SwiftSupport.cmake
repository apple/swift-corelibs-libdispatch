
include(CMakeParseArguments)

function(add_swift_library library)
  set(options)
  set(single_value_options MODULE_NAME;MODULE_LINK_NAME;MODULE_PATH;MODULE_CACHE_PATH;OUTPUT;TARGET)
  set(multiple_value_options SOURCES;SWIFT_FLAGS;CFLAGS;DEPENDS)

  cmake_parse_arguments(ASL "${options}" "${single_value_options}" "${multiple_value_options}" ${ARGN})

  cmake_parse_arguments(AST "${options}" "${single_value_options}" "${multiple_value_options}" ${ARGN})

  set(compile_flags ${CMAKE_SWIFT_FLAGS})
  set(link_flags)

  if(ASL_TARGET)
    list(APPEND FLAGS -target;${ASL_TARGET})
  endif()
  if(ASL_MODULE_NAME)
    list(APPEND flags -module-name;${ASL_MODULE_NAME})
  endif()
  if(AST_MODULE_LINK_NAME)
    list(APPEND compile_flags -module-link-name;${AST_MODULE_LINK_NAME})
  endif()
  if(AST_MODULE_CACHE_PATH)
    list(APPEND compile_flags -module-cache-path;${AST_MODULE_CACHE_PATH})
  endif()
  if(CMAKE_BUILD_TYPE MATCHES Debug OR CMAKE_BUILD_TYPE MATCHES RelWithDebInfo)
    list(APPEND compile_flags -g)
  endif()
  if(AST_SWIFT_FLAGS)
    foreach(flag ${AST_SWIFT_FLAGS})
      list(APPEND compile_flags ${flag})
    endforeach()
  endif()
  if(AST_CFLAGS)
    foreach(flag ${AST_CFLAGS})
      list(APPEND compile_flags -Xcc;${flag})
    endforeach()
  endif()
  if(AST_LINK_FLAGS)
    foreach(flag ${AST_LINK_FLAGS})
      list(APPEND link_flags ${flag})
    endforeach()
  endif()
  if(AST_LIBRARY)
    if(AST_STATIC AND AST_SHARED)
      message(SEND_ERROR "add_swift_target asked to create library as STATIC and SHARED")
    elseif(AST_STATIC OR NOT BUILD_SHARED_LIBS)
      set(library_kind STATIC)
    elseif(AST_SHARED OR BUILD_SHARED_LIBS)
      set(library_kind SHARED)
    endif()
  else()
    if(AST_STATIC OR AST_SHARED)
      message(SEND_ERROR "add_swift_target asked to create executable as STATIC or SHARED")
    endif()
  endif()
  if(NOT AST_OUTPUT)
    if(AST_LIBRARY)
      if(AST_SHARED OR BUILD_SHARED_LIBS)
        set(AST_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target}.dir/${CMAKE_SHARED_LIBRARY_PREFIX}${target}${CMAKE_SHARED_LIBRARY_SUFFIX})
      else()
        set(AST_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target}.dir/${CMAKE_STATIC_LIBRARY_PREFIX}${target}${CMAKE_STATIC_LIBRARY_SUFFIX})
      endif()
    else()
      set(AST_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target}.dir/${target}${CMAKE_EXECUTABLE_SUFFIX})
    endif()
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL Windows)
    if(AST_SHARED OR BUILD_SHARED_LIBS)
      set(IMPORT_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/${target}.dir/${CMAKE_IMPORT_LIBRARY_PREFIX}${target}${CMAKE_IMPORT_LIBRARY_SUFFIX})
    endif()
  endif()

  set(sources)
  foreach(source ${AST_SOURCES})
    get_filename_component(location ${source} PATH)
    if(IS_ABSOLUTE ${location})
      list(APPEND sources ${source})
    else()
      list(APPEND sources ${CMAKE_CURRENT_SOURCE_DIR}/${source})
    endif()
  endforeach()

  get_filename_component(module_directory ${ASL_MODULE_PATH} DIRECTORY)

  add_custom_command(OUTPUT
                       ${ASL_OUTPUT}
                       ${ASL_MODULE_PATH}
                       ${module_directory}/${ASL_MODULE_NAME}.swiftdoc
                     DEPENDS
                       ${ASL_SOURCES}
                       ${CMAKE_SWIFT_COMPILER}
                       ${ASL_DEPENDS}
                     COMMAND
                       ${CMAKE_COMMAND} -E make_directory ${module_directory}
                     COMMAND
                       ${CMAKE_SWIFT_COMPILER} ${flags} -c ${sources} -o ${ASL_OUTPUT})
  add_custom_target(${library}
                    DEPENDS
                       ${ASL_OUTPUT}
                       ${ASL_MODULE_PATH}
                       ${module_directory}/${ASL_MODULE_NAME}.swiftdoc)
endfunction()

# Returns the current achitecture name in a variable
#
# Usage:
#   get_swift_host_arch(result_var_name)
#
# If the current architecture is supported by Swift, sets ${result_var_name}
# with the sanitized host architecture name derived from CMAKE_SYSTEM_PROCESSOR.
function(get_swift_host_arch result_var_name)
  if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
    set("${result_var_name}" "x86_64" PARENT_SCOPE)
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64")
    set("${result_var_name}" "aarch64" PARENT_SCOPE)
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "ppc64")
    set("${result_var_name}" "powerpc64" PARENT_SCOPE)
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "ppc64le")
    set("${result_var_name}" "powerpc64le" PARENT_SCOPE)
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "s390x")
    set("${result_var_name}" "s390x" PARENT_SCOPE)
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv6l")
    set("${result_var_name}" "armv6" PARENT_SCOPE)
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7l")
    set("${result_var_name}" "armv7" PARENT_SCOPE)
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "AMD64")
    set("${result_var_name}" "x86_64" PARENT_SCOPE)
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "IA64")
    set("${result_var_name}" "itanium" PARENT_SCOPE)
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86")
    set("${result_var_name}" "i686" PARENT_SCOPE)
  else()
    message(FATAL_ERROR "Unrecognized architecture on host system: ${CMAKE_SYSTEM_PROCESSOR}")
  endif()
endfunction()
