#ifndef _SYS_SYSTEMINFO_H
#define _SYS_SYSTEMINFO_H
/*
 * Solaris sysinfo(command, buf, count).  The Linux-style sysinfo(struct
 * sysinfo *) of <sys/sysinfo.h> keeps the name in the library; this header
 * maps three-argument calls to __sysinfo_si.
 */
#ifdef __cplusplus
extern "C" {
#endif
@DEFINES sysinfo.h:SI_[A-Z0-9_]+@
long __sysinfo_si(int, char *, long);
#define sysinfo(cmd, buf, count) __sysinfo_si((cmd), (buf), (count))
#ifdef __cplusplus
}
#endif
#endif
