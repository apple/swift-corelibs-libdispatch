// Stub out Apple internal header file to redirect to linux_base.h.
// Maybe not the best fix in the long run, but avoids having to #ifdef
// the include of <os/base.h> in all of the other files....

#ifndef __OS_BASE__
#define __OS_BASE__

#include <os/linux_base.h>

#endif
