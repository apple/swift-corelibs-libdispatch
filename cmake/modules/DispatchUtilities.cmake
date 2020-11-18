
function(dispatch_set_linker target)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL Windows)
    set(CMAKE_HOST_EXECUTABLE_SUFFIX .exe)
  endif()

  if(USE_GOLD_LINKER)
    set_property(TARGET ${target}
                 APPEND_STRING
                 PROPERTY LINK_FLAGS
                   -fuse-ld=gold${CMAKE_HOST_EXECUTABLE_SUFFIX})
  endif()
  if(USE_LLD_LINKER)
    set_property(TARGET ${target}
                 APPEND_STRING
                 PROPERTY LINK_FLAGS
                   -fuse-ld=lld${CMAKE_HOST_EXECUTABLE_SUFFIX})
  endif()
endfunction()
