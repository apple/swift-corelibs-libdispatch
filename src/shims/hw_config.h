/*
 * Copyright (c) 2011 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

/*
 * IMPORTANT: This header file describes INTERNAL interfaces to libdispatch
 * which are subject to change in future releases of Mac OS X. Any applications
 * relying on these interfaces WILL break.
 */

#ifndef __DISPATCH_SHIMS_HW_CONFIG__
#define __DISPATCH_SHIMS_HW_CONFIG__

#if defined(__APPLE__)
#define DISPATCH_SYSCTL_LOGICAL_CPUS	"hw.logicalcpu_max"
#define DISPATCH_SYSCTL_PHYSICAL_CPUS	"hw.physicalcpu_max"
#define DISPATCH_SYSCTL_ACTIVE_CPUS		"hw.activecpu"
#elif defined(__FreeBSD__)
#define DISPATCH_SYSCTL_LOGICAL_CPUS	"kern.smp.cpus"
#define DISPATCH_SYSCTL_PHYSICAL_CPUS	"kern.smp.cpus"
#define DISPATCH_SYSCTL_ACTIVE_CPUS		"kern.smp.cpus"
#endif

static inline uint32_t
_dispatch_get_logicalcpu_max()
{
	uint32_t val = 1;
#if defined(_COMM_PAGE_LOGICAL_CPUS)
	uint8_t* u8val = (uint8_t*)(uintptr_t)_COMM_PAGE_LOGICAL_CPUS;
	val = (uint32_t)*u8val;
#elif defined(DISPATCH_SYSCTL_LOGICAL_CPUS)
	size_t valsz = sizeof(val);
	int ret = sysctlbyname(DISPATCH_SYSCTL_LOGICAL_CPUS,
			&val, &valsz, NULL, 0);
	(void)dispatch_assume_zero(ret);
	(void)dispatch_assume(valsz == sizeof(uint32_t));
#elif HAVE_SYSCONF && defined(_SC_NPROCESSORS_ONLN)
	int ret = (int)sysconf(_SC_NPROCESSORS_ONLN);
	val = ret < 0 ? 1 : ret;
#else
#warning "no supported way to query logical CPU count"
#endif
	return val;
}

static inline uint32_t
_dispatch_get_physicalcpu_max()
{
	uint32_t val = 1;
#if defined(_COMM_PAGE_PHYSICAL_CPUS)
	uint8_t* u8val = (uint8_t*)(uintptr_t)_COMM_PAGE_PHYSICAL_CPUS;
	val = (uint32_t)*u8val;
#elif defined(DISPATCH_SYSCTL_PHYSICAL_CPUS)
	size_t valsz = sizeof(val);
	int ret = sysctlbyname(DISPATCH_SYSCTL_LOGICAL_CPUS,
			&val, &valsz, NULL, 0);
	(void)dispatch_assume_zero(ret);
	(void)dispatch_assume(valsz == sizeof(uint32_t));
#elif HAVE_SYSCONF && defined(_SC_NPROCESSORS_ONLN)
	int ret = (int)sysconf(_SC_NPROCESSORS_ONLN);
	val = ret < 0 ? 1 : ret;
#else
#warning "no supported way to query physical CPU count"
#endif
	return val;
}

static inline uint32_t
_dispatch_get_activecpu()
{
	uint32_t val = 1;
#if defined(_COMM_PAGE_ACTIVE_CPUS)
	uint8_t* u8val = (uint8_t*)(uintptr_t)_COMM_PAGE_ACTIVE_CPUS;
	val = (uint32_t)*u8val;
#elif defined(DISPATCH_SYSCTL_ACTIVE_CPUS)
	size_t valsz = sizeof(val);
	int ret = sysctlbyname(DISPATCH_SYSCTL_ACTIVE_CPUS,
			&val, &valsz, NULL, 0);
	(void)dispatch_assume_zero(ret);
	(void)dispatch_assume(valsz == sizeof(uint32_t));
#elif HAVE_SYSCONF && defined(_SC_NPROCESSORS_ONLN)
	int ret = (int)sysconf(_SC_NPROCESSORS_ONLN);
	val = ret < 0 ? 1 : ret;
#else
#warning "no supported way to query active CPU count"
#endif
	return val;
}

#endif /* __DISPATCH_SHIMS_HW_CONFIG__ */
