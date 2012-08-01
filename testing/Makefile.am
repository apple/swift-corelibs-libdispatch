#
#
#

noinst_LTLIBRARIES=libbsdtests.la

libbsdtests_la_SOURCES=			\
	bsdtests.c					\
	bsdtests.h					\
	dispatch_test.c				\
	dispatch_test.h				\
	shims.h						\
	shims/asprintf.c			\
	shims/asprintf.h			\
	shims/arc4random.c			\
	shims/arc4random.h			\
	shims/fgetln.c				\
	shims/fgetln.h

check_PROGRAMS=					\
	bsdtestharness				\
	bsdtestsummarize			\
	$(TESTS)

noinst_SCRIPTS=leaks-wrapper.sh

TESTS=							\
	dispatch_apply				\
	dispatch_api				\
	dispatch_c99				\
	dispatch_deadname			\
	dispatch_debug				\
	dispatch_queue_finalizer	\
	dispatch_group				\
	dispatch_overcommit			\
	dispatch_pingpong			\
	dispatch_plusplus			\
	dispatch_priority			\
	dispatch_priority2			\
	dispatch_concur				\
	dispatch_context_for_key	\
	dispatch_proc				\
	dispatch_read				\
	dispatch_read2				\
	dispatch_after				\
	dispatch_timer				\
	dispatch_timer_short		\
	dispatch_timer_timeout		\
	dispatch_sema				\
	dispatch_suspend_timer		\
	dispatch_timer_bit31		\
	dispatch_timer_bit63		\
	dispatch_timer_set_time		\
	dispatch_starfish			\
	dispatch_cascade			\
	dispatch_drift				\
	dispatch_readsync			\
	dispatch_data				\
	dispatch_io					\
	dispatch_io_net				\
	dispatch_vm					\
	dispatch_vnode				\
	dispatch_select

dispatch_c99_CFLAGS=-std=c99
dispatch_plusplus_SOURCES=dispatch_plusplus.cpp
dispatch_priority2_SOURCES=dispatch_priority.c
dispatch_priority2_CFLAGS=-DUSE_SET_TARGET_QUEUE=1

INCLUDES=-I$(top_builddir) -I$(top_srcdir)

CPPFLAGS=
CFLAGS=-Wall -g $(MARCH_FLAGS) $(CBLOCKS_FLAGS)
OBJCFLAGS=-Wall -g $(MARCH_FLAGS) $(CBLOCKS_FLAGS)
CXXFLAGS=-Wall -g $(MARCH_FLAGS) $(CXXBLOCKS_FLAGS)

LDADD=libbsdtests.la ../src/libdispatch.la
libbsdtests_la_LDFLAGS=-avoid-version

TESTS_ENVIRONMENT=./bsdtestharness
DISTCLEAN=bench.cc

if HAVE_COREFOUNDATION
TESTS+=							\
	dispatch_cf_main			\
	dispatch_sync_on_main		\
	cffd

dispatch_cf_main_LDFLAGS=-framework CoreFoundation
dispatch_sync_on_main_LDFLAGS=-framework CoreFoundation
cffd_LDFLAGS=-framework CoreFoundation
endif

if HAVE_FOUNDATION
TESTS+=							\
	dispatch_sync_gc			\
	dispatch_apply_gc			\
	nsoperation					\
	bench

dispatch_sync_gc_SOURCES=dispatch_sync_gc.m
dispatch_sync_gc_OBJCFLAGS=-fobjc-gc
dispatch_sync_gc_LDFLAGS=-framework Foundation
dispatch_apply_gc_SOURCES=dispatch_apply_gc.m
dispatch_apply_gc_OBJCFLAGS=-fobjc-gc
dispatch_apply_gc_LDFLAGS=-framework Foundation
nsoperation_SOURCES=nsoperation.m
nsoperation_LDFLAGS=-framework Foundation
bench_SOURCES=bench.cc func.c
bench_LDFLAGS=-framework Foundation
# Workaround missing objcxx support in older autotools
bench.o: bench.cc
	$(CXXCOMPILE) -x objective-c++ -c -o $@ $<
endif
