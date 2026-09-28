/*
 * sieos/ipc.h - System V IPC (ABI v2): message queues, semaphores and
 * shared memory through the Solaris-style multiplexed calls msgsys,
 * semsys and shmsys.  Command values follow Solaris; the structure
 * layouts are SIEOS's own.
 */
#ifndef SIEOS_ABI_IPC_H
#define SIEOS_ABI_IPC_H

#include "types.h"

typedef int sieos_key_t;

#define SIEOS_IPC_PRIVATE ((sieos_key_t)0)

/* flags (with the permission bits in the low 9 bits) */
#define SIEOS_IPC_CREAT   0001000
#define SIEOS_IPC_EXCL    0002000
#define SIEOS_IPC_NOWAIT  0004000

/* control commands */
#define SIEOS_IPC_RMID    10
#define SIEOS_IPC_SET     11
#define SIEOS_IPC_STAT    12

struct sieos_ipc_perm {
    sieos_uid_t  uid, gid;             /* owner */
    sieos_uid_t  cuid, cgid;           /* creator */
    sieos_mode_t mode;                 /* permission bits */
    unsigned int seq;
    sieos_key_t  key;
    int __pad[5];
};

/* msgsys(op, ...) */
#define SIEOS_MSGGET 0                 /* msgsys(MSGGET, key, flags) */
#define SIEOS_MSGCTL 1                 /* msgsys(MSGCTL, id, cmd, msqid_ds *) */
#define SIEOS_MSGRCV 2                 /* msgsys(MSGRCV, id, msgp, size, type, flags) */
#define SIEOS_MSGSND 3                 /* msgsys(MSGSND, id, msgp, size, flags) */
#define SIEOS_MSG_NOERROR 010000

struct sieos_msqid_ds {
    struct sieos_ipc_perm msg_perm;
    unsigned long msg_cbytes;          /* bytes queued */
    unsigned long msg_qnum;            /* messages queued */
    unsigned long msg_qbytes;          /* maximum bytes */
    sieos_pid_t   msg_lspid, msg_lrpid;
    sieos_time_t  msg_stime, msg_rtime, msg_ctime;
    long __pad[3];
};

/* semsys(op, ...) */
#define SIEOS_SEMCTL     0             /* semsys(SEMCTL, id, num, cmd, arg) */
#define SIEOS_SEMGET     1             /* semsys(SEMGET, key, nsems, flags) */
#define SIEOS_SEMOP      2             /* semsys(SEMOP, id, sembuf *, nsops) */
#define SIEOS_SEMIDS     3             /* reserved */
#define SIEOS_SEMTIMEDOP 4             /* semsys(SEMTIMEDOP, id, sembuf *, nsops, const timespec *) */

/* semctl commands */
#define SIEOS_GETNCNT 3
#define SIEOS_GETPID  4
#define SIEOS_GETVAL  5
#define SIEOS_GETALL  6
#define SIEOS_GETZCNT 7
#define SIEOS_SETVAL  8
#define SIEOS_SETALL  9
#define SIEOS_SEM_UNDO 010000
#define SIEOS_SEMVMX  32767

struct sieos_semid_ds {
    struct sieos_ipc_perm sem_perm;
    unsigned short sem_nsems;
    short __pad1;
    int   __pad2;
    sieos_time_t sem_otime, sem_ctime;
    long __pad[3];
};

struct sieos_sembuf {
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
};

/* shmsys(op, ...) */
#define SIEOS_SHMAT  0                 /* shmsys(SHMAT, id, addr, flags) - returns the address */
#define SIEOS_SHMCTL 1                 /* shmsys(SHMCTL, id, cmd, shmid_ds *) */
#define SIEOS_SHMDT  2                 /* shmsys(SHMDT, addr) */
#define SIEOS_SHMGET 3                 /* shmsys(SHMGET, key, size, flags) */
#define SIEOS_SHM_RDONLY 010000
#define SIEOS_SHM_RND    020000
#define SIEOS_SHM_LOCK   3
#define SIEOS_SHM_UNLOCK 4
#define SIEOS_SHMLBA     4096

struct sieos_shmid_ds {
    struct sieos_ipc_perm shm_perm;
    sieos_size_t  shm_segsz;
    sieos_pid_t   shm_lpid, shm_cpid;
    unsigned long shm_nattch;
    sieos_time_t  shm_atime, shm_dtime, shm_ctime;
    long __pad[4];
};

SIEOS_STATIC_ASSERT(sizeof(struct sieos_ipc_perm) == 48, "ipc_perm size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_msqid_ds) == 128, "msqid_ds size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_semid_ds) == 96, "semid_ds size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_sembuf) == 6, "sembuf size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_shmid_ds) == 128, "shmid_ds size");

#endif
