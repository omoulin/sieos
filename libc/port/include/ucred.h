#ifndef _UCRED_H
#define _UCRED_H
/* Credentials of a process or of a socket's peer (Solaris's ucred_get(3C)). */
#include <sys/types.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ucred_s ucred_t;

ucred_t *ucred_get(pid_t);
void ucred_free(ucred_t *);
size_t ucred_size(void);
int getpeerucred(int, ucred_t **);
uid_t ucred_geteuid(const ucred_t *);
uid_t ucred_getruid(const ucred_t *);
uid_t ucred_getsuid(const ucred_t *);
gid_t ucred_getegid(const ucred_t *);
gid_t ucred_getrgid(const ucred_t *);
gid_t ucred_getsgid(const ucred_t *);
int ucred_getgroups(const ucred_t *, const gid_t **);
pid_t ucred_getpid(const ucred_t *);
int ucred_getzoneid(const ucred_t *);
int ucred_getprojid(const ucred_t *);
#ifdef __cplusplus
}
#endif
#endif
