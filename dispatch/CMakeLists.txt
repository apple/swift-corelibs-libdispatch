
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
          ${CMAKE_INSTALL_FULL_INCLUDEDIR}/dispatch)
if(ENABLE_SWIFT)
  get_filename_component(MODULE_MAP module.modulemap REALPATH)
  install(FILES
            ${MODULE_MAP}
          DESTINATION
            ${CMAKE_INSTALL_FULL_INCLUDEDIR}/dispatch)
endif()

