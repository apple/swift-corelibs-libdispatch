AC_DEFUN([DISPATCH_C_BLOCKS], [
#
# Detect compiler support for Blocks; perhaps someday -fblocks won't be
# required, in which case we'll need to change this.
#
AC_CACHE_CHECK([for C Blocks support], [dispatch_cv_cblocks], [
  saveCFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS -fblocks"
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],[(void)^{int i; i = 0; }();])], [
    CFLAGS="$saveCFLAGS"
    dispatch_cv_cblocks="-fblocks"
  ], [
    CFLAGS="$saveCFLAGS"
    dispatch_cv_cblocks="no"
  ])
])

#
# Allow configure to be passed a path to the directory where it should look
# for the Blocks runtime library, if any.
#
AC_ARG_WITH([blocks-runtime],
  [AS_HELP_STRING([--with-blocks-runtime],
    [Specify path to the blocks runtime])],
  [blocks_runtime=${withval}
    LIBS="$LIBS -L$blocks_runtime"]
)

AS_IF([test "x$dispatch_cv_cblocks" != "xno"], [
    CBLOCKS_FLAGS="$dispatch_cv_cblocks"

    #
    # It may be necessary to directly link the Blocks runtime on some
    # systems, so give it a try if we can't link a C program that uses
    # Blocks.  We will want to remove this at somepoint, as really -fblocks
    # should force that linkage already.
    #
    saveCFLAGS="$CFLAGS"
    CFLAGS="$CFLAGS -fblocks"
    AC_MSG_CHECKING([whether additional libraries are required for the Blocks runtime])
    AC_TRY_LINK([], [
	^{ int j; j=0; }();
    ], [
	AC_MSG_RESULT([no]);
    ], [
      saveLIBS="$LIBS"
      LIBS="$LIBS -lBlocksRuntime"
      AC_TRY_LINK([], [
	^{ int k; k=0; }();
      ], [
	AC_MSG_RESULT([-lBlocksRuntime])
      ], [
	AC_MSG_ERROR([can't find Blocks runtime])
      ])
    ])
    CFLAGS="$saveCFLAGS"
    have_cblocks=true
], [
    CBLOCKS_FLAGS=""
    have_cblocks=false
])
AM_CONDITIONAL(HAVE_BLOCKS, $have_cblocks)
AC_SUBST([CBLOCKS_FLAGS])
])
