
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
          ${CMAKE_INSTALL_FULL_INCLUDEDIR}/dispatch/)
if(ENABLE_SWIFT)
  install(FILES
            module.modulemap
          DESTINATION
            ${CMAKE_INSTALL_FULL_INCLUEDIR}/dispatch/)
endif()

