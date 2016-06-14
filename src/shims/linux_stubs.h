/*
 * This source file is part of the Swift.org open source project
 *
 * Copyright (c) 2015 Apple Inc. and the Swift project authors
 *
 * Licensed under Apache License v2.0 with Runtime Library Exception
 *
 * See http://swift.org/LICENSE.txt for license information
 * See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
 *
 */

// forward declarations for functions we are stubbing out
// in the intial linux port.

#ifndef __DISPATCH__STUBS__INTERNAL
#define __DISPATCH__STUBS__INTERNAL

mach_port_t pthread_mach_thread_np();

mach_port_t mach_task_self();

void mach_vm_deallocate(mach_port_t, mach_vm_address_t, mach_vm_size_t);

char* mach_error_string(mach_msg_return_t);
#endif
