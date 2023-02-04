
# TODO(compnerd) ensure that object_private.h voucher_activity_private.h
# voucher_private.h are included in the source tarball

install(FILES
          object.h
          generic_unix_base.h
          generic_win_base.h
        DESTINATION
          "${INSTALL_OS_HEADERS_DIR}")

