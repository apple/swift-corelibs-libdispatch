/*
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
 * This file contains stubbed out functions we are using during
 * the initial linux port.  When the port is complete, this file
 * should be empty (and thus removed).
 */

#include <stdint.h>

#include <config/config.h>

#include "pthread.h"

#define program_invocation_short_name "hi"

#include "os/linux_base.h"
#include "internal.h"


#undef LINUX_PORT_ERROR
#define LINUX_PORT_ERROR()  do { printf("LINUX_PORT_ERROR_CALLED %s:%d: %s\n",__FILE__,__LINE__,__FUNCTION__); abort(); } while (0)

void _dispatch_mach_msg_dispose() { LINUX_PORT_ERROR();  }

unsigned long _dispatch_mach_probe(dispatch_mach_t dm) {
  LINUX_PORT_ERROR();
}

dispatch_block_t _dispatch_block_create(dispatch_block_flags_t flags,
					voucher_t voucher, pthread_priority_t priority,
					dispatch_block_t block) {
  LINUX_PORT_ERROR();
}

void _dispatch_mach_invoke() { LINUX_PORT_ERROR();  }

size_t _dispatch_mach_msg_debug(dispatch_mach_msg_t dmsg, char* buf, size_t bufsiz) {
  LINUX_PORT_ERROR();
}
void _dispatch_mach_dispose() { LINUX_PORT_ERROR();  }
void _dispatch_mach_msg_invoke() { LINUX_PORT_ERROR();  }

unsigned long _dispatch_runloop_queue_probe(dispatch_queue_t dq) {
  LINUX_PORT_ERROR();
}
void _dispatch_runloop_queue_xref_dispose() { LINUX_PORT_ERROR();  }

void strlcpy() { LINUX_PORT_ERROR();  }
void _dispatch_runloop_queue_dispose() { LINUX_PORT_ERROR();  }
char* mach_error_string(mach_msg_return_t x) {
  LINUX_PORT_ERROR();
}

void mach_vm_deallocate() { LINUX_PORT_ERROR();  }

mach_port_t pthread_mach_thread_np() {
  return (mach_port_t)pthread_self();
}

mach_port_t mach_task_self() {
  return (mach_port_t)pthread_self();
}

int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
		 void *newp, size_t newlen) {
  LINUX_PORT_ERROR();
}

#if 0

// this code remains here purely for debugging purposes
// ultimately it can be deleted

DISPATCH_NOINLINE
static const char *
_evfiltstr(short filt)
{
        switch (filt) {
#define _evfilt2(f) case (f): return #f
        _evfilt2(EVFILT_READ);
        _evfilt2(EVFILT_WRITE);
        _evfilt2(EVFILT_AIO);
        _evfilt2(EVFILT_VNODE);
        _evfilt2(EVFILT_PROC);
        _evfilt2(EVFILT_SIGNAL);
        _evfilt2(EVFILT_TIMER);
#if HAVE_MACH
        _evfilt2(EVFILT_MACHPORT);
        _evfilt2(DISPATCH_EVFILT_MACH_NOTIFICATION);
#endif
        _evfilt2(EVFILT_FS);
        _evfilt2(EVFILT_USER);
#ifdef EVFILT_VM
        _evfilt2(EVFILT_VM);
#endif
#ifdef EVFILT_SOCK
        _evfilt2(EVFILT_SOCK);
#endif
#ifdef EVFILT_MEMORYSTATUS
        _evfilt2(EVFILT_MEMORYSTATUS);
#endif

        _evfilt2(DISPATCH_EVFILT_TIMER);
        _evfilt2(DISPATCH_EVFILT_CUSTOM_ADD);
        _evfilt2(DISPATCH_EVFILT_CUSTOM_OR);
        default:
                return "EVFILT_missing";
        }
}

#if 0
#define dbg_kevent64(fmt...)               do { printf(fmt); } while(0)
#define dbg_cond_kevent64(cond,fmt...)     do { if (cond) printf(fmt); } while(0)
#else
#define dbg_kevent64(fmt...)     	   do { } while(0)
#define dbg_cond_kevent64(cond,fmt...)     do { } while(0)
#endif


int kevent64(int kq, const struct kevent64_s *changelist_c, int nchanges, struct kevent64_s *eventlist,
             int nevents, unsigned int flags, const struct timespec *timeout) 
{
     // Documentation is not really clear. Instrument the code to make sure
     // we can do type conversions right now between kevent64 <-> kevent, where as 
     // kevent64 uses the ext[2] extension. So far we only see these used in the EVFILT_TIMER.
     // right now we do this in the way into kevent, we also have to assert that 
     // no more than 1 change or one event is passed until we get a better handle of the
     // usage pattern of this.  (Hubertus Franke)

     struct kevent64_s *changelist = (struct kevent64_s*) changelist_c; // so we can modify it

#if 1 
     // lets put some checks in here to make sure we do it all correct
     // we can only convert kevent64_s -> kevent for a single entry since kevent64_s has ext[0:1] extension
     if ((nchanges > 1) || (nevents > 1)) 
 	 LINUX_PORT_ERROR();
     if (nchanges) { 
	 dbg_kevent64("kevent64(%s,%x,%x): cl.ext[0,1]=%lx:%ld %lx:%ld cl.data=%lx:%ld\n", 
	              _evfiltstr(changelist->filter), changelist->flags, changelist->fflags,
		      changelist->ext[0], changelist->ext[0], 
                      changelist->ext[1], changelist->ext[1],
                      changelist->data, changelist->data);
         if ((changelist->filter == EVFILT_TIMER) && (changelist->fflags & NOTE_ABSOLUTE)) {
             // NOTE_ABSOLUTE is not recognized by the current kevent we need to convert this
	     // into a relative. Consider fiddling with creating relative events instead (didn't work 
	     // on first attempt). We also ignore the LEEWAY. Finally we must convert from 
             // NSECS to MSECS (might have to expand to deal with OTHER NOTE_xSECS flags

             //changelist->data -= _dispatch_get_nanoseconds();
             //changelist->data -= time(NULL) * NSEC_PER_SEC;
             dbg_kevent64("kevent64(%s,%x) data=%lx:%ld\n",
                          _evfiltstr(changelist->filter),changelist->fflags,
			  changelist->data,changelist->data);
	     //changelist->data /= 1000000UL;
	     //if ((int64_t)(changelist->data) <= 0) changelist->data = 1; // for some reason time turns negative
         }
     }
#endif
     // eventlist can not return more than 1 event type coersion doesn't work
     int rc = kevent(kq,(struct kevent*) changelist,nchanges,(struct kevent*) eventlist,nevents,timeout);
     if (rc > 1) 
         LINUX_PORT_ERROR();
     return rc;
}

#endif

/*
 * Stubbed out static data
 */

pthread_key_t dispatch_voucher_key;
pthread_key_t dispatch_pthread_root_queue_observer_hooks_key;

unsigned short dispatch_timer__program_semaphore;
unsigned short dispatch_timer__wake_semaphore;
unsigned short dispatch_timer__fire_semaphore;
unsigned short dispatch_timer__configure_semaphore;
void (*_dispatch_block_special_invoke)(void*);
struct dispatch_queue_attr_s _dispatch_queue_attr_concurrent;
