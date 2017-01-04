# Grand Central Dispatch

Grand Central Dispatch (GCD or libdispatch) provides comprehensive support for concurrent code execution on multicore hardware.

libdispatch is currently available on all Darwin platforms. This project aims to make a modern version of libdispatch available on all other Swift platforms. To do this, we will implement as much of the portable subset of the API as possible, using the existing open source C implementation.

libdispatch on Darwin is a combination of logic in the `xnu` kernel alongside the user-space Library. The kernel has the most information available to balance workload across the entire system. As a first step, however, we believe it is useful to bring up the basic functionality of the library using user-space pthread primitives on Linux.  Eventually, a Linux kernel module could be developed to support more informed thread scheduling.

## Project Goals

We are currently early in the development of this project. We began with a mirror of the open source drop that corresponds with OS X El Capitan (10.11) and have ported it to x86_64 Ubuntu 14.04 and 15.10. The next steps are:

1. Complete the work to adopt libdispatch in other Core Libraries projects, especially Foundation. This will validate our work and get immediate test coverage on basic functionality of the Swift API.
2. Include libdispatch and libdispatch-enabled Core Libraries in the Swift CI environment and the pre-built Swift toolchains at Swift.org.
3. Develop a test suite for the Swift APIs of libdispatch.
4. Enhance libdispatch as needed to support Swift language evolution and the needs of the other Core Libraries projects.

## Build and Install

For detailed instructions on building and installing libdispatch, see [INSTALL.md](INSTALL.md)

## Testing

For detailed instructions on testing libdispatch, see [TESTING.md](TESTING.md)
