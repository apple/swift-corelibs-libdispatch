#
#
#

noinst_LTLIBRARIES=libtest.la

libtest_la_SOURCES=			\
	dispatch_test.c

TESTS=					\
	dispatch_after			\
	dispatch_api			\
	dispatch_apply			\
	dispatch_c99			\
	dispatch_cascade		\
	dispatch_cffd			\
	dispatch_debug			\
	dispatch_drift			\
	dispatch_group			\
	dispatch_pingpong		\
	dispatch_priority		\
	dispatch_proc			\
	dispatch_read			\
	dispatch_readsync		\
	dispatch_sema			\
	dispatch_starfish		\
	dispatch_timer_bit31		\
	dispatch_timer_bit63		\
	dispatch_timer_set_time		\
	queue_finalizer			\
	nsoperation

noinst_PROGRAMS=$(TESTS)

TOOLS=					\
	harness				\
	summary

noinst_PROGRAMS+=$(TOOLS)

LDADD=libtest.la ../src/libdispatch.la
CFLAGS=-Werror -Wall -DDISPATCH_NO_LEGACY $(MARCH_FLAGS) $(CBLOCKS_FLAGS)

