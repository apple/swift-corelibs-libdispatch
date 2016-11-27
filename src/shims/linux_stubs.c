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

/*
 * This file contains stubbed out functions we are using during
 * the initial linux port.  When the port is complete, this file
 * should be empty (and thus removed).
 */

#include <stdint.h>
#ifdef __ANDROID__
#include <sys/syscall.h>
#else
#include <syscall.h>
#endif /* __ANDROID__ */

#if __has_include(<config/config_ac.h>)
#include <config/config_ac.h>
#else
#include <config/config.h>
#endif

#include "pthread.h"
#include "os/linux_base.h"
#include "internal.h"


#undef LINUX_PORT_ERROR
#define LINUX_PORT_ERROR()  do { printf("LINUX_PORT_ERROR_CALLED %s:%d: %s\n",__FILE__,__LINE__,__FUNCTION__); abort(); } while (0)


/*
 * Stubbed out static data
 */

pthread_key_t dispatch_voucher_key;
pthread_key_t dispatch_pthread_root_queue_observer_hooks_key;

unsigned short dispatch_timer__program_semaphore;
unsigned short dispatch_timer__wake_semaphore;
unsigned short dispatch_timer__fire_semaphore;
unsigned short dispatch_timer__configure_semaphore;
unsigned short dispatch_queue__pop_semaphore;
unsigned short dispatch_callout__entry_semaphore;
unsigned short dispatch_callout__return_semaphore;
unsigned short dispatch_queue__push_semaphore;
void (*_dispatch_block_special_invoke)(void*);
struct dispatch_queue_attr_s _dispatch_queue_attr_concurrent;
