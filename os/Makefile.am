#
#
#

if !HAVE_SWIFT
osdir=$(includedir)/os
else
if HAVE_X64
osdir=${prefix}/lib64/swift/os
else
osdir=${prefix}/lib/swift/os
endif
endif


os_HEADERS=	\
	object.h \
	linux_base.h

noinst_HEADERS=	\
	object_private.h \
	voucher_activity_private.h \
	voucher_private.h
