#ifndef _SYS_LOADAVG_H
#define _SYS_LOADAVG_H
#ifdef __cplusplus
extern "C" {
#endif
#define LOADAVG_1MIN  0
#define LOADAVG_5MIN  1
#define LOADAVG_15MIN 2
#define LOADAVG_NSTATS 3
int getloadavg(double [], int);
#ifdef __cplusplus
}
#endif
#endif
