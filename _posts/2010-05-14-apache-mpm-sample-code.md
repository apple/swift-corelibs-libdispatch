---
title: Apache MPM Sample Code
slug: apache-mpm-sample-code
date: 2010-05-14 12:37:08.510641-07
---

The Apache GCD MPM allows the [Apache web server](http://httpd.apache.org/) to use [Grand Central Dispatch](http://www.apple.com/macosx/technology/#grandcentral) (GCD), rather than threads or processes, as a source of concurrency on [Mac OS X](http://www.apple.com/macosx/) and [FreeBSD](http://www.freebsd.org/). It is intended to illustrate both GCD's programming model and programmability benefits, as well as how GCD can be integrated into a existing large-scale and thread-aware application. 

<!--more-->

# Apache GCD MPM
The Apache GCD MPM, developed by ​Robert Watson at the University of Cambridge, allows the Apache web server to use Grand Central Dispatch (GCD), rather than threads or processes, as a source of concurrency on Mac OS X and FreeBSD. It is intended to illustrate both GCD's programming model and programmability benefits, as well as how GCD can be integrated into a existing large-scale and thread-aware application. A quick skim of the code will bring to light some of its key benefits: a work model integrated with the GCD concurrency framework avoids complex utility libraries to distribute work and manage concurrency, and the integration of C Blocks into the programming language avoids the complexity-overhead of a message passing model.

Please see the implementation notes below before attempting to use this software.

## Patches

[```20100421a-apache-gcdmpm.diff```](https://raw.githubusercontent.com/apple/swift-corelibs-libdispatch/macosforge/trac/attachment/wiki/apache/20100421a-apache-gcdmpm.diff) is the April 21 2010(a) version of the Apache GCD MPM patch. 

The patch is relative to Apache httpd-trunk r895969 (5 January 2010) and apr r886734 (1 December 2009).

[```20100506-apr-buildconf.diff```](https://raw.githubusercontent.com/apple/swift-corelibs-libdispatch/macosforge/trac/attachment/wiki/apache/20100506-apr-buildconf.diff) is a tweak to apr r886734's buildconf required on Mac OS X for the 20100421a release of the GCD MPM.

## Implementation notes

The GCD MPM is experimental software, so should not (yet) be relied on in production.

The GCD MPM has been built and tested on both Mac OS X Snow Leopard and FreeBSD 8-STABLE. On FreeBSD, a GCD-patched Apache must be compiled using clang so that C Blocks are supported.

The only known serious functional issue with the GCD MPM is that the Apache scoreboard (monitoring) feature is not currently supported, as existing code relies on a unique process/thread ID for each in-flight connection.

See the comments in gcd.c for more information on known issues, as well as design and implementation notes.

## Compiling and installing the GCD MPM

To use the Apache GCD MPM, you will need to check out, build, and install Apache's httpd-trunk Subversion repository. GCD MPM patches are synchronized to specific versions of the Subversion trunk, and may neither compile nor work with other versions!

On FreeBSD, begin by installing the libdispatch port. Its configure scripts will detect whether an 8-STABLE system is new enough to support GCD. Likewise, the clang port must also be installed so that C Blocks are available.

First, download the Apache GCD MPM patch. Mac OS X:

```
% curl -O http://libdispatch.macosforge.org/trac/raw-attachment/wiki/apache/20100421a-apache-gcdmpm.diff
% curl -O http://libdispatch.macosforge.org/trac/raw-attachment/wiki/apache/20100506-apr-buildconf.diff
```

FreeBSD:

```
% fetch http://libdispatch.macosforge.org/trac/raw-attachment/wiki/apache/20100421a-apache-gcdmpm.diff
```

On FreeBSD, install the pcre package; on Mac OS X, you will need to download, build, and install it:

```
% curl -O ftp://ftp.csx.cam.ac.uk/pub/software/programming/pcre/pcre-8.02.tar.gz
% tar xvf pcre-8.02.tar.gz
% cd pcre-8.02
% ./configure
% make
% sudo make install
```

Next, check out Apache httpd and apr; notice that apr is checked out within the httpd checkout:

```
% svn co -r r895969  http://svn.apache.org/repos/asf/httpd/httpd/trunk
...
% cd trunk/srclib
% svn co -r 886734 http://svn.apache.org/repos/asf/apr/apr/trunk apr
...
% cd ..
```

The version of glibtoolize shipped with Mac OS X Snow Leopard seems not to like the version of buildconf shipped with apr; as such, you'll need to apply a patch to it (Mac OS X only) in the trunk/srclib/apr directory:

```
% cd srclib/apr
% patch < ../../../20100506-apr-buildconf.diff
% cd ../..
```
Next, apply the Apache GCD MPM patch in the trunk directory:

```
% patch -p0 < ../20100421a-apache-gcdmpm.diff
patching file server/mpm/MPM.NAMING
patching file server/mpm/config.m4
patching file server/mpm/config2.m4
patching file server/mpm/gcd/Makefile.in
patching file server/mpm/gcd/gcd.h
patching file server/mpm/gcd/mpm_default.h
patching file server/mpm/gcd/config.m4
patching file server/mpm/gcd/config3.m4
patching file server/mpm/gcd/gcd_parent.c
patching file server/mpm/gcd/gcd.c
patching file modules/arch/unix/config5.m4
```

Because you're building from the development version of Apache, you will need to generate autoconf and other configuration files using buildconf in the trunk directory:

```
% ./buildconf
...
```

Next, you must configure Apache to use the checked out APR and GCD MPM. On Mac OS X, gcc supports C Blocks so you can use:

```
%./configure --with-included-apr --with-mpm=gcd
...
```

On FreeBSD, you need to point configure at clang, as well as tell it where to find libiconv:

```
% CC=clang ./configure --with-included-apr --with-mpm=gcd CFLAGS="-I/usr/local/include" LDFLAGS="-L/usr/local/lib -liconv -lm"
```
Finally, build and install Apache from the trunk directory:

```
# make
...
# make install
...
```

Apache should otherwise operate as normal.

