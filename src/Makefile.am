#
#
#

if HAVE_SWIFT
swiftlibdir=${prefix}/lib/swift/linux
swiftlib_LTLIBRARIES=libdispatch.la
else
lib_LTLIBRARIES=libdispatch.la
endif

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
	shims/lock.c			\
	shims/lock.h			\
	shims/perfmon.h			\
	shims/time.h			\
	shims/tsd.h				\
	shims/yield.h

EXTRA_libdispatch_la_SOURCES=
EXTRA_libdispatch_la_DEPENDENCIES=

AM_CPPFLAGS=-I$(top_builddir) -I$(top_srcdir) -I$(top_srcdir)/private

DISPATCH_CFLAGS=-Wall $(VISIBILITY_FLAGS) $(OMIT_LEAF_FP_FLAGS) \
	$(MARCH_FLAGS) $(KQUEUE_CFLAGS) $(BSD_OVERLAY_CFLAGS)
AM_CFLAGS= $(PTHREAD_WORKQUEUE_CFLAGS) $(DISPATCH_CFLAGS) $(CBLOCKS_FLAGS)
AM_OBJCFLAGS=$(DISPATCH_CFLAGS) $(CBLOCKS_FLAGS)
AM_CXXFLAGS=$(PTHREAD_WORKQUEUE_CFLAGS) $(DISPATCH_CFLAGS) $(CXXBLOCKS_FLAGS)
AM_OBJCXXFLAGS=$(DISPATCH_CFLAGS) $(CXXBLOCKS_FLAGS)

if BUILD_OWN_PTHREAD_WORKQUEUES
  PTHREAD_WORKQUEUE_LIBS=$(top_builddir)/libpwq/libpthread_workqueue.la
  PTHREAD_WORKQUEUE_CFLAGS=-I$(top_srcdir)/libpwq/include
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
	-Wl,-order_file,$(top_srcdir)/xcodeconfig/libdispatch.order
else
libdispatch_la_SOURCES+=block.cpp
libdispatch_la_CXXFLAGS=$(AM_CXXFLAGS) -std=gnu++11 -fno-exceptions
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

if HAVE_SWIFT
libdispatch_la_SOURCES+=swift/swift_wrappers.c
EXTRA_libdispatch_la_SOURCES+=swift/Dispatch.swift
EXTRA_libdispatch_la_DEPENDENCIES+=$(abs_builddir)/Dispatch.o $(abs_builddir)/Dispatch.swiftmodule
libdispatch_la_LIBADD+=$(abs_builddir)/Dispatch.o

SWIFT_OBJECTS=	\
	$(abs_builddir)/Dispatch.swiftmodule \
	$(abs_builddir)/Dispatch.swiftdoc \
	$(abs_builddir)/Dispatch.o

SWIFTC_FLAGS = -Xcc -D__DISPATCH_BUILDING_SWIFT_MODULE__=1 -Xcc -fmodule-map-file=$(abs_top_srcdir)/dispatch/module.map -I$(abs_top_srcdir) -parse-as-library -Xcc -fblocks

$(abs_builddir)/Dispatch.o: $(abs_srcdir)/swift/Dispatch.swift
	$(SWIFTC) $(SWIFTC_FLAGS) -c -o $@ $<

$(abs_builddir)/Dispatch.swiftmodule: $(abs_srcdir)/swift/Dispatch.swift
	$(SWIFTC) $(SWIFTC_FLAGS) -emit-module -emit-module-path $@ $<

if HAVE_SWIFT
swiftmoddir=${prefix}/lib/swift/linux/${build_cpu}
swiftmod_HEADERS=$(abs_builddir)/Dispatch.swiftmodule $(abs_builddir)/Dispatch.swiftdoc
endif

endif

BUILT_SOURCES=$(MIG_SOURCES) $(DTRACE_SOURCES)
nodist_libdispatch_la_SOURCES=$(BUILT_SOURCES)
CLEANFILES=$(BUILT_SOURCES) $(SWIFT_OBJECTS)
DISTCLEANFILES=pthread_machdep.h pthread System mach objc

