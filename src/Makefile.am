#
#
#

lib_LTLIBRARIES=libdispatch.la

libdispatch_la_SOURCES=		\
	apply.c					\
	benchmark.c				\
	data.c					\
	init.c					\
	io.c					\
	object.c				\
	once.c					\
	queue.c					\
	semaphore.c				\
	source.c				\
	time.c					\
	protocol.defs			\
	provider.d				\
	data_internal.h			\
	internal.h				\
	io_internal.h			\
	object_internal.h		\
	queue_internal.h		\
	semaphore_internal.h	\
	shims.h					\
	source_internal.h		\
	trace.h					\
	shims/atomic.h			\
	shims/getprogname.h		\
	shims/hw_config.h		\
	shims/malloc_zone.h		\
	shims/perfmon.h			\
	shims/time.h			\
	shims/tsd.h

INCLUDES=-I$(top_builddir) -I$(top_srcdir) -I$(top_srcdir)/private \
	@APPLE_LIBC_SOURCE_PATH@ @APPLE_LIBCLOSURE_SOURCE_PATH@ @APPLE_XNU_SOURCE_PATH@

libdispatch_la_CFLAGS=-Wall $(VISIBILITY_FLAGS) $(OMIT_LEAF_FP_FLAGS)
libdispatch_la_CFLAGS+=$(MARCH_FLAGS) $(CBLOCKS_FLAGS) $(KQUEUE_CFLAGS)

libdispatch_la_LDFLAGS=-avoid-version

if HAVE_DARWIN_LD
libdispatch_la_LDFLAGS+=-Wl,-compatibility_version,1 -Wl,-current_version,$(VERSION)
endif

CLEANFILES=

if USE_MIG
BUILT_SOURCES=				\
	protocolUser.c			\
	protocol.h				\
	protocolServer.c		\
	protocolServer.h

nodist_libdispatch_la_SOURCES=$(BUILT_SOURCES)
CLEANFILES+=$(BUILT_SOURCES)

%User.c %.h %Server.c %Server.h: $(abs_srcdir)/%.defs
	$(MIG) -user $*User.c -header $*.h \
	    -server $*Server.c -sheader $*Server.h $<
endif

if USE_XNU_SOURCE
# hack for pthread_machdep.h's #include <System/machine/cpu_capabilities.h>
$(libdispatch_la_OBJECTS): $(abs_srcdir)/System
$(abs_srcdir)/System:
	$(LN_S) -fh "@APPLE_XNU_SOURCE_SYSTEM_PATH@" System
CLEANFILES+=System
endif
