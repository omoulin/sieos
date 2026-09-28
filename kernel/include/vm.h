/*
 * vm.h - Process address spaces: mapped areas, copy-on-write, page faults.
 */
#ifndef SIEOS_VM_H
#define SIEOS_VM_H

#include "kernel.h"

struct proc;

struct inode;
struct shmseg;

/* An mmap region: anonymous or file memory, shared or private, or a System V segment. */
struct vm_area {
    uint64_t start, end;
    int prot;                    /* SIEOS_PROT_* */
    int flags;                   /* SIEOS_MAP_* */
    struct inode *ip;            /* file mapping: referenced */
    uint64_t off;                /* file offset of start */
    struct shmseg *shm;          /* shmat segment */
    struct vm_area *next;
};

uint64_t vm_space_copy(struct proc *child, struct proc *parent);   /* fork: new pml4 or 0 */
void     vm_space_free(struct proc *p, uint64_t pml4);
void     vm_exec_reset(struct proc *p);                            /* drop mmap areas at exec */
long     vm_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off);
long     vm_munmap(uint64_t addr, uint64_t len);
bool     vm_vmem_ok(struct proc *p, uint64_t n);    /* RLIMIT_VMEM allows n more bytes */
bool     vm_brk_ok(struct proc *p, uint64_t old, uint64_t new);
void     vm_add_area(struct proc *p, uint64_t start, uint64_t end, int prot, int flags);
long     vm_mprotect(uint64_t addr, uint64_t len, int prot);
long     vm_mincore(uint64_t addr, uint64_t len, char *vec);
long     vm_memcntl(uint64_t addr, uint64_t len, int cmd, uint64_t arg);
/* Page fault at addr: true if resolved (demand-zero page, copy-on-write). */
bool     vm_fault(uint64_t addr, uint64_t err, bool from_user);
long     vm_map_shm(struct shmseg *seg, uint64_t addr, uint64_t len, int prot, bool fixed);   /* shmat */
long     vm_unmap_shm(uint64_t addr);                                                        /* shmdt */

/* ipc.c: System V shared memory segments seen by the VM */
void     shm_attach_ref(struct shmseg *seg, int delta);
uint64_t shm_frame(struct shmseg *seg, uint64_t idx);

#endif
