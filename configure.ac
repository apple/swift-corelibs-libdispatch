#
# When this file changes, rerun autogen.sh.
#

AC_PREREQ(2.59)
AC_INIT([libdispatch], [1.1], [libdispatch@macosforge.org], [libdispatch])
AC_REVISION([$$])
AC_CONFIG_AUX_DIR(config)
AC_CONFIG_HEADER([config/config.h])
AC_CONFIG_MACRO_DIR([m4])
ac_clean_files=a.out.dSYM
AM_MAINTAINER_MODE

AC_PROG_CC([clang gcc cc])
AC_PROG_CXX([clang++ g++ c++])
AC_PROG_OBJC([clang gcc cc])

#
# On Mac OS X, some required header files come from other source packages;
# allow specifying where those are.
#
AC_ARG_WITH([apple-libc-source],
  [AS_HELP_STRING([--with-apple-libc-source],
    [Specify path to Apple Libc source])], [
  apple_libc_source_pthreads_path=${withval}/pthreads
  CPPFLAGS="$CPPFLAGS -I$apple_libc_source_pthreads_path"
])

AC_ARG_WITH([apple-libclosure-source],
  [AS_HELP_STRING([--with-apple-libclosure-source],
    [Specify path to Apple libclosure source])], [
  apple_libclosure_source_path=${withval}
  CPPFLAGS="$CPPFLAGS -I$apple_libclosure_source_path"
])

AC_ARG_WITH([apple-xnu-source],
  [AS_HELP_STRING([--with-apple-xnu-source],
    [Specify path to Apple XNU source])], [
  apple_xnu_source_libkern_path=${withval}/libkern
  apple_xnu_source_osfmk_path=${withval}/osfmk
  CPPFLAGS="$CPPFLAGS -I$apple_xnu_source_libkern_path"
])

AC_CACHE_CHECK([for System.framework/PrivateHeaders], dispatch_cv_system_privateheaders,
  [AS_IF([test -d /System/Library/Frameworks/System.framework/PrivateHeaders],
    [dispatch_cv_system_privateheaders=yes], [dispatch_cv_system_privateheaders=no])]
)
AS_IF([test "x$dispatch_cv_system_privateheaders" != "xno"],
  [CPPFLAGS="$CPPFLAGS -I/System/Library/Frameworks/System.framework/PrivateHeaders"]
)

#
# On Mac OS X, libdispatch_init is automatically invoked during libSystem
# process initialization.  On other systems, it is tagged as a library
# constructor to be run by automatically by the runtime linker.
#
AC_ARG_ENABLE([libdispatch-init-constructor],
  [AS_HELP_STRING([--disable-libdispatch-init-constructor],
    [Disable libdispatch_init as a constructor])],,
  [AS_IF([test -f /usr/lib/system/libdispatch.dylib],
    [enable_libdispatch_init_constructor=no])]
)
AS_IF([test "x$enable_libdispatch_init_constructor" != "xno"],
  [AC_DEFINE(USE_LIBDISPATCH_INIT_CONSTRUCTOR, 1,
    [Define to tag libdispatch_init as a constructor])]
)

#
# On Mac OS X libdispatch can use the non-portable direct pthread TSD functions
#
AC_ARG_ENABLE([apple-tsd-optimizations],
  [AS_HELP_STRING([--enable-apple-tsd-optimizations],
    [Use non-portable pthread TSD optimizations for Mac OS X.])]
)
AS_IF([test "x$enable_apple_tsd_optimizations" = "xyes"],
  [AC_DEFINE(USE_APPLE_TSD_OPTIMIZATIONS, 1,
    [Define to use non-portable pthread TSD optimizations for Mac OS X)])]
)

AC_USE_SYSTEM_EXTENSIONS
AC_PROG_INSTALL
AC_PROG_LIBTOOL
AC_PATH_PROGS(MIG, mig)

AC_PATH_PROG(LEAKS, leaks)
AS_IF([test "x$LEAKS" != "x"],
  [AC_DEFINE(HAVE_LEAKS, 1, [Define if Apple leaks program is present])]
)

