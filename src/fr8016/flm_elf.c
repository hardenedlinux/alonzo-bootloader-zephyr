/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

/*
 * Runtime .FLM (ELF32) parser.
 *
 * A .FLM is an ARMCC-produced ELF32 (little-endian, ROPI + RWPI, no relocation
 * sections). This module parses an external .FLM read straight from the TF card
 * so the bootloader can load and run the vendor flash algorithm on the target.
 * It only *reads* the ELF structure -- it never modifies a byte of the flash
 * algorithm and never re-implements it.
 *
 * It extracts exactly what flm_load() needs:
 *   * the PrgCode / PrgData blob pointers (into the caller's buffer) and sizes
 *   * the Init / UnInit / EraseChip / EraseSector / ProgramPage entry offsets
 *   * the flash geometry from the FlashDevice ("DevDscr") descriptor
 */

#include "flm.h"

#include <errno.h>
#include <string.h>

/* ELF32 little-endian constants. */
#define ELF_MAG0 0x7fu
#define ELF_MAG1 'E'
#define ELF_MAG2 'L'
#define ELF_MAG3 'F'
#define ELFCLASS32 1u
#define ELFDATA2LSB 1u

/* Elf32_Ehdr field offsets. */
#define EHDR_CLASS     4u
#define EHDR_DATA      5u
#define EHDR_SHOFF     0x20u
#define EHDR_SHENTSIZE 0x2eu
#define EHDR_SHNUM     0x30u
#define EHDR_SHSTRNDX  0x32u
#define EHDR_SIZE      52u

/* Elf32_Shdr field offsets (little-endian). */
#define SHDR_NAME   0x00u
#define SHDR_ADDR   0x0cu
#define SHDR_OFFSET 0x10u
#define SHDR_SIZE   0x14u
#define SHDR_BYTES  40u

/* Elf32_Sym field offsets (little-endian). */
#define SYM_NAME  0x00u
#define SYM_VALUE 0x04u
#define SYM_INFO  0x0cu
#define SYM_BYTES 16u

#define STB_GLOBAL 1u

/* FlashDevice descriptor (DevDscr section) field offsets (ARM FlashOS.h). */
#define DEV_ADDR_OFF     0x84u /* flash base address   */
#define DEV_SIZE_OFF     0x88u /* flash size           */
#define DEV_PAGE_OFF     0x8cu /* program page size    */
#define DEV_EMPTY_OFF    0x94u /* erased byte value    */
#define DEV_SECTOR_OFF   0xa0u /* first sector size    */
#define DEV_SECTADDR_OFF 0xa4u /* first sector address */

struct flm_sec {
	uint32_t offset;
	uint32_t size;
	uint32_t addr;
};

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Resolve the section named @p want. On success fills @p out and returns 0;
 * returns -ENOENT if absent, -EINVAL on a malformed header.
 */
static int sec_get(const uint8_t *elf, size_t elf_len, uint32_t e_shoff,
		   uint32_t e_shentsize, uint32_t e_shnum, uint32_t e_shstrndx,
		   const char *want, struct flm_sec *out)
{
	uint32_t shstr_off;
	uint32_t shstr_size;
	uint32_t i;

	if (e_shstrndx >= e_shnum) {
		return -EINVAL;
	}
	{
		const uint8_t *sh = elf + e_shoff + e_shstrndx * e_shentsize;

		shstr_off = rd32(sh + SHDR_OFFSET);
		shstr_size = rd32(sh + SHDR_SIZE);
		if (shstr_off + shstr_size > elf_len) {
			return -EINVAL;
		}
	}

	for (i = 0; i < e_shnum; i++) {
		const uint8_t *sh = elf + e_shoff + i * e_shentsize;
		uint32_t name_off = rd32(sh + SHDR_NAME);
		const char *name;

		if (name_off >= shstr_size) {
			continue;
		}
		name = (const char *)(elf + shstr_off + name_off);
		if (strcmp(name, want) == 0) {
			out->offset = rd32(sh + SHDR_OFFSET);
			out->size = rd32(sh + SHDR_SIZE);
			out->addr = rd32(sh + SHDR_ADDR);
			if (out->offset + out->size > elf_len) {
				return -EINVAL;
			}
			return 0;
		}
	}
	return -ENOENT;
}

/* Look up the value of the global symbol @p name. Returns 0 / -ENOENT. */
static int sym_value(const uint8_t *elf, const struct flm_sec *symtab,
		     const struct flm_sec *strtab, const char *name,
		     uint32_t *value)
{
	uint32_t n = symtab->size / SYM_BYTES;
	uint32_t i;

	for (i = 0; i < n; i++) {
		const uint8_t *sym = elf + symtab->offset + i * SYM_BYTES;
		uint32_t name_off = rd32(sym + SYM_NAME);
		uint8_t info = sym[SYM_INFO];
		const char *sname;

		if ((info >> 4) != STB_GLOBAL) {
			continue;
		}
		if (name_off >= strtab->size) {
			continue;
		}
		sname = (const char *)(elf + strtab->offset + name_off);
		if (strcmp(sname, name) == 0) {
			*value = rd32(sym + SYM_VALUE);
			return 0;
		}
	}
	return -ENOENT;
}

