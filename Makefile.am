#
#
#

ACLOCAL_AMFLAGS = -I m4

if BUILD_OWN_PTHREAD_WORKQUEUES
  MAYBE_PTHREAD_WORKQUEUES = libpwq
endif

if BUILD_OWN_KQUEUES
  MAYBE_KQUEUES = libkqueue
endif

if BUILD_TESTS
  MAYBE_TESTS = tests
endif

SUBDIRS=						\
	dispatch					\
	$(MAYBE_PTHREAD_WORKQUEUES)	\
	$(MAYBE_KQUEUES)			\
	man							\
	os							\
	private						\
	src							\
	$(MAYBE_TESTS)

EXTRA_DIST=					\
	README.md				\
	LICENSE					\
	PATCHES					\
	autogen.sh				\
	config/config.h			\
	libdispatch.xcodeproj	\
	resolver				\
	tools					\
	xcodeconfig				\
	xcodescripts
