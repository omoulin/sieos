/* SIEOS: Solaris errno values (from the ABI headers). */
@DEFINES errno.h:E[A-Z0-9]+@

/* Linux-only names some programs use: never returned by the kernel */
#define EDOTDOT         200
#define EHWPOISON       201
#define EISNAM          202
#define EKEYEXPIRED     203
#define EKEYREJECTED    204
#define EKEYREVOKED     205
#define EMEDIUMTYPE     206
#define ENAVAIL         207
#define ENOKEY          208
#define ENOMEDIUM       209
#define ENOTNAM         210
#define EREMOTEIO       211
#define ERFKILL         212
#define EUCLEAN         213
