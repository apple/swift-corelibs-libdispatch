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
	firehose/firehose_internal.h \
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
	$(MARCH_FLAGS) $(BSD_OVERLAY_CFLAGS)
if DISPATCH_ENABLE_ASSERTS
DISPATCH_CFLAGS+=-DDISPATCH_DEBUG=1
endif
AM_CFLAGS= $(KQUEUE_CFLAGS) $(PTHREAD_WORKQUEUE_CFLAGS) $(DISPATCH_CFLAGS) $(CBLOCKS_FLAGS)
AM_OBJCFLAGS=$(DISPATCH_CFLAGS) $(CBLOCKS_FLAGS)
AM_CXXFLAGS=$(KQUEUE_CFLAGS) $(PTHREAD_WORKQUEUE_CFLAGS) $(DISPATCH_CFLAGS) $(CXXBLOCKS_FLAGS)
AM_OBJCXXFLAGS=$(DISPATCH_CFLAGS) $(CXXBLOCKS_FLAGS)

if BUILD_OWN_KQUEUES
  KQUEUE_LIBS+=$(top_builddir)/libkqueue/libkqueue.la
  KQUEUE_CFLAGS+=-I$(top_srcdir)/libkqueue/include
endif

if BUILD_OWN_PTHREAD_WORKQUEUES
  PTHREAD_WORKQUEUE_LIBS=$(top_builddir)/libpwq/libpthread_workqueue.la
  PTHREAD_WORKQUEUE_CFLAGS=-I$(top_srcdir)/libpwq/include
else
if HAVE_PTHREAD_WORKQUEUES
  PTHREAD_WORKQUEUE_LIBS=-lpthread_workqueue
endif
endif

if BUILD_OWN_BLOCKS_RUNTIME
libdispatch_la_SOURCES+= BlocksRuntime/data.c BlocksRuntime/runtime.c
CBLOCKS_FLAGS+= -I$(top_srcdir)/src/BlocksRuntime
CXXBLOCKS_FLAGS+= -I$(top_srcdir)/src/BlocksRuntime
BLOCKS_RUNTIME_LIBS=-ldl
endif

libdispatch_la_LDFLAGS=-avoid-version
libdispatch_la_LIBADD=$(KQUEUE_LIBS) $(PTHREAD_WORKQUEUE_LIBS) $(BSD_OVERLAY_LIBS) $(BLOCKS_RUNTIME_LIBS)

if HAVE_DARWIN_LD
libdispatch_la_LDFLAGS+=-Wl,-compatibility_version,1 \
	-Wl,-current_version,$(VERSION) -Wl,-dead_strip \
	-Wl,-alias_list,$(top_srcdir)/xcodeconfig/libdispatch.aliases
endif

if USE_GOLD_LINKER
libdispatch_la_LDFLAGS+=-Xcompiler -fuse-ld=gold
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
SWIFT_SRC_FILES=\
	swift/Block.swift \
	swift/Data.swift \
	swift/Dispatch.swift \
	swift/IO.swift \
	swift/Private.swift \
	swift/Queue.swift \
	swift/Source.swift \
	swift/Time.swift \
	swift/Wrapper.swift

SWIFT_ABS_SRC_FILES = $(SWIFT_SRC_FILES:%=$(abs_srcdir)/%)
SWIFT_OBJ_FILES = $(SWIFT_SRC_FILES:%.swift=$(abs_builddir)/%.o)

libdispatch_la_SOURCES+=swift/DispatchStubs.cc
EXTRA_libdispatch_la_SOURCES+=$(SWIFT_SRC_FILES)

EXTRA_libdispatch_la_DEPENDENCIES+=$(SWIFT_OBJ_FILES) $(abs_builddir)/swift/Dispatch.swiftmodule
libdispatch_la_LIBADD+=$(SWIFT_OBJ_FILES)

SWIFT_GEN_FILES=	\
	$(abs_builddir)/swift/Dispatch.swiftmodule \
	$(abs_builddir)/swift/Dispatch.swiftdoc \
	$(SWIFT_OBJ_FILES) \
	$(SWIFT_OBJ_FILES:%=%.d) \
	$(SWIFT_OBJ_FILES:%=%.swiftdeps) \
	$(SWIFT_OBJ_FILES:%=%.~partial.swiftmodule) \
	$(SWIFT_OBJ_FILES:%=%.~partial.swiftdoc) \
	$(SWIFT_OBJ_FILES:%=%.~partial.swiftdeps)

SWIFTC_FLAGS = -Xcc -fmodule-map-file=$(abs_top_srcdir)/dispatch/module.modulemap -I$(abs_top_srcdir) -Xcc -fblocks
if DISPATCH_ENABLE_OPTIMIZATION
SWIFTC_FLAGS+=-O
endif

$(abs_builddir)/swift/%.o:	$(abs_srcdir)/swift/%.swift $(SWIFTC)
	@rm -f $@
	$(SWIFTC) -frontend -c $(SWIFT_ABS_SRC_FILES) -primary-file $< \
	$(SWIFTC_FLAGS) -module-name Dispatch -module-link-name dispatch \
	-o $@ -emit-module-path $@.~partial.swiftmodule \
	-emit-module-doc-path $@.~partial.swiftdoc -emit-dependencies-path $@.d \
	-emit-reference-dependencies-path $@.swiftdeps \
	-module-cache-path $(top_builddir)

$(abs_builddir)/swift/Dispatch.swiftmodule: $(SWIFT_ABS_SRC_FILES) $(SWIFTC)
	@rm -f $@
	$(SWIFTC) -frontend -emit-module $(SWIFT_OBJ_FILES:%=%.~partial.swiftmodule) \
	$(SWIFTC_FLAGS) -module-cache-path $(top_builddir) -module-link-name dispatch \
	-o $@ -emit-module-doc-path $(@:%.swiftmodule=%.swiftdoc)

swiftmoddir=${prefix}/lib/swift/linux/${build_cpu}
swiftmod_HEADERS=\
	$(abs_builddir)/swift/Dispatch.swiftmodule \
	$(abs_builddir)/swift/Dispatch.swiftdoc

endif

BUILT_SOURCES=$(MIG_SOURCES) $(DTRACE_SOURCES)
nodist_libdispatch_la_SOURCES=$(BUILT_SOURCES)
CLEANFILES=$(BUILT_SOURCES) $(SWIFT_GEN_FILES)
DISTCLEANFILES=pthread_machdep.h pthread System mach objc

