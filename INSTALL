Grand Central Dispatch (GCD)

GCD is a concurrent programming framework first shipped with Mac OS X Snow
Leopard.  This package is an open source bundling of libdispatch, the core
user space library implementing GCD.  At the time of writing, support for
the BSD kqueue API, and specifically extensions introduced in Mac OS X Snow
Leopard and FreeBSD 9-CURRENT, are required to use libdispatch.  Other
systems are currently unsupported.

  Configuring and installing libdispatch

GCD is built using autoconf, automake, and libtool, and has a number of
compile-time configuration options that should be reviewed before starting.
An uncustomized install requires:

	sh autogen.sh
	./configure
	make
	make install

The following configure options may be of general interest:

--with-apple-libc-source

	Specify the path to Apple's Libc package, so that appropriate headers can
	be found and used.

--with-apple-libclosure-source

	Specify the path to Apple's Libclosure package, so that appropriate headers
	can be found and used.

--with-apple-xnu-source

	Specify the path to Apple's XNU package, so that appropriate headers can be
	found and used.

--with-blocks-runtime

	On systems where -fblocks is supported, specify an additional library path
	in which libBlocksRuntime can be found.  This is not required on Mac OS X,
	where the Blocks runtime is included in libSystem, but is required on
	FreeBSD.

The following options are likely to only be useful when building libdispatch on
Mac OS X as a replacement for /usr/lib/system/libdispatch.dylib:

--with-apple-objc4-source

	Specify the path to Apple's objc4 package, so that appropriate headers can
	be found and used.

--with-apple-libauto-source

	Specify the path to Apple's libauto package, so that appropriate headers
	can be found and used.

--disable-libdispatch-init-constructor

	Do not tag libdispatch's init routine as __constructor, in which case it
	must be run manually before libdispatch routines can be called. This is the
	default when building on Mac OS X. For /usr/lib/system/libdispatch.dylib
	the init routine is called automatically during process start.

--enable-apple-tsd-optimizations

	Use a non-portable allocation scheme for pthread per-thread data (TSD) keys
	when building libdispatch for /usr/lib/system on Mac OS X.  This should not
	be used on other OS's, or on Mac OS X when building a stand-alone library.

  Typical configuration commands

The following command lines create the configuration required to build
libdispatch for /usr/lib/system on OS X MountainLion:

	sh autogen.sh
	cflags='-arch x86_64 -arch i386 -g -Os'
	./configure CFLAGS="$cflags" OBJCFLAGS="$cflags" CXXFLAGS="$cflags" \
		--prefix=/usr --libdir=/usr/lib/system \
		--disable-dependency-tracking --disable-static \
		--enable-apple-tsd-optimizations \
		--with-apple-libc-source=/path/to/10.8.0/Libc-825.24 \
		--with-apple-libclosure-source=/path/to/10.8.0/libclosure-59 \
		--with-apple-xnu-source=/path/to/10.8.0/xnu-2050.7.9 \
		--with-apple-objc4-source=/path/to/10.8.0/objc4-532 \
		--with-apple-libauto-source=/path/to/10.8.0/libauto-185.1
	make check

Typical configuration line for FreeBSD 8.x and 9.x to build libdispatch with
clang and blocks support:

	sh autogen.sh
	./configure CC=clang --with-blocks-runtime=/usr/local/lib
	make check
