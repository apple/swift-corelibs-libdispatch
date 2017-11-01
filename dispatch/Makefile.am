#
#
#

if !HAVE_SWIFT
dispatchdir=$(includedir)/dispatch
else
if HAVE_X64
dispatchdir=${prefix}/lib64/swift/dispatch
else
dispatchdir=${prefix}/lib/swift/dispatch
endif
endif

dispatch_HEADERS=	\
	base.h			\
	block.h			\
	data.h			\
	dispatch.h		\
	group.h			\
	introspection.h	\
	io.h			\
	object.h		\
	once.h			\
	queue.h			\
	semaphore.h		\
	source.h		\
	time.h

if HAVE_SWIFT
dispatch_HEADERS+=module.modulemap
endif
