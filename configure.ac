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
# Try to build the legacy API only if specifically requested.
#
AC_ARG_WITH([legacy-api],
  [AS_HELP_STRING([--with-legacy-api],
    [Support historic (deprecated) Apple API in libdispatch.])
  ],[],[
    use_legacy_api=false
    AC_DEFINE(DISPATCH_NO_LEGACY,,Define to compile out legacy API)
  ])
AM_CONDITIONAL(USE_LEGACY_API, $use_legacy_api)

#
# On Mac OS X Snow Leopard, libpispatch_init is automatically invoked during
# libsyscall process setup.  On other systems, it is tagged as a library
# constructor to be run by automatically by the runtime linker.
#
AC_ARG_WITH([libdispatch-init-constructor],
  [AS_HELP_STRING([--without-libdispatch-init-constructor],
    [Don't tag libdispatch_init as a constructor])
  ],[],[
    AC_DEFINE(USE_LIBDISPATCH_INIT_CONSTRUCTOR,,Define to tag libdispatch_init as a constructor)
  ])

#
# libdispatch has micro-optimized and deeply personal knowledge of the Mac OS
# implementation details.  Only enable this if explicitly requested, as it
# will lead to data corruption if applied on systems violating its
# expectations.
#
AC_ARG_WITH([apple-tsd-optimizations],
  [AS_HELP_STRING([--with-apple-tsd-optimizations],
    [Use less portable pthread TSD optimizations for Mac OS X.])
  ],[
    AC_DEFINE(USE_APPLE_TSD_OPTIMIZATIONS,,Define to compile in tsd optimizations)
  ],[])

AC_ARG_WITH([apple-semaphore-optimizations],
  [AS_HELP_STRING([--with-apple-semaphore-optimizations],
    [Use less portable semaphore optimizations for Mac OS X.])
  ],[
    AC_DEFINE(USE_APPLE_SEMAPHORE_OPTIMIZATIONS,,Define to compile in semaphore optimizations)
  ],[])

AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LIBTOOL

AM_INIT_AUTOMAKE(AC_PACKAGE_NAME, AC_PACKAGE_VERSION)

#
# Find libraries we will need
#
AC_SEARCH_LIBS(clock_gettime, rt)
AC_SEARCH_LIBS(pthread_create, pthread, [
    use_libpthread=true
  ],[
    use_libpthread=false
  ]
)
AM_CONDITIONAL(USE_LIBPTHREAD, $use_libpthread)

#
# Checks for header files.
#
AC_HEADER_STDC
AC_CHECK_HEADERS([Availability.h pthread_np.h malloc/malloc.h])

#
# We use the availability of mach.h to decide whether to compile in all sorts
# of Machisms, including using Mach ports as event sources, etc.
#
AC_CHECK_HEADER([mach/mach.h],[
  AC_DEFINE(HAVE_MACH,,Define if mach is present)
])

#
# Find functions and declarations we care about.
#
AC_CHECK_DECLS([EVFILT_SESSION, NOTE_NONE, NOTE_REAP, NOTE_SIGNAL], [], [],
  [[#include <sys/event.h>]])
AC_CHECK_DECLS([VQ_UPDATE, VQ_VERYLOWDISK], [], [], [[#include <sys/mount.h>]])
AC_CHECK_FUNCS([pthread_key_init_np mach_absolute_time pthread_workqueue_attr_init_np malloc_create_zone sem_init])

#
# Generate Makefiles.
#
AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
