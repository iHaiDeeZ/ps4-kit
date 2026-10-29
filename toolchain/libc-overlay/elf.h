/* ps4-kit (from rpcs3): FreeBSD's names for the aux vector types on top of the OpenOrbis (musl)
 * header. FreeBSD-targeting code (abseil) declares Elf32_auxv_t = Elf32_Auxinfo; musl already has
 * Elf32_auxv_t, so the FreeBSD names are the same types. */
#include_next <elf.h>
#ifndef _PS4KIT_ELF_H
#define _PS4KIT_ELF_H
typedef Elf32_auxv_t Elf32_Auxinfo;
typedef Elf64_auxv_t Elf64_Auxinfo;
#endif
