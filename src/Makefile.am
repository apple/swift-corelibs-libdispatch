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
INCLUDES=-I$(top_builddir) -I$(top_srcdir) \
	@APPLE_LIBC_SOURCE_PATH@ @APPLE_XNU_SOURCE_PATH@

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

if USE_MIG
libdispatch_la_SOURCES+=	\
	protocolServer.c
BUILT_SOURCES=			\
	protocol.h		\
	protocolServer.c	\
	protocolServer.h
CLEANFILES=$BUILT_SOURCES

protocol.h protocolServer.h protocolServer.c: protocol.defs
	$(MIG) -user /dev/null -header protocol.h -server protocolServer.c \
	    -sheader protocolServer.h protocol.defs
endif
