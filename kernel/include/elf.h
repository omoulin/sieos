/*
 * elf.h
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_ELF_H
#define SIEOS_ELF_H

#include <stdint.h>

#define ELF_MAGIC  0x464C457FU      /* "\x7FELF" little endian */
#define ELFCLASS64 2
#define ET_EXEC    2
#define ET_DYN     3              /* static-PIE when there is no PT_INTERP */
#define EM_X86_64  62
#define PT_LOAD    1
#define PT_INTERP  3
#define PT_PHDR    6
#define PF_X       1
#define PF_W       2
#define PF_R       4

typedef struct {
    uint32_t e_magic;
    uint8_t  e_class;
    uint8_t  e_data;
    uint8_t  e_version0;
    uint8_t  e_osabi;
    uint8_t  e_pad[8];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#endif
