#
#
#

ACLOCAL_AMFLAGS = -I m4

if BUILD_TESTS
  MAYBE_TESTS = tests
endif

SUBDIRS=						\
	dispatch					\
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