int flm_elf_parse(const uint8_t *elf, size_t elf_len, struct flm_image *img,
		  const uint8_t **code_out, const uint8_t **data_out)
{
	static const char *const names[5] = {
		"Init", "UnInit", "EraseChip", "EraseSector", "ProgramPage",
	};
	struct flm_sec code, data, devdscr, symtab, strtab;
	uint32_t e_shoff, e_shentsize, e_shnum, e_shstrndx;
	uint32_t syms[5];
	uint32_t devadr, szdev, szpage, szsector, sectaddr;
	uint8_t valempty;
	uint32_t i;
	int rc;

	if (img == NULL || code_out == NULL || data_out == NULL) {
		return -EINVAL;
	}
	if (elf == NULL || elf_len < EHDR_SIZE) {
		return -EINVAL;
	}
	if (elf[0] != ELF_MAG0 || elf[1] != ELF_MAG1 ||
	    elf[2] != ELF_MAG2 || elf[3] != ELF_MAG3) {
		return -EINVAL;
	}
	if (elf[EHDR_CLASS] != ELFCLASS32 || elf[EHDR_DATA] != ELFDATA2LSB) {
		return -EINVAL;
	}

	e_shoff = rd32(elf + EHDR_SHOFF);
	e_shentsize = rd16(elf + EHDR_SHENTSIZE);
	e_shnum = rd16(elf + EHDR_SHNUM);
	e_shstrndx = rd16(elf + EHDR_SHSTRNDX);

	if (e_shentsize != SHDR_BYTES || e_shnum == 0 ||
	    e_shoff + (uint32_t)e_shnum * e_shentsize > elf_len) {
		return -EINVAL;
	}

	rc = sec_get(elf, elf_len, e_shoff, e_shentsize, e_shnum, e_shstrndx,
		     "PrgCode", &code);
	if (rc != 0) {
		return rc;
	}
	rc = sec_get(elf, elf_len, e_shoff, e_shentsize, e_shnum, e_shstrndx,
		     "PrgData", &data);
	if (rc != 0) {
		return rc;
	}
	rc = sec_get(elf, elf_len, e_shoff, e_shentsize, e_shnum, e_shstrndx,
		     "DevDscr", &devdscr);
	if (rc != 0) {
		return rc;
	}
	if (devdscr.size < DEV_SECTADDR_OFF + 4u) {
		return -EINVAL;
	}
	rc = sec_get(elf, elf_len, e_shoff, e_shentsize, e_shnum, e_shstrndx,
		     ".symtab", &symtab);
	if (rc != 0) {
		return rc;
	}
	rc = sec_get(elf, elf_len, e_shoff, e_shentsize, e_shnum, e_shstrndx,
		     ".strtab", &strtab);
	if (rc != 0) {
		return rc;
	}

	for (i = 0; i < 5; i++) {
		rc = sym_value(elf, &symtab, &strtab, names[i], &syms[i]);
		if (rc != 0) {
			return rc;
		}
		/* Every entry function is Thumb: the LSB of its value is set. */
		if ((syms[i] & 1u) == 0u) {
			return -EINVAL;
		}
	}

	/* Flash geometry from the FlashDevice descriptor (DevDscr). */
	devadr = rd32(elf + devdscr.offset + DEV_ADDR_OFF);
	szdev = rd32(elf + devdscr.offset + DEV_SIZE_OFF);
	szpage = rd32(elf + devdscr.offset + DEV_PAGE_OFF);
	valempty = elf[devdscr.offset + DEV_EMPTY_OFF];
	szsector = rd32(elf + devdscr.offset + DEV_SECTOR_OFF);
	sectaddr = rd32(elf + devdscr.offset + DEV_SECTADDR_OFF);
	if (sectaddr != 0u) {
		return -ENOTSUP; /* non-uniform sector map unsupported */
	}

	memset(img, 0, sizeof(*img));
	img->magic = FLM_IMAGE_MAGIC;
	img->version = FLM_IMAGE_VERSION;
	img->code_size = code.size;
	img->data_size = data.size;
	/* Entry offsets are code-relative (symbol value minus PrgCode VMA); the
	 * Thumb bit is preserved. */
	img->code_entry_offset = syms[0] - code.addr;
	img->uninit_entry_offset = syms[1] - code.addr;
	img->erase_chip_entry_offset = syms[2] - code.addr;
	img->erase_sector_entry_offset = syms[3] - code.addr;
	img->program_page_entry_offset = syms[4] - code.addr;
	img->required_stack_size = FLM_REQUIRED_STACK_SIZE;
	img->flash_base = devadr;
	img->flash_size = szdev;
	img->sector_size = szsector;
	img->page_size = szpage;
	img->erase_value = valempty;

	if (img->code_size == 0u || img->data_size == 0u) {
		return -EINVAL;
	}

	*code_out = elf + code.offset;
	*data_out = elf + data.offset;
	return 0;
}
