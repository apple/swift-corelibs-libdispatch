
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
  get_filename_component(MODULE_MAP module.modulemap REALPATH)
  install(FILES
            ${MODULE_MAP}
          DESTINATION
            "${INSTALL_DISPATCH_HEADERS_DIR}")
endif()

