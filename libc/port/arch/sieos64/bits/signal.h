/* SIEOS: Solaris signal numbers and the kernel's amd64 ucontext layout. */
#if defined(_POSIX_SOURCE) || defined(_POSIX_C_SOURCE) \
 || defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)

#if defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
#define MINSIGSTKSZ 2048
#define SIGSTKSZ 8192
#endif

#ifdef _GNU_SOURCE
enum { REG_R15 = 0, REG_R14, REG_R13, REG_R12, REG_R11, REG_R10, REG_R9, REG_R8,
       REG_RDI, REG_RSI, REG_RBP, REG_RBX, REG_RDX, REG_RCX, REG_RAX, REG_TRAPNO,
       REG_ERR, REG_RIP, REG_CS, REG_RFL, REG_RSP, REG_SS, REG_FS, REG_GS, REG_ES,
       REG_DS, REG_FSBASE, REG_GSBASE };
#define REG_R15 REG_R15
#define REG_R14 REG_R14
#define REG_R13 REG_R13
#define REG_R12 REG_R12
#define REG_R11 REG_R11
#define REG_R10 REG_R10
#define REG_R9 REG_R9
#define REG_R8 REG_R8
#define REG_RDI REG_RDI
#define REG_RSI REG_RSI
#define REG_RBP REG_RBP
#define REG_RBX REG_RBX
#define REG_RDX REG_RDX
#define REG_RCX REG_RCX
#define REG_RAX REG_RAX
#define REG_TRAPNO REG_TRAPNO
#define REG_ERR REG_ERR
#define REG_RIP REG_RIP
#define REG_CS REG_CS
#define REG_RFL REG_RFL
#define REG_EFL REG_RFL
#define REG_RSP REG_RSP
#define REG_SS REG_SS
#define REG_FS REG_FS
#define REG_GS REG_GS
#define REG_ES REG_ES
#define REG_DS REG_DS
#define REG_FSBASE REG_FSBASE
#define REG_GSBASE REG_GSBASE
#endif

#if defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
#define NGREG 28
typedef long greg_t, gregset_t[28];
typedef struct __attribute__((aligned(16))) {
	struct {
		unsigned short cw, sw;
		unsigned char fctw, __fx_rsvd;
		unsigned short fop;
		unsigned long rip, rdp;
		unsigned mxcsr, mxcsr_mask;
		unsigned char st[8][16];
		unsigned char xmm[16][16];
		unsigned char __fx_ign2[6][16];
		unsigned status, xstatus;
	} fpchip_state;
} fpregset_t;
typedef struct {
	gregset_t gregs;
	fpregset_t fpregs;
} mcontext_t;
#else
typedef struct __attribute__((aligned(16))) {
	unsigned long __space[94];
} mcontext_t;
#endif

struct sigaltstack {
	void *ss_sp;
	size_t ss_size;
	int ss_flags;
	int __pad;
};

typedef struct __attribute__((aligned(16))) __ucontext {
	unsigned long uc_flags;
	struct __ucontext *uc_link;
	sigset_t uc_sigmask;
	stack_t uc_stack;
	mcontext_t uc_mcontext;
	long __uc_filler[5];
} ucontext_t;

#define UC_SIGMASK 0x01
#define UC_STACK   0x02
#define UC_CPU     0x04
#define UC_FPU     0x08
#define UC_ALL     0x0f

#define SA_ONSTACK   0x00000001
#define SA_RESETHAND 0x00000002
#define SA_RESTART   0x00000004
#define SA_SIGINFO   0x00000008
#define SA_NODEFER   0x00000010
#define SA_NOCLDWAIT 0x00010000
#define SA_NOCLDSTOP 0x00020000
#define SA_RESTORER  0

#define __SI_SWAP_ERRNO_CODE

#endif

@DEFINES SIG(HUP|INT|QUIT|ILL|TRAP|ABRT|IOT|EMT|FPE|KILL|BUS|SEGV|SYS|PIPE|ALRM|TERM|USR1|USR2|CHLD|CLD|PWR|WINCH|URG|POLL|IO|STOP|TSTP|CONT|TTIN|TTOU|VTALRM|PROF|XCPU|XFSZ|WAITING|LWP|FREEZE|THAW|LOST|XRES|JVM1|JVM2|INFO)@
#define SIGUNUSED SIGSYS

#define _NSIG 129
