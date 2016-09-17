---
title: Apache MPM Sample Code
slug: apache-mpm-sample-code
date: 2010-05-14 12:37:08.510641-07
---

The Apache GCD MPM allows the [Apache web server](http://httpd.apache.org/) to use [Grand Central Dispatch](http://www.apple.com/macosx/technology/#grandcentral) (GCD), rather than threads or processes, as a source of concurrency on [Mac OS X](http://www.apple.com/macosx/) and [FreeBSD](http://www.freebsd.org/). It is intended to illustrate both GCD's programming model and programmability benefits, as well as how GCD can be integrated into a existing large-scale and thread-aware application. See the [wiki page](https://libdispatch.macosforge.org/trac/wiki/apache) for more information.
