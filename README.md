# Grand Central Dispatch

Grand Central Dispatch (GCD or libdispatch) provides comprehensive support for concurrent code execution on multicore hardware.

libdispatch is currently available on all Darwin platforms. This project aims to make a modern version of libdispatch available on all other Swift platforms. To do this, we will implement as much of the portable subset of the API as possible, using the existing open source C implementation.

## Project Goals

We are currently very early in the development of this project. Our starting point is simply a mirror of the open source drop that corresponds with OS X El Capitan (10.11). Therefore, our earliest goals are:

0. Build and test the C source code as a dynamic library on the current Swift Linux targets (Ubuntu 14.04 and Ubuntu 15.10).
0. Add a `module.modulemap` and make the libdispatch API importable into Swift.
0. After the previous two steps are done, consider possible improvements to the interface of the libdispatch API in Swift.

## Building a C Library

libdispatch on Darwin is a combination of logic in the `xnu` kernel alongside the user-space Library. The kernel has the most information available to balance workload across the entire system. As a first step, however, we believe it is useful to bring up the basic functionality of the library using user-space pthread primitives on Linux.

Our first tasks for this project are:

0. Adapt the current autotools build system to work on Linux, or develop a new makefile or other build script for the project on Linux. The current version of the build system has only been tested on Darwin, though previous versions have been made to work on FreeBSD and Linux (see INSTALL).
0. Omit as much of the extra functionality of the library as possible, to get a core version of the project building. Much of the OS X-specific functionality can be elided completely on Linux.
0. Adopt libdispatch in other Core Libraries projects, especially Foundation. This will validate our work and get immediate coverage on basic functionality.
0. Incrementally add functionality back in.

Some C headers and sources (e.g. `Availability.h`, `Block.h`, and the libclosure `runtime.c`) are similar to ones embedded into the CoreFoundation part of [swift-corelibs-foundation](http://github.com/apple/swift-corelibs-foundation). We should figure out a mechanism to share these instead of duplicating them across projects.

## Toolchain
To add libdispatch to the toolchain in Linux, you need to add libdispatch and install-libdispatch lines to ./swift/utils/build-presets.ini under `[preset: buildbot_linux]` section. Also to make the build run faster, you can comment the test lines. After those updates the section would be as following:

```
[preset: buildbot_linux]
mixin-preset=mixin_linux_installation
build-subdir=buildbot_linux
lldb
release
#test
#validation-test
#long-test
libdispatch
foundation
lit-args=-v
dash-dash

install-libdispatch
install-foundation
reconfigure
```

After that run:

    utils/build-toolchain local.swift