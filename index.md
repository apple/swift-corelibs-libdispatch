---
title: Grand Central Dispatch
---

{:.lead}
Grand Central Dispatch (GCD or libdispatch) provides comprehensive support for concurrent code execution on multicore hardware.

libdispatch is currently available on all Darwin platforms. This project aims to make a modern version of libdispatch available on all other [Swift](https://swift.org) platforms. To do this, we will implement as much of the portable subset of the API as possible, using the existing open source C implementation.


## License terms

* [Apache License, Version 2.0](https://opensource.org/licenses/apache2.0.php)


## Source code

* <https://github.com/apple/swift-corelibs-libdispatch>


## Mailing lists

* [libdispatch-dev](https://lists.macosforge.org/mailman/listinfo/libdispatch-dev) General discussion

## Additional resources

{% comment %}
### Documentation

* [Introducing Blocks and Grand Central Dispatch](http://developer.apple.com/mac/articles/cocoa/introblocksgcd.html)
* [Concurrency Programming Guide](http://developer.apple.com/mac/library/documentation/General/Conceptual/ConcurrencyProgrammingGuide/Introduction/Introduction.html)
* [Grand Central Dispatch (GCD) Reference](http://developer.apple.com/mac/library/documentation/Performance/Reference/GCD_libdispatch_Ref/Reference/reference.html)
{% endcomment %}

### Projects

* [Grand Central Dispatch on FreeBSD](https://wiki.freebsd.org/GCD)
* [LLVM Compiler Runtime Library](http://compiler-rt.llvm.org/)
* [Clang C Compler](http://clang.llvm.org)