AM_INIT_AUTOMAKE([foreign])

DISPATCH_C_ATOMIC_BUILTINS

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
AC_SEARCH_LIBS(pthread_create, pthread)

#
# Prefer native kqueue(2); otherwise use libkqueue if present.
#
AC_CHECK_HEADER(sys/event.h, [],
  [PKG_CHECK_MODULES(KQUEUE, libkqueue)]
)

#
# Checks for header files.
#
AC_HEADER_STDC
AC_CHECK_HEADERS([TargetConditionals.h pthread_np.h malloc/malloc.h libkern/OSCrossEndian.h libkern/OSAtomic.h])

# hack for pthread_machdep.h's #include <System/machine/cpu_capabilities.h>
AS_IF([test -n "$apple_xnu_source_osfmk_path"], [
  saveCPPFLAGS="$CPPFLAGS"
  CPPFLAGS="$CPPFLAGS -I."
  ln -fsh "$apple_xnu_source_osfmk_path" System
])
AC_CHECK_HEADERS([pthread_machdep.h])
AS_IF([test -n "$apple_xnu_source_osfmk_path"], [
  rm -f System
  CPPFLAGS="$saveCPPFLAGS"
  AC_CONFIG_COMMANDS([src/System],
    [ln -fsh "$apple_xnu_source_osfmk_path" src/System],
    [apple_xnu_source_osfmk_path="$apple_xnu_source_osfmk_path"])
])

#
# Parts of the testsuite use CoreFoundation and Foundation
#
AC_CHECK_HEADER([CoreFoundation/CoreFoundation.h],
  [have_corefoundation=true], [have_corefoundation=false]
)
AM_CONDITIONAL(HAVE_COREFOUNDATION, $have_corefoundation)
AC_LANG_PUSH([Objective C])
AC_CHECK_HEADER([Foundation/Foundation.h], [
    AC_DEFINE(USE_OBJC, 1, [Define to use Objective-C runtime])
    have_foundation=true], [have_foundation=false]
)
AM_CONDITIONAL(HAVE_FOUNDATION, $have_foundation)
AC_LANG_POP([Objective C])

#
# We use the availability of mach.h to decide whether to compile in all sorts
# of Machisms, including using Mach ports as event sources, etc.
#
AC_CHECK_HEADER([mach/mach.h], [
    AC_DEFINE(HAVE_MACH, 1, [Define if mach is present])
    AC_DEFINE(__DARWIN_NON_CANCELABLE, 1, [Define if using Darwin $NOCANCEL])
    have_mach=true], [have_mach=false]
)
AM_CONDITIONAL(USE_MIG, $have_mach)

#
# We use the availability of pthread_workqueue.h to decide whether to compile
# in support for pthread work queues.
#
AC_CHECK_HEADER([pthread_workqueue.h],
  [AC_DEFINE(HAVE_PTHREAD_WORKQUEUES, 1, [Define if pthread work queues are present])]
)
AC_CHECK_FUNCS([pthread_workqueue_setdispatch_np])

