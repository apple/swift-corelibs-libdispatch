// forward declarations for functions we are stubbing out
// in the intial linux port.

#ifndef __DISPATCH__STUBS__INTERNAL
#define __DISPATCH__STUBS__INTERNAL

int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
		 void *newp, size_t newlen);

mach_port_t pthread_mach_thread_np();

mach_port_t mach_task_self();

void mach_vm_deallocate(mach_port_t, mach_vm_address_t, mach_vm_size_t);

char* mach_error_string(mach_msg_return_t);
#endif


