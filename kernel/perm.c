/*
 * perm.c - Unix discretionary access control.
 *
 * Classic owner/group/other permission bits checked against the effective
 * user and group IDs plus supplementary groups.  The superuser (euid 0)
 * bypasses read/write checks and may execute any file that has at least
 * one execute bit set.
 */
#include "fs.h"
#include "proc.h"

bool cred_in_group(struct proc *p, int gid)
{
    if (p->egid == gid)
        return true;
    for (int i = 0; i < p->ngroups; i++)
        if (p->groups[i] == gid)
            return true;
    return false;
}

int inode_permission(struct inode *ip, int mask)
{
    uint16_t mode = inode_mode(ip);
    if ((mask & W_OK) && inode_readonly(ip) && (S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode)))
        return -EROFS;
    if (current->euid == 0) {
        if ((mask & X_OK) && !S_ISDIR(mode) && !(mode & 0111))
            return -EACCES;
        return 0;
    }
    int bits;
    if (current->euid == inode_uid(ip))
        bits = (mode >> 6) & 7;
    else if (cred_in_group(current, inode_gid(ip)))
        bits = (mode >> 3) & 7;
    else
        bits = mode & 7;
    return (bits & mask) == mask ? 0 : -EACCES;
}

bool inode_owner_or_root(struct inode *ip)
{
    return current->euid == 0 || current->euid == inode_uid(ip);
}

/* May the caller remove 'victim' from 'dir'?  (write+search, sticky bit) */
int may_delete(struct inode *dir, struct inode *victim)
{
    int r = inode_permission(dir, W_OK | X_OK);
    if (r < 0)
        return r;
    if ((inode_mode(dir) & S_ISVTX) && current->euid != 0 &&
        current->euid != inode_uid(victim) && current->euid != inode_uid(dir))
        return -EPERM;
    return 0;
}
