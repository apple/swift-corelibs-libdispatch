---
title: libdispatch is open source!
slug: libdispatch-is-open-source
date: 2009-09-10 16:01:06.396856-07
---

## Welcome to the libdispatch project!

The libdispatch project consists of the user space implementation of the Grand Central Dispatch API as seen in Mac OS X version 10.6 Snow Leopard. The Mac OS X kernel support for GCD may be found in the [xnu project](https://opensource.apple.com/source/xnu/xnu-1456.1.26/). While kernel support provides many performance optimizations on Mac OS X, it is not strictly required for portability to other platforms. However, in order to implement the full API for Grand Central Dispatch, [C compiler](http://clang.llvm.org) support for blocks is required. The [blocks runtime](http://compiler-rt.llvm.org) is available as part of the [LLVM project](http://llvm.org).

This project is intended to be a resource for developers interested in learning more about libdispatch on Mac OS X. Contributions to this project will be continually evaluated for possible inclusion in future releases of Mac OS X. The sources are available under the terms of the [Apache License, Version 2.0](https://opensource.apple.com/license/apache/) in the hope that they might serve as a launching point for porting GCD to other platforms.


<!--more-->

## Source Code

You can [browse the sources](https://libdispatch.macosforge.org/trac/browser) online or check out the latest sources from the repository via [Subversion](https://svn.macosforge.org/repository/libdispatch/trunk/) or [Git](git://git.macosforge.org/libdispatch.git).


## Additional Resources

We recognize that libdispatch is a new technology and you likely have many questions. Here are some documentation resources for getting started:

* [Introducing Blocks and Grand Central Dispatch](http://developer.apple.com/mac/articles/cocoa/introblocksgcd.html)
* [Concurrency Programming Guide](http://developer.apple.com/mac/library/documentation/General/Conceptual/ConcurrencyProgrammingGuide/Introduction/Introduction.html)
* [Grand Central Dispatch (GCD) Reference](http://developer.apple.com/mac/library/documentation/Performance/Reference/GCD_libdispatch_Ref/Reference/reference.html)

Feel free to direct any questions you may have to the new email discussion list:

* [libdispatch-dev@lists.macosforge.org](https://lists.macosforge.org/mailman/listinfo.cgi/libdispatch-dev)

Sincerely,  
The GCD team
