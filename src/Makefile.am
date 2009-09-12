#
#
#

lib_LTLIBRARIES=libdispatch.la

libdispatch_la_SOURCES=	\
	apply.c		\
	benchmark.c	\
	object.c	\
	once.c		\
	queue.c		\
	semaphore.c	\
	shims.c		\
	source.c	\
	time.c

libdispatch_la_CFLAGS=-Wall -Werror

#
# This will need some refinement: gcc requires a minimum of -march=i486 on
# x86 in order to implement built-in atomic operations.  But when should we
# define this optional argument?  We need appropriate configure parts to make
# this conditional.
#
libdispatch_la_CFLAGS+=-march=i686

if USE_LEGACY_API
libdispatch_la_SOURCES+=	\
	legacy.c
endif

if USE_LIBPTHREAD
libdispatch_la_LIBADD=-lpthread
endif

#
# This hack is needed because the default include line from automake will add
# -I. to compiler commands.  That leads to the local semaphore.h overriding
# the one in /usr/local on systems using POSIX semaphores.  We can remove
# this hack once header files are moved to their own directory.
#
DEFAULT_INCLUDES = @am__isrc@ -I.. -I$(top_builddir)/config
