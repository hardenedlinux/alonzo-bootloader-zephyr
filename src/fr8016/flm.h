/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * FLM runtime loader (Phase 4).
 *
 * Loads and runs a vendor .FLM flash algorithm on the FR8016HA Cortex-M3, on
 * top of the cortexm.h debug-control layer. The loader is *generic*: it knows
 * nothing about the FR8010H/FR8016H QSPI flash controller. The vendor .FLM raw
 * machine code is the single source of truth and executes verbatim on the
 * target -- this module never re-implements flash_write()/flash_erase()/
 * qspi_flash_init()/qspi_stig_cmd() or any equivalent.
 *
 * The .FLM is read from the TF card at runtime and parsed by flm_elf_parse()
 * (flm_elf.c), which fills `struct flm_image` and returns pointers into the raw
 * code/data blobs; flm_load() then copies those blobs verbatim into target RAM.
 * Swapping the .FLM on the card requires no rebuild.
 *
 * The .FLM is ARMCC ROPI + RWPI (BuildAttributes, no relocation sections), so a
 * plain copy + set-R9 load is exact: all read-only accesses are PC-relative and
 * all read-write accesses are R9-relative (offsets baked into the literal
 * pools). R9 (the static base) therefore = the runtime address of PrgData.
 *
 * Scope: this module only moves code/data into target RAM, sets R9/SP/PC and
 * arguments, halts/runs, waits for the core to halt again and reads R0. It does
 * NOT handle UART<->SWD mode switching, FR reset, flash file reads, or the
 * ble.bin update state machine -- those belong to fr8016.c (Phase 5).
 */

#ifndef ALONZO_FR8016_FLM_H
#define ALONZO_FR8016_FLM_H

#include <stddef.h>
#include <stdint.h>

/* Fixed-format image magic and version ("FLM1"). */
#define FLM_IMAGE_MAGIC    0x314d4c46u
#define FLM_IMAGE_VERSION  1u

/* Minimum stack the loader reserves for the FLM routines (bytes). */
#define FLM_REQUIRED_STACK_SIZE 512u

/*
 * Fixed-format image produced at runtime by flm_elf_parse() from a vendor .FLM.
 *
 * The entry fields are code-relative offsets into the code blob (symbol value
 * minus the PrgCode section VMA), with the Thumb bit set. At runtime the loader
 * executes them at `code_base + (entry & ~1)`. They describe *where* the FLM
 * routines live, never what they do. The flash geometry (flash_base/size/
 * sector_size/page_size/erase_value) is metadata read from the FlashDevice
 * descriptor -- it is what the host needs to drive the erase/program loop, not a
 * re-implementation of the flash algorithm.
 */
struct flm_image {
	uint32_t magic;
	uint32_t version;
	uint32_t code_size;                    /* PrgCode bytes */
	uint32_t data_size;                    /* PrgData bytes */
	uint32_t code_entry_offset;            /* Init() offset, Thumb bit set */
	uint32_t uninit_entry_offset;          /* UnInit() */
	uint32_t erase_chip_entry_offset;      /* EraseChip() (no-op in FR8010H) */
	uint32_t erase_sector_entry_offset;    /* EraseSector(addr) */
	uint32_t program_page_entry_offset;    /* ProgramPage(addr, size, buffer) */
	uint32_t required_stack_size;          /* minimum stack, bytes */
	uint32_t flash_base;                   /* FR flash base (0x01000000) */
	uint32_t flash_size;                   /* FR flash size (0x80000) */
	uint32_t sector_size;                  /* erase granularity (0x1000) */
	uint32_t page_size;                    /* program granularity (0x100) */
	uint32_t erase_value;                  /* erased byte (0xff) */
	uint32_t reserved[1];
};

/*
 * Target-RAM layout chosen by the caller (fr8016.c). All addresses are in the
 * FR8016HA memory space and must not overlap:
 *
 *   [code_base .. code_base + code_size)   PrgCode
 *   [data_base .. data_base + data_size)   PrgData  (R9 = data_base)
 *   [data_base + data_size .. +4)          BKPT stub (0xBE00)
 *   [stack_top - stack_size .. stack_top)  stack (grows down)
 *
 * fr8016.c also places a page buffer after the BKPT stub, before the stack.
 */
struct flm_runtime {
	uint32_t code_base;
	uint32_t data_base;   /* becomes R9 (static base) */
	uint32_t stack_top;   /* SP on entry */
};

/*
 * Parse a .FLM (ELF32, ARMCC ROPI/RWPI) already read into memory.
 *
 * @p elf / @p elf_len are the raw .FLM bytes read from the TF card. On success
 * the function fills @p img (magic/version, code/data sizes, entry offsets,
 * required stack, flash geometry) and sets @p *code_out / @p *data_out to point
 * into @p elf at the PrgCode / PrgData blobs -- so @p elf must stay valid until
 * flm_load() has copied them into target RAM. The algorithm bytes are never
 * modified. Returns 0 / -errno.
 */
int flm_elf_parse(const uint8_t *elf, size_t elf_len,
		  struct flm_image *img,
		  const uint8_t **code_out, const uint8_t **data_out);

/*
 * Load the image: copy code/data into target RAM, place the BKPT stub, halt the
 * core and record the entry points. The caller must already have brought up the
 * SWD line and DAP (swd_init + swd_line_reset + dap_init) and have the target
 * in debug state. Returns 0 / -errno.
 */
int flm_load(const struct flm_image *img, const uint8_t *code,
	     const uint8_t *data, const struct flm_runtime *rt);

/* Run the FLM entry points. Each returns 0 on success, -errno otherwise. */
int flm_init(void);
int flm_uninit(void);
int flm_erase_sector(uint32_t addr);
int flm_program_page(uint32_t addr, uint32_t size, uint32_t buffer);

/*
 * Static self-test: validate an image header (magic, sizes, entry offsets within
 * code bounds). Touches no target state, so it needs no hardware. Returns 0 /
 * -errno.
 */
int flm_selftest(const struct flm_image *img);

#endif /* ALONZO_FR8016_FLM_H */
