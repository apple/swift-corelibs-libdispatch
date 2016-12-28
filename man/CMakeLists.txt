
# TODO(compnerd) add symlinks
if(NOT ENABLE_SWIFT)
  install(FILES
            dispatch.3
            dispatch_after.3
            dispatch_api.3
            dispatch_apply.3
            dispatch_async.3
            dispatch_data_create.3
            dispatch_group_create.3
            dispatch_io_create.3
            dispatch_io_read.3
            dispatch_object.3
            dispatch_once.3
            dispatch_queue_create.3
            dispatch_read.3
            dispatch_semaphore_create.3
            dispatch_source_create.3
            dispatch_time.3
          DESTINATION
            "${CMAKE_INSTALL_FULL_MANDIR}/man3")
endif()
