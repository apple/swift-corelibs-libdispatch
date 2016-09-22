#
#
#

ACLOCAL_AMFLAGS = -I m4

if BUILD_OWN_PTHREAD_WORKQUEUES
SUBDIRS=					\
	dispatch				\
	libpwq					\
	man						\
	os						\
	private					\
	src						\
	tests
else
SUBDIRS=					\
	dispatch				\
	man						\
	os						\
	private					\
	src						\
	tests
endif

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
