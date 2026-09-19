/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "flm.h"
#include "cortexm.h"
#include "dap.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

/* BKPT #0 instruction (Thumb). The FLM routine returns into this and halts. */
#define FLM_BKPT_HALFWORD 0xBE00u
/* 32-bit word placed at the BKPT stub (upper halfword is never executed). */
#define FLM_BKPT_WORD     0x0000BE00u

/* Bound for waiting on a FLM routine to halt. Erase/program take up to ~10 s. */
#define FLM_CALL_TIMEOUT_MS 20000u
#define FLM_CALL_POLL_US    1000u

/* Loaded state, filled by flm_load(). */
static const struct flm_image *flm_img;
static struct flm_runtime flm_rt;
static uint32_t flm_bkpt;

static uint32_t align4(uint32_t a)
{
	return (a + 3u) & ~3u;
}

/*
 * Copy @p len bytes to target RAM at @p dst via 32-bit MEM-AP writes. The FLM
 * blobs are 4-byte aligned (ELF section alignment), so word writes are exact.
 */
static int flm_copy_to_ram(uint32_t dst, const uint8_t *src, uint32_t len)
{
	uint32_t word;
	int rc;

	if ((dst & 3u) != 0u || (len & 3u) != 0u) {
		return -EINVAL;
	}
	for (uint32_t i = 0; i < len; i += 4u) {
		memcpy(&word, &src[i], 4u);
		rc = mem_write32(dst + i, word);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

int flm_load(const struct flm_image *img, const uint8_t *code,
	     const uint8_t *data, const struct flm_runtime *rt)
{
	uint32_t bkpt;
	int rc;

	if (img == NULL || code == NULL || data == NULL || rt == NULL) {
		return -EINVAL;
	}
	if (img->magic != FLM_IMAGE_MAGIC || img->version != FLM_IMAGE_VERSION) {
		return -EINVAL;
	}
	if (img->code_size == 0u || img->data_size == 0u) {
		return -EINVAL;
	}

	/* Layout: code, then data, then the BKPT stub -- all before the stack. */
	bkpt = align4(rt->data_base + img->data_size);
	if (rt->code_base + img->code_size > rt->data_base) {
		return -EINVAL; /* code and data would overlap */
	}
	if (rt->stack_top < bkpt + 4u + img->required_stack_size) {
		return -EINVAL; /* stack region would overlap code/data/BKPT */
	}

	/* Bring up the debug domain if not already done (idempotent). */
	rc = dap_init();
	if (rc != 0) {
		return rc;
	}

	rc = flm_copy_to_ram(rt->code_base, code, img->code_size);
	if (rc != 0) {
		return rc;
	}
	rc = flm_copy_to_ram(rt->data_base, data, img->data_size);
	if (rc != 0) {
		return rc;
	}
	rc = mem_write32(bkpt, FLM_BKPT_WORD);
	if (rc != 0) {
		return rc;
	}

	/* Halt the target before the first call so register setup is stable. */
	rc = cm_halt();
	if (rc != 0) {
		return rc;
	}

	flm_img = img;
	flm_rt = *rt;
	flm_bkpt = bkpt;
	return 0;
}

/*
 * Execute one FLM entry point with up to four arguments and return its R0.
 * The routine returns into the BKPT stub (LR), which halts the core; we wait
 * for that halt and read the result.
 */
static int flm_call(uint32_t entry, uint32_t r0, uint32_t r1, uint32_t r2,
		    uint32_t r3, uint32_t *ret)
{
	int rc;

	if (flm_img == NULL) {
		return -ENXIO;
	}

	rc = cm_write_reg(CM_REG_R9, flm_rt.data_base);
	if (rc != 0) {
		return rc;
	}
	rc = cm_write_sp(flm_rt.stack_top);
	if (rc != 0) {
		return rc;
	}
	rc = cm_write_reg(CM_REG_LR, flm_bkpt | 1u); /* Thumb return */
	if (rc != 0) {
		return rc;
	}
	rc = cm_write_reg(CM_REG_R0, r0);
	if (rc != 0) {
		return rc;
	}
	rc = cm_write_reg(CM_REG_R1, r1);
	if (rc != 0) {
		return rc;
	}
	rc = cm_write_reg(CM_REG_R2, r2);
	if (rc != 0) {
		return rc;
	}
	rc = cm_write_reg(CM_REG_R3, r3);
	if (rc != 0) {
		return rc;
	}
	/* entry is a code-relative offset with the Thumb bit set; the PC points at
	 * code_base + offset (the Thumb bit is cleared: M-profile ignores PC LSB). */
	rc = cm_write_pc(flm_rt.code_base + (entry & ~1u));
	if (rc != 0) {
		return rc;
	}

	/* Resume and wait for the BKPT halt (bounded; erase/program are slow). */
	rc = cm_resume();
	if (rc != 0) {
		return rc;
	}
	for (uint32_t i = 0; i < (FLM_CALL_TIMEOUT_MS * 1000u) / FLM_CALL_POLL_US;
	     i++) {
		bool halted = false;

		rc = cm_is_halted(&halted);
		if (rc != 0) {
			return rc;
		}
		if (halted) {
			if (ret != NULL) {
				rc = cm_read_reg(CM_REG_R0, ret);
				if (rc != 0) {
					return rc;
				}
			}
			return 0;
		}
		k_busy_wait(FLM_CALL_POLL_US);
	}

	return -ETIMEDOUT;
}

/* FLM entry points return 0 on success; map non-zero to -EIO. */
static int flm_run(uint32_t entry, uint32_t r0, uint32_t r1, uint32_t r2,
		   uint32_t r3)
{
	uint32_t ret = 0;
	int rc;

	rc = flm_call(entry, r0, r1, r2, r3, &ret);
	if (rc != 0) {
		return rc;
	}
	if (ret != 0u) {
		printk("flm: entry 0x%x returned 0x%x\n", entry, ret);
		return -EIO;
	}
	return 0;
}

int flm_init(void)
{
	if (flm_img == NULL) {
		return -ENXIO;
	}
	return flm_run(flm_img->code_entry_offset, 0, 0, 0, 0);
}

int flm_uninit(void)
{
	if (flm_img == NULL) {
		return -ENXIO;
	}
	return flm_run(flm_img->uninit_entry_offset, 0, 0, 0, 0);
}

int flm_erase_sector(uint32_t addr)
{
	if (flm_img == NULL) {
		return -ENXIO;
	}
	return flm_run(flm_img->erase_sector_entry_offset, addr, 0, 0, 0);
}

int flm_program_page(uint32_t addr, uint32_t size, uint32_t buffer)
{
	if (flm_img == NULL) {
		return -ENXIO;
	}
	return flm_run(flm_img->program_page_entry_offset, addr, size, buffer, 0);
}

int flm_selftest(const struct flm_image *img)
{
	if (img == NULL) {
		return -EINVAL;
	}
	if (img->magic != FLM_IMAGE_MAGIC || img->version != FLM_IMAGE_VERSION) {
		printk("flm_selftest: bad magic/version\n");
		return -EINVAL;
	}
	if (img->code_size == 0u || img->data_size == 0u) {
		printk("flm_selftest: empty code/data\n");
		return -EINVAL;
	}
	/* Entry offsets must lie within the code blob (Thumb bit ignored). */
	const uint32_t entries[] = {
		img->code_entry_offset, img->uninit_entry_offset,
		img->erase_chip_entry_offset, img->erase_sector_entry_offset,
		img->program_page_entry_offset,
	};
	for (uint32_t i = 0; i < ARRAY_SIZE(entries); i++) {
		if ((entries[i] & ~1u) >= img->code_size) {
			printk("flm_selftest: entry %u out of bounds\n", i);
			return -EINVAL;
		}
	}

	printk("flm_selftest: code=%u data=%u flash=0x%x/%u sector=%u page=%u\n",
	       img->code_size, img->data_size, img->flash_base, img->flash_size,
	       img->sector_size, img->page_size);
	printk("flm_selftest: Init=0x%x UnInit=0x%x EraseChip=0x%x "
	       "EraseSector=0x%x ProgramPage=0x%x\n",
	       img->code_entry_offset, img->uninit_entry_offset,
	       img->erase_chip_entry_offset, img->erase_sector_entry_offset,
	       img->program_page_entry_offset);
	printk("flm_selftest: PASS\n");
	return 0;
}
