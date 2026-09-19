/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

/*
 * Single source of truth for the Alonzo flash layout.
 *
 * Everything is derived from ALONZO_BOOTLOADER_SIZE (CONFIG_ALONZO_BOOTLOADER_SIZE)
 * so that no magic addresses (0x08020000, 0x08040000, ...) are scattered through
 * the source. The bootloader, the application and the linker all agree on these
 * values:
 *
 *   Bootloader:  CONFIG_FLASH_LOAD_OFFSET = 0
 *                CONFIG_FLASH_LOAD_SIZE   = ALONZO_BOOTLOADER_SIZE
 *   Application: CONFIG_FLASH_LOAD_OFFSET = ALONZO_BOOTLOADER_SIZE
 *
 * Build-time validation below rejects an invalid (non-sector-aligned) size.
 */

#ifndef ALONZO_MEMORY_LAYOUT_H
#define ALONZO_MEMORY_LAYOUT_H

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

/* STM32F411CEU6: 512 KB flash, 128 KB SRAM. */
#define ALONZO_FLASH_BASE   0x08000000u
#define ALONZO_FLASH_SIZE   (512u * 1024u)
#define ALONZO_FLASH_END    (ALONZO_FLASH_BASE + ALONZO_FLASH_SIZE)

#define ALONZO_SRAM_BASE    0x20000000u
#define ALONZO_SRAM_SIZE    (128u * 1024u)
#define ALONZO_SRAM_END     (ALONZO_SRAM_BASE + ALONZO_SRAM_SIZE)

/* Bootloader reserved size (configurable). */
#define ALONZO_BOOTLOADER_SIZE  CONFIG_ALONZO_BOOTLOADER_SIZE

/* Application region, immediately after the bootloader. */
#define ALONZO_APP_START    (ALONZO_FLASH_BASE + ALONZO_BOOTLOADER_SIZE)
#define ALONZO_APP_SIZE     (ALONZO_FLASH_SIZE - ALONZO_BOOTLOADER_SIZE)
#define ALONZO_APP_END      ALONZO_FLASH_END

/* Flash-API offsets are relative to the flash base. */
#define ALONZO_APP_OFFSET   (ALONZO_APP_START - ALONZO_FLASH_BASE)

/*
 * The STM32F411 flash is a single bank with sectors:
 *   S0..S3 = 16 KB, S4 = 64 KB, S5..S7 = 128 KB
 * A bootloader placed at FLASH_BASE is sector-aligned iff its size is one of
 * the cumulative sector boundaries below.
 */
#define _ALONZO_IS_SECTOR_ALIGNED(sz)					\
	((sz) == 0x04000u || (sz) == 0x08000u || (sz) == 0x0C000u ||	\
	 (sz) == 0x10000u || (sz) == 0x20000u || (sz) == 0x40000u ||	\
	 (sz) == 0x60000u || (sz) == 0x80000u)

BUILD_ASSERT(_ALONZO_IS_SECTOR_ALIGNED(ALONZO_BOOTLOADER_SIZE),
	     "ALONZO_BOOTLOADER_SIZE must be a sector-aligned size");

BUILD_ASSERT(ALONZO_BOOTLOADER_SIZE < ALONZO_FLASH_SIZE,
	     "bootloader size must be smaller than total flash");

/* The linker-enforced bootloader size must match the layout definition. */
BUILD_ASSERT(CONFIG_FLASH_LOAD_SIZE == ALONZO_BOOTLOADER_SIZE,
	     "CONFIG_FLASH_LOAD_SIZE must equal ALONZO_BOOTLOADER_SIZE");

#endif /* ALONZO_MEMORY_LAYOUT_H */
