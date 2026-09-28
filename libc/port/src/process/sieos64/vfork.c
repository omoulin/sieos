#define _GNU_SOURCE
#include <unistd.h>

/* vfork is fork: the child's address space is a copy-on-write copy. */
pid_t vfork(void)
{
	return fork();
}
