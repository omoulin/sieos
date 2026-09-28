#ifndef _SYS_UADMIN_H
#define _SYS_UADMIN_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
@DEFINES sysinfo.h:A_(REBOOT|SHUTDOWN|REMOUNT)@
@DEFINES sysinfo.h:AD_(HALT|BOOT|IBOOT|POWEROFF)@
int uadmin(int, int, uintptr_t);
#ifdef __cplusplus
}
#endif
#endif
