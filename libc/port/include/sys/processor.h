#ifndef _SYS_PROCESSOR_H
#define _SYS_PROCESSOR_H
/* Solaris processor information and binding (processor_info, p_online, processor_bind). */
#ifdef __cplusplus
extern "C" {
#endif
#include <sys/types.h>
#include <sys/procset.h>

typedef int processorid_t;
typedef int psetid_t;

@DEFINES sysinfo.h:P_(OFFLINE|ONLINE|STATUS|NOINTR)@
@DEFINES sysinfo.h:PBIND_(NONE|QUERY)@
@DEFINES sysinfo.h:PI_(TYPELEN|FPUTYPE)@

typedef struct {
	int pi_state;
	char pi_processor_type[PI_TYPELEN];
	char pi_fputypes[PI_FPUTYPE];
	int pi_clock;
} processor_info_t;

int processor_info(processorid_t, processor_info_t *);
int p_online(processorid_t, int);
int processor_bind(idtype_t, id_t, processorid_t, processorid_t *);
processorid_t getcpuid(void);

#ifdef __cplusplus
}
#endif
#endif
