/* Free the stack of an exiting detached thread, then end the LWP. */
#include "sieos/syscall.h"
.text
.global __unmapself
.type   __unmapself,@function
__unmapself:
	mov $SIEOS_SYS_munmap,%eax
	syscall
	mov $SIEOS_SYS_lwp_exit,%eax
	syscall
	hlt
