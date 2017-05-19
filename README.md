# Grand Central Dispatch

Grand Central Dispatch (GCD or libdispatch) provides comprehensive support for concurrent code execution on multicore hardware.

libdispatch is currently available on all Darwin platforms. This project aims to make a modern version of libdispatch available on all other Swift platforms. To do this, we will implement as much of the portable subset of the API as possible, using the existing open source C implementation.

libdispatch on Darwin is a combination of logic in the `xnu` kernel alongside the user-space Library. The kernel has the most information available to balance workload across the entire system. As a first step, however, we believe it is useful to bring up the basic functionality of the library using user-space pthread primitives on Linux.  Eventually, a Linux kernel module could be developed to support more informed thread scheduling.

## Project Status

A port of libdispatch to Linux has been completed. On Linux, since Swift 3, swift-corelibs-libdispatch has been included in all Swift releases and is used by other swift-corelibs projects.

Opportunities to contribute and on-going work include:

1. Develop a test suite for the Swift APIs of libdispatch.
2. Enhance libdispatch as needed to support Swift language evolution and the needs of the other Core Libraries projects.

## Build and Install

For detailed instructions on building and installing libdispatch, see [INSTALL.md](INSTALL.md)

## Testing

For detailed instructions on testing libdispatch, see [TESTING.md](TESTING.md)
