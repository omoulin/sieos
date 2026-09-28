/*
 * sieos/mount.h - mount(2) and umount2(2) flags (ABI v2), Solaris values.
 *
 *   mount(const char *spec, const char *dir, int mflag, const char *fstype,
 *         const char *dataptr, int datalen)
 *   umount2(const char *dir, int mflag)
 *
 * The file system types are "tmpfs" and "proc".  The mount table is
 * /proc/mnttab (also /etc/mnttab): one line per mount,
 *   special <TAB> mount point <TAB> type <TAB> options <TAB> mount time
 */
#ifndef SIEOS_ABI_MOUNT_H
#define SIEOS_ABI_MOUNT_H

#define SIEOS_MS_RDONLY    0x0001
#define SIEOS_MS_FSS       0x0002   /* old four-argument form (ignored) */
#define SIEOS_MS_DATA      0x0004   /* dataptr/datalen are given */
#define SIEOS_MS_REMOUNT   0x0008   /* change the flags of a mount */
#define SIEOS_MS_NOSUID    0x0010   /* set-id bits are ignored */
#define SIEOS_MS_OVERLAY   0x0080   /* mount over a non-empty directory */
#define SIEOS_MS_OPTIONSTR 0x0100
#define SIEOS_MS_FORCE     0x0400   /* umount2: accepted */

#endif
