
install(FILES
          base.h
          block.h
          data.h
          dispatch.h
          group.h
          introspection.h
          io.h
          object.h
          once.h
          queue.h
          semaphore.h
          source.h
          time.h
        DESTINATION
          "${INSTALL_DISPATCH_HEADERS_DIR}")
if(ENABLE_SWIFT)
  set(base_dir "${CMAKE_CURRENT_SOURCE_DIR}")
  if(NOT BUILD_SHARED_LIBS)
    set(base_dir "${CMAKE_CURRENT_SOURCE_DIR}/generic_static")
  endif()

  get_filename_component(
    MODULE_MAP
    module.modulemap
    REALPATH
    BASE_DIR "${base_dir}")

  install(FILES
            ${MODULE_MAP}
          DESTINATION
            "${INSTALL_DISPATCH_HEADERS_DIR}")
endif()

