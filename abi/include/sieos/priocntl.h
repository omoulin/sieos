/*
 * sieos/priocntl.h - scheduling classes and priocntl(2) (ABI v2), as Solaris.
 *
 *   priocntl(idtype, id, cmd, arg)       idtype: P_PID, P_LWPID (sieos/wait.h), P_MYID ids
 *
 * The dispatcher runs the runnable LWP of highest global priority, round
 * robin among equals:
 *   SYS  60-99    kernel (reserved)
 *   RT  100-159   real time: fixed priorities 0-59, a time quantum
 *   TS    0-59    time sharing: priority falls when a quantum is used up and
 *                 rises after sleeping; the user priority (upri, -60..60,
 *                 nice) shifts it
 *   FX    0-60    fixed priority (user priority), a time quantum
 * New LWPs inherit the class of their creator; init runs in TS.
 */
#ifndef SIEOS_ABI_PRIOCNTL_H
#define SIEOS_ABI_PRIOCNTL_H

#include "types.h"

typedef short sieos_pri_t;

#define SIEOS_PC_GETCID      0      /* pcinfo_t: pc_clname -> pc_cid, pc_clinfo */
#define SIEOS_PC_GETCLINFO   1      /* pcinfo_t: pc_cid -> pc_clname, pc_clinfo */
#define SIEOS_PC_SETPARMS    2      /* pcparms_t */
#define SIEOS_PC_GETPARMS    3      /* pcparms_t (pc_cid PC_CLNULL: the target's class) */
#define SIEOS_PC_GETPRIRANGE 5      /* pcpri_t */
#define SIEOS_PC_DONICE      6      /* pcnice_t */

#define SIEOS_PC_CLNULL      (-1)
#define SIEOS_PC_CLNMSZ      16
#define SIEOS_PC_CLINFOSZ    8
#define SIEOS_PC_CLPARMSZ    8

/* class ids */
#define SIEOS_CID_SYS 0
#define SIEOS_CID_TS  1
#define SIEOS_CID_FX  2
#define SIEOS_CID_RT  3

typedef struct {
    sieos_id_t pc_cid;
    char pc_clname[SIEOS_PC_CLNMSZ];
    int pc_clinfo[SIEOS_PC_CLINFOSZ];
} sieos_pcinfo_t;

typedef struct {
    sieos_id_t pc_cid;
    int pc_clparms[SIEOS_PC_CLPARMSZ];
} sieos_pcparms_t;

typedef struct {
    sieos_id_t pc_cid;
    sieos_pri_t pc_clpmax, pc_clpmin;
} sieos_pcpri_t;

#define SIEOS_PC_GETNICE 0
#define SIEOS_PC_SETNICE 1
typedef struct {
    int pc_val;                     /* nice value, -20..19 */
    int pc_op;                      /* SIEOS_PC_GETNICE / SIEOS_PC_SETNICE */
} sieos_pcnice_t;

/* class-specific parameters, overlaid on pc_clparms and pc_clinfo */
typedef struct { sieos_pri_t ts_uprilim, ts_upri; } sieos_tsparms_t;
typedef struct { sieos_pri_t ts_maxupri; } sieos_tsinfo_t;
typedef struct { sieos_pri_t rt_pri; unsigned int rt_tqsecs; int rt_tqnsecs; } sieos_rtparms_t;
typedef struct { sieos_pri_t rt_maxpri; } sieos_rtinfo_t;
typedef struct { sieos_pri_t fx_uprilim, fx_upri; unsigned int fx_tqsecs; int fx_tqnsecs; } sieos_fxparms_t;
typedef struct { sieos_pri_t fx_maxupri; } sieos_fxinfo_t;

#define SIEOS_TS_MAXUPRI  60
#define SIEOS_TS_NOCHANGE (-32768)
#define SIEOS_RT_MAXPRI   59
#define SIEOS_RT_NOCHANGE (-1)       /* rt_pri, or rt_tqnsecs: keep the value */
#define SIEOS_RT_TQDEF    (-2)       /* rt_tqnsecs: the default quantum */
#define SIEOS_RT_TQINF    (-3)       /* rt_tqnsecs: no quantum */
#define SIEOS_FX_MAXUPRI  60
#define SIEOS_FX_NOCHANGE (-32768)
#define SIEOS_FX_TQDEF    (-2)
#define SIEOS_FX_TQINF    (-3)

SIEOS_STATIC_ASSERT(sizeof(sieos_pcinfo_t) == 52, "pcinfo size");
SIEOS_STATIC_ASSERT(sizeof(sieos_pcparms_t) == 36, "pcparms size");

#endif
