#
#
#

if HAVE_SWIFT
dispatchdir=${prefix}/lib/swift/dispatch
else
dispatchdir=$(includedir)/dispatch
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
dispatch_HEADERS+=module.map
endif
