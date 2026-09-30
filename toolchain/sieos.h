/* Target definitions for SIEOS (x86_64-pc-sieos): the kernel's ABI v2 with
   the musl-based C library.  Programs are linked dynamically against
   /lib/ld-musl-sieos64.so.1 (libc.so), or statically with -static / -static-pie.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
   This file is installed as gcc/config/sieos.h by toolchain/sieos-toolchain.py.  */

#undef  TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()		\
  do						\
    {						\
      builtin_define ("__sieos__");		\
      builtin_define_std ("unix");		\
      builtin_assert ("system=sieos");		\
      builtin_assert ("system=unix");		\
      builtin_assert ("system=posix");		\
    }						\
  while (false)

/* The C library is musl.  */
#undef  OPTION_MUSL
#define OPTION_MUSL 1
#undef  OPTION_MUSL_P
#define OPTION_MUSL_P(opts) 1

#define SIEOS_DYNAMIC_LINKER "/lib/ld-musl-sieos64.so.1"

/* Dynamic linking by default; -static, -static-pie and -pie as on other ELF systems.  */
#undef  STARTFILE_SPEC
#define STARTFILE_SPEC \
  "%{!shared:%{static-pie:rcrt1.o%s;pie:Scrt1.o%s;:crt1.o%s}} crti.o%s " \
  "%{static:crtbeginT.o%s;shared|static-pie|pie:crtbeginS.o%s;:crtbegin.o%s}"

#undef  ENDFILE_SPEC
#define ENDFILE_SPEC "%{static:crtend.o%s;shared|static-pie|pie:crtendS.o%s;:crtend.o%s} crtn.o%s"

#undef  LIB_SPEC
#define LIB_SPEC "%{pthread:} -lc"

#undef  LINK_SPEC
#define LINK_SPEC \
  "-z max-page-size=4096 %{!no-eh-frame-hdr:--eh-frame-hdr} " \
  "%{shared:-shared} %{static:-static} %{static-pie:-static -pie --no-dynamic-linker -z text} " \
  "%{rdynamic:-export-dynamic} " \
  "%{!shared:%{!static:%{!static-pie:-dynamic-linker " SIEOS_DYNAMIC_LINKER "}}}"

#undef  LINK_EH_SPEC
#define LINK_EH_SPEC ""

/* The whole C library is in libc.a: libstdc++ must not probe pthreads with weak symbols.  */
#define GTHREAD_USE_WEAK 0

#define TARGET_POSIX_IO

/* Mark objects as not needing an executable stack (the kernel enforces NX).  */
#undef  TARGET_ASM_FILE_END
#define TARGET_ASM_FILE_END file_end_indicate_exec_stack
