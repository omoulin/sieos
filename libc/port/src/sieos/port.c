/*
 * port.c - Event ports, as Solaris's port_create(3C) family (sys/port.h):
 * the portfs system call (154).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <port.h>
#include "syscall.h"
#include "sieos/syscall.h"

int port_create(void)
{
	return syscall(SIEOS_SYS_portfs, PORT_CREATE);
}

int port_associate(int port, int source, uintptr_t object, int events, void *user)
{
	return syscall(SIEOS_SYS_portfs, PORT_ASSOCIATE, port, source, object, events, user);
}

int port_dissociate(int port, int source, uintptr_t object)
{
	return syscall(SIEOS_SYS_portfs, PORT_DISSOCIATE, port, source, object);
}

int port_send(int port, int events, void *user)
{
	return syscall(SIEOS_SYS_portfs, PORT_SEND, port, events, user);
}

int port_sendn(int ports[], int errors[], unsigned int nent, int events, void *user)
{
	return syscall(SIEOS_SYS_portfs, PORT_SENDN, ports, errors, nent, events, user);
}

int port_get(int port, port_event_t *pe, struct timespec *timeout)
{
	return syscall(SIEOS_SYS_portfs, PORT_GET, port, pe, timeout);
}

int port_getn(int port, port_event_t list[], unsigned int max, unsigned int *nget, struct timespec *timeout)
{
	return syscall(SIEOS_SYS_portfs, PORT_GETN, port, list, max, nget, timeout);
}

int port_alert(int port, int flags, int events, void *user)
{
	return syscall(SIEOS_SYS_portfs, PORT_ALERT, port, flags, events, user);
}
