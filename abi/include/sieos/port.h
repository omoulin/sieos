/*
 * sieos/port.h - Event ports (Solaris's port_create(3C) family): one queue
 * where a thread collects events of several sources.  System call 154,
 * portfs(op, ...):
 *
 *   SIEOS_PORT_CREATE      ()                                  -> port descriptor
 *   SIEOS_PORT_ASSOCIATE   (port, source, object, events, user)
 *   SIEOS_PORT_DISSOCIATE  (port, source, object)
 *   SIEOS_PORT_SEND        (port, events, user)
 *   SIEOS_PORT_SENDN       (int *ports, int *errors, nent, events, user) -> how many sent
 *   SIEOS_PORT_GET         (port, sieos_port_event_t *, const sieos_timespec *timeout)
 *   SIEOS_PORT_GETN        (port, sieos_port_event_t *, max, unsigned *nget, const sieos_timespec *timeout)
 *   SIEOS_PORT_ALERT       (port, flags, events, user)
 *
 * Timeouts are relative; NULL waits forever; a zero timeout does not wait
 * (ETIME when nothing came).  port_getn waits for *nget events (at least 1,
 * at most max) and returns how many it got in *nget.
 *
 * Sources:
 *   PORT_SOURCE_FD    a descriptor's poll events (object: the descriptor),
 *                     reported once, then dissociated (associate again);
 *   PORT_SOURCE_FILE  File Event Notification (object: a sieos_file_obj *,
 *                     the path and the times the caller saw): FILE_ACCESS,
 *                     FILE_MODIFIED, FILE_ATTRIB, FILE_TRUNC, and always the
 *                     exceptions FILE_DELETE, FILE_RENAME_FROM/TO, UNMOUNTED,
 *                     MOUNTEDOVER; at once if the times differ from the
 *                     file's; reported once, then dissociated.  A directory
 *                     is modified when a name in it is added or removed;
 *   PORT_SOURCE_USER  port_send's events;
 *   PORT_SOURCE_TIMER a POSIX timer's expirations (SIGEV_PORT, signal.h);
 *   PORT_SOURCE_ALERT port_alert: every port_get returns it until cleared.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_ABI_PORT_H
#define SIEOS_ABI_PORT_H

#include "types.h"
#include "time.h"

#define SIEOS_PORT_CREATE     0
#define SIEOS_PORT_ASSOCIATE  1
#define SIEOS_PORT_DISSOCIATE 2
#define SIEOS_PORT_SEND       3
#define SIEOS_PORT_SENDN      4
#define SIEOS_PORT_GET        5
#define SIEOS_PORT_GETN       6
#define SIEOS_PORT_ALERT      7

#define SIEOS_PORT_SOURCE_AIO   1
#define SIEOS_PORT_SOURCE_TIMER 2
#define SIEOS_PORT_SOURCE_USER  3
#define SIEOS_PORT_SOURCE_FD    4
#define SIEOS_PORT_SOURCE_ALERT 5
#define SIEOS_PORT_SOURCE_MQ    6
#define SIEOS_PORT_SOURCE_FILE  7

#define SIEOS_PORT_ALERT_SET    0x01
#define SIEOS_PORT_ALERT_UPDATE 0x02

typedef struct sieos_port_event {
    int portev_events;                  /* the source's events */
    unsigned short portev_source;       /* SIEOS_PORT_SOURCE_* */
    unsigned short portev_pad;
    unsigned long portev_object;        /* the descriptor, the file_obj *, the timer ... */
    void *portev_user;                  /* the associate's (send's) user value */
} sieos_port_event_t;

/* PORT_SOURCE_FILE */
#define SIEOS_FILE_ACCESS      0x00000001
#define SIEOS_FILE_MODIFIED    0x00000002
#define SIEOS_FILE_ATTRIB      0x00000004
#define SIEOS_FILE_DELETE      0x00000010
#define SIEOS_FILE_RENAME_TO   0x00000020
#define SIEOS_FILE_RENAME_FROM 0x00000040
#define SIEOS_FILE_TRUNC       0x00100000
#define SIEOS_FILE_NOFOLLOW    0x10000000
#define SIEOS_UNMOUNTED        0x20000000
#define SIEOS_MOUNTEDOVER      0x40000000
#define SIEOS_FILE_EXCEPTION   (SIEOS_UNMOUNTED | SIEOS_FILE_DELETE | SIEOS_FILE_RENAME_TO | \
                                SIEOS_FILE_RENAME_FROM | SIEOS_MOUNTEDOVER)

struct sieos_file_obj {
    struct sieos_timespec fo_atime;
    struct sieos_timespec fo_mtime;
    struct sieos_timespec fo_ctime;
    unsigned long fo_pad[3];
    char *fo_name;                      /* the file's path */
};

/* SIEOS_SIGEV_PORT's sigev_value.sival_ptr (signal.h) */
typedef struct sieos_port_notify {
    int portnfy_port;
    void *portnfy_user;
} sieos_port_notify_t;

#endif