#
# Find functions and declarations we care about.
#
AC_CHECK_DECLS([CLOCK_UPTIME, CLOCK_MONOTONIC], [], [],
  [[#include <time.h>]])
AC_CHECK_DECLS([NOTE_NONE, NOTE_REAP, NOTE_SIGNAL], [], [],
  [[#include <sys/event.h>]])
AC_CHECK_DECLS([FD_COPY], [], [], [[#include <sys/select.h>]])
AC_CHECK_DECLS([SIGEMT], [], [], [[#include <signal.h>]])
AC_CHECK_DECLS([VQ_UPDATE, VQ_VERYLOWDISK], [], [], [[#include <sys/mount.h>]])
AC_CHECK_DECLS([program_invocation_short_name], [], [], [[#include <errno.h>]])
AC_CHECK_FUNCS([pthread_key_init_np pthread_main_np mach_absolute_time malloc_create_zone sysconf getprogname])

AC_CHECK_DECLS([POSIX_SPAWN_START_SUSPENDED],
  [have_posix_spawn_start_suspended=true], [have_posix_spawn_start_suspended=false],
  [[#include <sys/spawn.h>]]
)
AM_CONDITIONAL(HAVE_POSIX_SPAWN_START_SUSPENDED, $have_posix_spawn_start_suspended)

AC_CHECK_FUNC([sem_init],
  [have_sem_init=true], [have_sem_init=false]
)

#
# We support both Mach semaphores and POSIX semaphores; if the former are
# available, prefer them.
#
AC_MSG_CHECKING([what semaphore type to use]);
AS_IF([test "x$have_mach" = "xtrue"],
  [AC_DEFINE(USE_MACH_SEM, 1, [Define to use Mach semaphores])
    AC_MSG_RESULT([Mach semaphores])],
  [test "x$have_sem_init" = "xtrue"],
  [AC_DEFINE(USE_POSIX_SEM, 1, [Define to use POSIX semaphores])
    AC_MSG_RESULT([POSIX semaphores])],
  [AC_MSG_ERROR([no supported semaphore type])]
)

AC_CHECK_HEADERS([sys/cdefs.h], [], [],
  [#ifdef HAVE_SYS_CDEFS_H
   #include <sys/cdefs.h>
   #endif])

DISPATCH_C_BLOCKS

AC_CACHE_CHECK([for -fvisibility=hidden], [dispatch_cv_cc_visibility_hidden], [
  saveCFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS -fvisibility=hidden"
  AC_LINK_IFELSE([AC_LANG_PROGRAM([
    extern __attribute__ ((visibility ("default"))) int foo; int foo;], [foo = 0;])],
    [dispatch_cv_cc_visibility_hidden="yes"], [dispatch_cv_cc_visibility_hidden="no"])
  CFLAGS="$saveCFLAGS"
])
AS_IF([test "x$dispatch_cv_cc_visibility_hidden" != "xno"], [
    VISIBILITY_FLAGS="-fvisibility=hidden"
])
AC_SUBST([VISIBILITY_FLAGS])

AC_CACHE_CHECK([for -momit-leaf-frame-pointer], [dispatch_cv_cc_omit_leaf_fp], [
  saveCFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS -momit-leaf-frame-pointer"
  AC_LINK_IFELSE([AC_LANG_PROGRAM([
    extern int foo(void); int foo(void) {return 1;}], [foo();])],
    [dispatch_cv_cc_omit_leaf_fp="yes"], [dispatch_cv_cc_omit_leaf_fp="no"])
  CFLAGS="$saveCFLAGS"
])
AS_IF([test "x$dispatch_cv_cc_omit_leaf_fp" != "xno"], [
  OMIT_LEAF_FP_FLAGS="-momit-leaf-frame-pointer"
])
AC_SUBST([OMIT_LEAF_FP_FLAGS])

AC_CACHE_CHECK([for darwin linker], [dispatch_cv_ld_darwin], [
  saveLDFLAGS="$LDFLAGS"
  LDFLAGS="$LDFLAGS -dynamiclib -compatibility_version 1.2.3 -current_version 4.5.6"
  AC_LINK_IFELSE([AC_LANG_PROGRAM([
    extern int foo; int foo;], [foo = 0;])],
    [dispatch_cv_ld_darwin="yes"], [dispatch_cv_ld_darwin="no"])
  LDFLAGS="$saveLDFLAGS"
])
AM_CONDITIONAL(HAVE_DARWIN_LD, [test "x$dispatch_cv_ld_darwin" != "xno"])

#
# Temporary: some versions of clang do not mark __builtin_trap() as
# __attribute__((__noreturn__)).  Detect and add if required.
#
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([void __attribute__((__noreturn__)) temp(void) { __builtin_trap(); }], [])],
  [AC_DEFINE(HAVE_NORETURN_BUILTIN_TRAP, 1, [Define if __builtin_trap marked noreturn])]
)

#
# Generate Makefiles.
#
AC_CONFIG_FILES([Makefile dispatch/Makefile man/Makefile private/Makefile src/Makefile])
AC_OUTPUT
