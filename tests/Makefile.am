#
#
#

AUTOMAKE_OPTIONS = subdir-objects

noinst_LTLIBRARIES=libbsdtests.la

libbsdtests_la_SOURCES=				\
	bsdtests.c				\
	bsdtests.h				\
	dispatch_test.c				\
	dispatch_test.h				\
	linux_port.h

check_PROGRAMS=					\
	bsdtestharness				\
	bsdtestsummarize			\
	$(TESTS)

noinst_SCRIPTS=leaks-wrapper.sh

# Tests that have not been ported to Linux
# because they test unsupported functionality.
UNPORTED_TESTS=					\
	dispatch_deadname			\
	dispatch_proc				\
	dispatch_vm					\
	dispatch_vnode

# Tests that usually pass, but occasionally fail
# and are therefore not suitable for general CI usage.
UNRELIABLE_TESTS=				\
	dispatch_priority			\
	dispatch_priority2			\
	dispatch_concur				\
	dispatch_read				\
	dispatch_read2				\
	dispatch_suspend_timer		\
	dispatch_pingpong			\
	dispatch_drift				\
	dispatch_readsync			\
	dispatch_cascade			\
	dispatch_io

if EXTENDED_TEST_SUITE
ADDITIONAL_TESTS= $(UNRELIABLE_TESTS)
else
ADDITIONAL_TESTS=
endif

# The set of tests that will be run by 'make check'
TESTS=							\
	dispatch_apply				\
	dispatch_api				\
	dispatch_c99				\
	dispatch_debug				\
	dispatch_queue_finalizer	\
	dispatch_group				\
	dispatch_overcommit			\
	dispatch_plusplus			\
	dispatch_context_for_key	\
	dispatch_after				\
	dispatch_timer				\
	dispatch_timer_short		\
	dispatch_timer_timeout		\
	dispatch_sema				\
	dispatch_timer_bit31		\
	dispatch_timer_bit63		\
	dispatch_timer_set_time		\
	dispatch_starfish			\
	dispatch_data				\
	dispatch_io_net				\
	dispatch_select				\
	$(ADDITIONAL_TESTS)


dispatch_c99_CFLAGS=$(DISPATCH_TESTS_CFLAGS) $(CBLOCKS_FLAGS) $(KQUEUE_CFLAGS) -std=c99
dispatch_plusplus_SOURCES=dispatch_plusplus.cpp
dispatch_priority2_SOURCES=dispatch_priority.c
dispatch_priority2_CPPFLAGS=$(AM_CPPFLAGS) -DUSE_SET_TARGET_QUEUE=1

AM_CPPFLAGS=-I$(top_builddir) -I$(top_srcdir)

DISPATCH_TESTS_CFLAGS=-Wall -Wno-deprecated-declarations $(MARCH_FLAGS)
AM_CFLAGS=$(DISPATCH_TESTS_CFLAGS) $(CBLOCKS_FLAGS) $(KQUEUE_CFLAGS) $(BSD_OVERLAY_CFLAGS)
AM_OBJCFLAGS=$(DISPATCH_TESTS_CFLAGS) $(CBLOCKS_FLAGS)
AM_CXXFLAGS=$(DISPATCH_TESTS_CFLAGS) $(CXXBLOCKS_FLAGS) $(BSD_OVERLAY_CFLAGS)
AM_OBJCXXFLAGS=$(DISPATCH_TESTS_CFLAGS) $(CXXBLOCKS_FLAGS)

if BUILD_OWN_KQUEUES
  KQUEUE_LIBS+=$(top_builddir)/libkqueue/libkqueue.la
  KQUEUE_CFLAGS+=-I$(top_srcdir)/libkqueue/include
endif

if !BUILD_OWN_PTHREAD_WORKQUEUES
if HAVE_PTHREAD_WORKQUEUES
  PTHREAD_WORKQUEUE_LIBS=-lpthread_workqueue
endif
endif

if BUILD_OWN_BLOCKS_RUNTIME
CBLOCKS_FLAGS+= -I$(top_srcdir)/src/BlocksRuntime
CXXBLOCKS_FLAGS+= -I$(top_srcdir)/src/BlocksRuntime
endif

if HAVE_SWIFT
  SWIFT_LIBS=-L$(SWIFT_LIBDIR) -lswiftCore -lswiftSwiftOnoneSupport
  AM_LDFLAGS=-rpath $(SWIFT_LIBDIR)
endif

LDADD=libbsdtests.la $(top_builddir)/src/libdispatch.la $(KQUEUE_LIBS) $(PTHREAD_WORKQUEUE_LIBS) $(BSD_OVERLAY_LIBS) $(SWIFT_LIBS)
libbsdtests_la_LDFLAGS=-avoid-version

bsdtestsummarize_LDADD=-lm $(BSD_OVERLAY_LIBS)
dispatch_timer_short_LDADD=-lm $(LDADD)
dispatch_group_LDADD=-lm $(LDADD)

if HAVE_LEAKS
AM_TESTS_ENVIRONMENT=
else
AM_TESTS_ENVIRONMENT=NOLEAKS=1
endif
LOG_COMPILER=./bsdtestharness

DISTCLEAN=Foundation/bench.cc

if HAVE_COREFOUNDATION
TESTS+=							\
	dispatch_cf_main			\
	dispatch_transform			\
	dispatch_sync_on_main		\
	cffd
AM_CFLAGS+=-DHAVE_COREFOUNDATION

dispatch_cf_main_LDFLAGS=-framework CoreFoundation
dispatch_transform_LDFLAGS=-framework CoreFoundation -framework Security
dispatch_sync_on_main_LDFLAGS=-framework CoreFoundation
cffd_LDFLAGS=-framework CoreFoundation
endif

if HAVE_FOUNDATION
TESTS+=							\
	dispatch_sync_gc			\
	dispatch_apply_gc			\
	nsoperation					\
	bench
AM_CFLAGS+=-DHAVE_FOUNDATION

dispatch_sync_gc_SOURCES=Foundation/dispatch_sync_gc.m
dispatch_sync_gc_LDFLAGS=-framework Foundation
dispatch_apply_gc_SOURCES=Foundation/dispatch_apply_gc.m
dispatch_apply_gc_LDFLAGS=-framework Foundation
nsoperation_SOURCES=Foundation/nsoperation.m
nsoperation_LDFLAGS=-framework Foundation
bench_SOURCES=Foundation/bench.mm func.c
bench_LDFLAGS=-framework Foundation
endif

# For use by swift/utils/build-script to force test cases to be
# built during the build phase of CI
build-tests: $(TESTS) $(check_PROGRAMS)
