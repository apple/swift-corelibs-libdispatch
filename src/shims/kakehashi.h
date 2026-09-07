// Copyright 2026 Kakehashi Project
// SPDX-License-Identifier: Apache-2.0

#ifndef DISPATCH_SHIMS_KAKEHASHI_H
#define DISPATCH_SHIMS_KAKEHASHI_H

#if !DISPATCH_KAKEHASHI
#error "the Kakehashi compatibility header is build-mode specific"
#endif

#include <TargetConditionals.h>
#include <mach/mach.h>
#include <os/availability.h>
#include <sched.h>

// Kakehashi needs Darwin ABI declarations and Mach-O output, but it does not
// provide Apple's private Darwin libdispatch platform SPI. Select the portable
// implementation paths independently of the compiler's Apple target triple.
#undef TARGET_OS_MAC
#define TARGET_OS_MAC 0

typedef struct dispatch_mach_msg_s *dispatch_mach_msg_t;

#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_ALL (1u << 8)
#define ULF_NO_ERRNO (1u << 24)

int __ulock_wait2(uint32_t operation, void *address, uint64_t value,
		uint64_t timeout_ns, uint64_t value2);
int __ulock_wake(uint32_t operation, void *address, uint64_t wake_value);

// The public macOS SDK does not describe bridgeOS. Treat those declarations as
// macOS declarations when compiling the Kakehashi guest ABI.
#ifndef __API_AVAILABLE_PLATFORM_bridgeos
#define __API_AVAILABLE_PLATFORM_bridgeos(version) bridgeos, introduced = version
#endif

#endif
