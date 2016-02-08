#
#
#

lib_LTLIBRARIES=libdispatch.la

libdispatch_la_SOURCES=		\
	allocator.c				\
	apply.c					\
	benchmark.c				\
	data.c					\
	introspection.c			\
	init.c					\
	io.c					\
	object.c				\
	once.c					\
	queue.c					\
	semaphore.c				\
	source.c				\
	time.c					\
	transform.c				\
	voucher.c				\
	protocol.defs			\
	provider.d				\
	allocator_internal.h	\
	data_internal.h			\
	inline_internal.h		\
	internal.h				\
	introspection_internal.h	\
	io_internal.h			\
	object_internal.h		\
	queue_internal.h		\
	semaphore_internal.h	\
	shims.h					\
	source_internal.h		\
	trace.h					\
	voucher_internal.h		\
	shims/atomic.h			\
	shims/atomic_sfb.h		\
	shims/getprogname.h		\
	shims/hw_config.h		\
	shims/linux_stubs.c		\
	shims/linux_stubs.h		\
	shims/perfmon.h			\
	shims/time.h			\
	shims/tsd.h				\
	shims/pthread_main_np.h		\
	shims/yield.h

AM_CPPFLAGS=-I$(top_builddir) -I$(top_srcdir) \
	-I$(top_srcdir)/private -I$(top_srcdir)/os

DISPATCH_CFLAGS=-Wall $(VISIBILITY_FLAGS) $(OMIT_LEAF_FP_FLAGS) \
	$(MARCH_FLAGS) $(KQUEUE_CFLAGS) $(BSD_OVERLAY_CFLAGS)
AM_CFLAGS= $(PTHREAD_WORKQUEUE_CFLAGS) $(DISPATCH_CFLAGS) $(CBLOCKS_FLAGS)
AM_OBJCFLAGS=$(DISPATCH_CFLAGS) $(CBLOCKS_FLAGS)
AM_CXXFLAGS=$(DISPATCH_CFLAGS) $(CXXBLOCKS_FLAGS)
AM_OBJCXXFLAGS=$(DISPATCH_CFLAGS) $(CXXBLOCKS_FLAGS)

if BUILD_OWN_PTHREAD_WORKQUEUES
  PTHREAD_WORKQUEUE_LIBS=../libpwq/libpthread_workqueue.la
  PTHREAD_WORKQUEUE_CFLAGS=-I../libpwq/include
else
  if HAVE_PTHREAD_WORKQUEUES
    PTHREAD_WORKQUEUE_LIBS=-lpthread_workqueue
  endif
endif

libdispatch_la_LDFLAGS=-avoid-version
libdispatch_la_LIBADD=$(KQUEUE_LIBS) $(PTHREAD_WORKQUEUE_LIBS) $(BSD_OVERLAY_LIBS)

if HAVE_DARWIN_LD
libdispatch_la_LDFLAGS+=-Wl,-compatibility_version,1 \
	-Wl,-current_version,$(VERSION) -Wl,-dead_strip \
	-Wl,-alias_list,$(top_srcdir)/xcodeconfig/libdispatch.aliases
endif

if USE_OBJC
libdispatch_la_SOURCES+=block.cpp data.m object.m
libdispatch_la_OBJCFLAGS=$(AM_OBJCFLAGS) -Wno-switch -fobjc-gc
libdispatch_la_CXXFLAGS=$(AM_CXXFLAGS) -std=gnu++11 -fno-exceptions
libdispatch_la_LDFLAGS+=-Wl,-upward-lobjc -Wl,-upward-lauto \
	-Wl,-order_file,$(top_srcdir)/xcodeconfig/libdispatch.order \
	-Wl,-alias_list,$(top_srcdir)/xcodeconfig/libdispatch_objc.aliases \
	-Wl,-unexported_symbols_list,$(top_srcdir)/xcodeconfig/libdispatch.unexport
endif

if USE_MIG
MIG_SOURCES=				\
	protocolUser.c			\
	protocol.h				\
	protocolServer.c		\
	protocolServer.h

%User.c %.h %Server.c %Server.h: $(abs_srcdir)/%.defs
	$(MIG) -user $*User.c -header $*.h \
	    -server $*Server.c -sheader $*Server.h $<
endif

if USE_DTRACE
DTRACE_SOURCES=provider.h

%.h: $(abs_srcdir)/%.d
	$(DTRACE) -h -s $< -o $@
endif

BUILT_SOURCES=$(MIG_SOURCES) $(DTRACE_SOURCES)
nodist_libdispatch_la_SOURCES=$(BUILT_SOURCES)
CLEANFILES=$(BUILT_SOURCES)
DISTCLEANFILES=pthread_machdep.h pthread System mach objc

