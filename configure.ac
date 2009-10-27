#
# When this file changes, rerun autogen.sh.
#

AC_PREREQ(2.59)
AC_INIT([libdispatch], [1.0], [libdispatch@macosforge.org], [libdispatch])
AC_REVISION([$$])
AC_CONFIG_AUX_DIR(config)
AC_CONFIG_HEADER([config/config.h])
AM_MAINTAINER_MODE

#
# On Mac OS X, some required header files come from other source packages;
# allow specifying where those are.
#
AC_ARG_WITH([apple-libc-source],
  [AS_HELP_STRING([--with-apple-libc-source],
    [Specify path to Apple Libc source])],
  [apple_libc_source_path=${withval}/pthreads
    APPLE_LIBC_SOURCE_PATH=-I$apple_libc_source_path
    CPPFLAGS="$CPPFLAGS -I$apple_libc_source_path"],
  [APPLE_LIBC_SOURCE_PATH=]
)
AC_SUBST([APPLE_LIBC_SOURCE_PATH])

AC_ARG_WITH([apple-xnu-source],
  [AS_HELP_STRING([--with-apple-xnu-source],
    [Specify path to Apple XNU source])],
  [apple_xnu_source_path=${withval}/libkern
    APPLE_XNU_SOURCE_PATH=-I$apple_xnu_source_path
    CPPFLAGS="$CPPFLAGS -I$apple_xnu_source_path"],
  [APPLE_XNU_SOURCE_PATH=]
)
AC_SUBST([APPLE_XNU_SOURCE_PATH])

#
# Try to build the legacy API only if specifically requested.
#
AC_ARG_ENABLE([legacy-api],
  [AS_HELP_STRING([--enable-legacy-api], [Enable legacy (deprecated) API.])]
)

AS_IF([test "x$enable_legacy_api" != "xyes"],
  [use_legacy_api=false
    AC_DEFINE(DISPATCH_NO_LEGACY,,[Define to compile out legacy API])],
  [use_legacy_api=true]
)
AM_CONDITIONAL(USE_LEGACY_API, $use_legacy_api)

#
# On Mac OS X Snow Leopard, libpispatch_init is automatically invoked during
# libsyscall process setup.  On other systems, it is tagged as a library
# constructor to be run by automatically by the runtime linker.
#
AC_ARG_ENABLE([libdispatch-init-constructor],
  [AS_HELP_STRING([--disable-libdispatch-init-constructor],
    [Disable libdispatch_init as a constructor])]
)

AS_IF([test "x$enable_libdispatch_init_constructor" != "xno"],
  [AC_DEFINE(USE_LIBDISPATCH_INIT_CONSTRUCTOR,,
    [Define to tag libdispatch_init as a constructor])]
)

#
# Whether or not to include/reference a crashreporter symbol.
#
AC_ARG_ENABLE([apple-crashreporter-info],
  [AS_HELP_STRING([--enable-apple-crashreporter-info],
    [Use Mac OS X crashreporter info])]
)

AS_IF([test "x$enable_apple_crashreporter_info" = "xyes"],
  [AC_DEFINE(USE_APPLE_CRASHREPORTER_INFO,,
    [Define to use Mac OS X crashreporter info])]
)

#
# libdispatch has micro-optimized and deeply personal knowledge of Mac OS
# implementation details.  Only enable this if explicitly requested, as it
# will lead to data corruption if applied on systems violating its
# expectations.
#
AC_ARG_ENABLE([apple-tsd-optimizations],
  [AS_HELP_STRING([--enable-apple-tsd-optimizations],
    [Use non-portable pthread TSD optimizations for Mac OS X.])]
)

AS_IF([test "x$enable_apple_tsd_optimizations" = "xyes"],
  [AC_DEFINE(USE_APPLE_TSD_OPTIMIZATIONS,,
    [Define to use non-portable pthread TSD optimizations for Mac OS X)])]
)

AC_ARG_ENABLE([apple-semaphore-optimizations],
  [AS_HELP_STRING([--enable-apple-semaphore-optimizations],
    [Use non-portable semaphore optimizations for Mac OS X.])]
)

AS_IF([test "x$enable_apple_semaphore_optimizations" = "xyes"],
  [AC_DEFINE(USE_APPLE_SEMAPHORE_OPTIMIZATIONS,,
    [Define to use non-portablesemaphore optimizations for Mac OS X])]
)

AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LIBTOOL
AC_PATH_PROGS(MIG, mig)

AM_INIT_AUTOMAKE([foreign])

#
# This is a bit subtle: on i386 systems without at least -march=i486 defined,
# certain built-in atomics fall back to depending on undefined symbols if
# their return values are used.
#
AC_CACHE_CHECK([for gcc atomic builtins],[dispatch_cv_atomic],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([],[[
int i, x =0;
i = __sync_add_and_fetch(&x,1);
return x;]])],[dispatch_cv_atomic=yes],
  [saveCFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS -march=i486"
  AC_LINK_IFELSE([AC_LANG_PROGRAM([],[[
  int i, x =0;
  i = __sync_add_and_fetch(&x,1);
  return x;]])],[CFLAGS="$saveCFLAGS"
dispatch_cv_atomic="-march=i486"
])])])

case $dispatch_cv_atomic in
  yes) ;;
  -march*) MARCH_FLAGS="$dispatch_cv_atomic"
         AC_SUBST([MARCH_FLAGS]) ;;
  *) AC_MSG_ERROR([No gcc builtin atomic operations available]) ;;
esac

#
# Find libraries we will need
#
AC_SEARCH_LIBS(clock_gettime, rt)
AC_SEARCH_LIBS(pthread_create, pthread,
  [use_libpthread=true],
  [use_libpthread=false]
)
AM_CONDITIONAL(USE_LIBPTHREAD, $use_libpthread)

#
# Checks for header files.
#
AC_HEADER_STDC
AC_CHECK_HEADERS([Availability.h pthread_machdep.h pthread_np.h malloc/malloc.h libkern/OSCrossEndian.h libkern/OSAtomic.h])

#
# We use the availability of mach.h to decide whether to compile in all sorts
# of Machisms, including using Mach ports as event sources, etc.
#
AC_CHECK_HEADER([mach/mach.h],
  [AC_DEFINE(HAVE_MACH,,Define if mach is present)
    use_mig=true],
  [use_mig=false]
)
AM_CONDITIONAL(USE_MIG, $use_mig)

#
# We use the availability of pthread_workqueue.h to decide whether to compile
# in support for pthread work queues.
#
AC_CHECK_HEADER([pthread_workqueue.h],
  [AC_DEFINE(HAVE_PTHREAD_WORKQUEUES,,Define if pthread work queues are present)]
)

#
# Find functions and declarations we care about.
#
AC_CHECK_DECLS([CLOCK_UPTIME, CLOCK_MONOTONIC], [], [],
  [[#include <time.h>]])
AC_CHECK_DECLS([EVFILT_SESSION, NOTE_NONE, NOTE_REAP, NOTE_SIGNAL], [], [],
  [[#include <sys/event.h>]])
AC_CHECK_DECLS([FD_COPY], [], [], [[#include <sys/select.h>]])
AC_CHECK_DECLS([SIGEMT], [], [], [[#include <signal.h>]])
AC_CHECK_DECLS([VQ_UPDATE, VQ_VERYLOWDISK], [], [], [[#include <sys/mount.h>]])
AC_CHECK_FUNCS([pthread_key_init_np pthread_main_np mach_absolute_time malloc_create_zone sem_init])

#
# Does the platform support __private_extern__?  There is surely a better way
# to check this.
#
AC_TRY_LINK([], [
	__private_extern__ int foo;
], [
  AC_DEFINE(HAVE_PRIVATE_EXTERN,, Define if __private_extern__ present)
])

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
], [
    CBLOCKS_FLAGS=""
])
AC_SUBST([CBLOCKS_FLAGS])

#
# Temporary: some versions of clang do not mark __builtin_trap() as
# __attribute__((__noreturn__)).  Detect and add if required.
#
AC_COMPILE_IFELSE([
  AC_LANG_PROGRAM([void __attribute__((__noreturn__)) temp(void) { __builtin_trap(); }], [])], [
    AC_DEFINE(HAVE_NORETURN_BUILTIN_TRAP,,[Define if __builtin_trap marked noreturn])
  ], [])

#
# Generate Makefiles.
#
AC_CONFIG_FILES([Makefile dispatch/Makefile man/Makefile src/Makefile])
AC_OUTPUT
