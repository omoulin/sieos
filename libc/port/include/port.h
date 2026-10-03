#ifndef _PORT_H
#define _PORT_H
/* Event ports (Solaris's port_create(3C) family). */
#include <sys/port.h>
#include <stdint.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
int port_create(void);
int port_associate(int, int, uintptr_t, int, void *);
int port_dissociate(int, int, uintptr_t);
int port_send(int, int, void *);
int port_sendn(int [], int [], unsigned int, int, void *);
int port_get(int, port_event_t *, struct timespec *);
int port_getn(int, port_event_t [], unsigned int, unsigned int *, struct timespec *);
int port_alert(int, int, int, void *);
#ifdef __cplusplus
}
#endif
#endif
