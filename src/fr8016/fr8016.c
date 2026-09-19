/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "fr8016.h"
#include "cortexm.h"
#include "dap.h"
#include "flm.h"
#include "led.h"
#include "swd.h"
#include "tf.h"

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/*
 * FR8016HA flash programming (Phase 5).
 *
 * The STM32 programs the FR8016HA over SWD by running the vendor .FLM flash
 * algorithm in the FR8016HA's own RAM. The .FLM is NOT built into the
 * bootloader: it is read from the TF card at runtime (alongside ble.bin) and
 * parsed by flm_elf_parse(); only its raw code/data bytes are copied, verbatim,
 * into FR RAM and executed there. The STM32 is only:
 *
 *   * an SWD master (swd.c) on PA2/PA3 (USART2 in normal operation),
 *   * a Cortex-M debug-register driver (cortexm.c) to halt/run/reset the core
 *     and move its registers,
 *   * a MEM-AP memory-transfer host (dap.c) to copy code/data/buffers in and to
 *     read flash back for verification, and
 *   * a FLM runtime host (flm.c) that calls Init/EraseSector/ProgramPage/UnInit.
 *
 * It never touches the FR8016HA QSPI flash controller directly, and it never
 * re-implements any part of the flash algorithm.
 */

/* USART2 (PA2 = TX, PA3 = RX) borrows its pins for SWD (SWCLK/SWDIO). */
#define FR_USART2_NODE DT_NODELABEL(usart2)

/* One ProgramPage payload, in bytes. Matches page_size of the FR8010H.FLM. */
#define FR_PAGE_BUF_SIZE 256u

/* Upper bound on a .FLM ELF we accept from the TF card (generic cap). */
#define FR_FLM_MAX_SIZE 65536u

/* Work-area layout inside the FR8016HA SRAM window (see fr_work_layout()). */
struct fr_work {
	struct flm_runtime rt;  /* code_base, data_base, stack_top */
	uint32_t page_buf;      /* FR RAM staging buffer for ProgramPage */
};

/* Holds the .FLM ELF for the duration of parse + flm_load(). */
static uint8_t fr_flm_buf[FR_FLM_MAX_SIZE];

static uint32_t align4(uint32_t a)
{
	return (a + 3u) & ~3u;
}

/*
 * Lay out the FLM work area inside the configured FR8016HA SRAM window:
 *
 *   [code_base .. code_base + code_size)   PrgCode
 *   [data_base .. data_base + data_size)   PrgData   (R9 = data_base)
 *   [data_base + data_size .. +4)          BKPT stub
 *   [page_buf .. page_buf + page_size)     ProgramPage staging buffer
 *   [stack_top - stack .. stack_top)       stack (grows down)
 */
static int fr_work_layout(const struct flm_image *img, struct fr_work *w)
{
	uint32_t base = CONFIG_ALONZO_FR8016_RAM_BASE;
	uint32_t size = CONFIG_ALONZO_FR8016_RAM_SIZE;
	uint32_t code_base = base;
	uint32_t data_base = align4(code_base + img->code_size);
	uint32_t page_buf = align4(data_base + img->data_size + 4u); /* +4: BKPT */
	uint32_t stack_top = base + size;

	/* Guard the subtraction below against a malformed image header. */
	if (img->required_stack_size > size) {
		return -ENOSPC;
	}

	/* The page buffer and the FLM stack must both fit below stack_top. */
	if (page_buf + FR_PAGE_BUF_SIZE > stack_top - img->required_stack_size) {
		return -ENOSPC;
	}

	w->rt.code_base = code_base;
	w->rt.data_base = data_base;
	w->rt.stack_top = stack_top;
	w->page_buf = page_buf;
	return 0;
}

/*
 * Enter SWD mode: reconfigure PA2/PA3 from USART2 (AF) to GPIO. This takes the
 * pins away from USART2, which is the whole "disable USART2" step: the USART2
 * peripheral is left clocked but idle (nothing drives it), and its pins are now
 * GPIOs for the bit-banged SWD. Nothing writes to USART2 in the bootloader.
 */
static int fr_enter_swd(void)
{
	return swd_init();
}

/*
 * Restore USART2 by re-applying its default pinctrl state (PA2 = TX, PA3 = RX).
 * USART2 was configured at boot and never torn down, so re-muxing the pins is
 * enough to put it back in service.
 */
static int fr_restore_usart2(void)
{
#if DT_NODE_HAS_STATUS(FR_USART2_NODE, okay)
	/*
	 * Re-apply USART2's default pinctrl state (PA2 = TX, PA3 = RX). The
	 * pinctrl config is instantiated by the UART driver; CONFIG_PINCTRL_NON_STATIC
	 * makes it visible here so this module can borrow and restore the pins.
	 */
	PINCTRL_DT_DEV_CONFIG_DECLARE(FR_USART2_NODE);
	int rc = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(FR_USART2_NODE),
				     PINCTRL_STATE_DEFAULT);

	if (rc != 0) {
		printk("fr8016: restore usart2 failed (%d)\n", rc);
		return rc;
	}
#endif
	return 0;
}

/*
 * Attach: full SWD line reset, power up the DAP and halt the FR8016HA core.
 */
static int fr_attach(void)
{
	int rc;

	rc = swd_line_reset();
	if (rc != 0) {
		return rc;
	}
	rc = dap_init();
	if (rc != 0) {
		return rc;
	}
	return cm_halt();
}

/* Copy @p len bytes to FR RAM at @p dst as 32-bit MEM-AP writes. */
static int fr_copy_to_ram(uint32_t dst, const uint8_t *src, uint32_t len)
{
	uint32_t aligned = align4(len);
	uint32_t word;
	int rc;

	if ((dst & 3u) != 0u) {
		return -EINVAL;
	}
	/* src is zero-padded to a word boundary by the caller. */
	for (uint32_t i = 0; i < aligned; i += 4u) {
		memcpy(&word, &src[i], 4u);
		rc = mem_write32(dst + i, word);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

/* Read the whole file at @p path into @p buf (cap @p cap bytes). */
static int fr_read_file(const char *path, uint8_t *buf, size_t cap, size_t *len)
{
	struct fs_file_t file;
	struct fs_dirent entry;
	size_t want;
	size_t done = 0;
	ssize_t r;
	int rc;

	rc = fs_stat(path, &entry);
	if (rc != 0) {
		return rc;
	}
	if (entry.size == 0 || entry.size > cap) {
		return -EFBIG;
	}
	want = (size_t)entry.size;

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc != 0) {
		return rc;
	}
	while (done < want) {
		r = fs_read(&file, buf + done, want - done);
		if (r <= 0) {
			(void)fs_close(&file);
			return (r < 0) ? (int)r : -EIO;
		}
		done += (size_t)r;
	}
	(void)fs_close(&file);
	*len = done;
	return 0;
}

/* Read the .FLM from the TF card and parse it into @p img + code/data ptrs. */
static int fr_load_flm(const char *flm_path, struct flm_image *img,
		       const uint8_t **code, const uint8_t **data)
{
	size_t len;
	int rc;

	rc = fr_read_file(flm_path, fr_flm_buf, sizeof(fr_flm_buf), &len);
	if (rc != 0) {
		printk("fr8016: read %s failed (%d)\n", flm_path, rc);
		return rc;
	}
	rc = flm_elf_parse(fr_flm_buf, len, img, code, data);
	if (rc != 0) {
		printk("fr8016: parse %s failed (%d)\n", flm_path, rc);
		return rc;
	}
	printk("fr8016: FLM code=%u data=%u flash=0x%x/%u sector=%u page=%u\n",
	       img->code_size, img->data_size, img->flash_base, img->flash_size,
	       img->sector_size, img->page_size);
	return 0;
}

/* Erase the sectors covering [flash_base, flash_base + image_size). */
static int fr_erase(const struct flm_image *img, uint32_t flash_base,
		    uint32_t image_size)
{
	uint32_t sector = img->sector_size;
	uint32_t n = (image_size + sector - 1u) / sector;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t addr = flash_base + i * sector;
		int rc = flm_erase_sector(addr);

		if (rc != 0) {
			printk("fr8016: erase sector 0x%x failed (%d)\n", addr, rc);
			return rc;
		}
		led_toggle(ALONZO_LED_RED); /* blink once per erased sector */
	}
	return 0;
}

/* Program the image in page_size chunks, streaming from @p file. */
static int fr_program(const struct flm_image *img, struct fs_file_t *file,
		      uint32_t flash_base, uint32_t image_size, uint32_t page_buf)
{
	uint8_t buf[FR_PAGE_BUF_SIZE];
	uint32_t offset = 0;
	uint32_t page = img->page_size;

	while (offset < image_size) {
		uint32_t chunk = image_size - offset;
		ssize_t r;

		if (chunk > page) {
			chunk = page;
		}

		r = fs_read(file, buf, chunk);
		if (r != (ssize_t)chunk) {
			printk("fr8016: read at 0x%x failed (%d)\n",
			       offset, (int)r);
			return (r < 0) ? (int)r : -EIO;
		}

		/* Zero the pad so word reads in fr_copy_to_ram stay in bounds. */
		memset(&buf[chunk], 0, sizeof(buf) - chunk);

		int rc = fr_copy_to_ram(page_buf, buf, chunk);

		if (rc != 0) {
			return rc;
		}
		rc = flm_program_page(flash_base + offset, chunk, page_buf);
		if (rc != 0) {
			printk("fr8016: program page 0x%x failed (%d)\n",
			       flash_base + offset, rc);
			return rc;
		}
		offset += chunk;
	}
	return 0;
}

/*
 * Verify by reading flash back through the MEM-AP (the flash is XIP-mapped at
 * flash_base) and comparing word-for-word against the source file. This checks
 * what was actually written, not just that ProgramPage returned success.
 */
static int fr_verify(struct fs_file_t *file, uint32_t flash_base,
		     uint32_t image_size)
{
	uint8_t buf[FR_PAGE_BUF_SIZE];
	uint32_t offset = 0;
	int rc;

	rc = fs_seek(file, 0, FS_SEEK_SET);
	if (rc != 0) {
		printk("fr8016: verify seek failed (%d)\n", rc);
		return rc;
	}

	while (offset < image_size) {
		uint32_t chunk = image_size - offset;
		ssize_t r;

		if (chunk > FR_PAGE_BUF_SIZE) {
			chunk = FR_PAGE_BUF_SIZE;
		}

		r = fs_read(file, buf, chunk);
		if (r != (ssize_t)chunk) {
			printk("fr8016: verify read at 0x%x failed (%d)\n",
			       offset, (int)r);
			return (r < 0) ? (int)r : -EIO;
		}
		memset(&buf[chunk], 0, sizeof(buf) - chunk);

		for (uint32_t i = 0; i < chunk; i += 4u) {
			uint32_t n = (chunk - i >= 4u) ? 4u : (chunk - i);
			uint32_t expected = 0u;
			uint32_t actual;
			uint32_t mask;

			memcpy(&expected, &buf[i], n);
			rc = mem_read32(flash_base + offset + i, &actual);
			if (rc != 0) {
				printk("fr8016: verify read 0x%x failed (%d)\n",
				       flash_base + offset + i, rc);
				return rc;
			}
			/* Mask the possibly-partial tail word. */
			mask = (n == 4u) ? 0xffffffffu : ((1u << (8u * n)) - 1u);
			if ((actual & mask) != (expected & mask)) {
				printk("fr8016: verify mismatch at 0x%x: "
				       "wrote 0x%08x read 0x%08x\n",
				       flash_base + offset + i, expected, actual);
				return -EIO;
			}
		}
		offset += chunk;
	}
	return 0;
}

int fr8016_update(const char *ble_path, const char *flm_path)
{
	struct flm_image img;
	const uint8_t *code = NULL;
	const uint8_t *data = NULL;
	struct fs_file_t file;
	struct fs_dirent entry;
	struct fr_work work;
	uint32_t image_size;
	uint32_t flash_base;
	int rc;

	fs_file_t_init(&file);

	/* 1. Read and parse the external .FLM (before touching USART2). */
	rc = fr_load_flm(flm_path, &img, &code, &data);
	if (rc != 0) {
		return rc;
	}

	/* 2. Disable USART2: PA2/PA3 become SWD GPIOs. */
	rc = fr_enter_swd();
	if (rc != 0) {
		printk("fr8016: swd_init failed (%d)\n", rc);
		return rc;
	}

	/* 3. SWD attach: line reset -> DAP -> halt the FR8016HA. */
	rc = fr_attach();
	if (rc != 0) {
		printk("fr8016: attach failed (%d)\n", rc);
		goto out_restore;
	}

	/* 4. Select the FR RAM work area and load the FLM. */
	rc = fr_work_layout(&img, &work);
	if (rc != 0) {
		printk("fr8016: FR RAM window too small (%d)\n", rc);
		goto out_restore;
	}
	rc = flm_load(&img, code, data, &work.rt);
	if (rc != 0) {
		printk("fr8016: flm_load failed (%d)\n", rc);
		goto out_restore;
	}

	/* 5. Open the image and validate its size against the FR flash. */
	rc = fs_open(&file, ble_path, FS_O_READ);
	if (rc != 0) {
		printk("fr8016: open %s failed (%d)\n", ble_path, rc);
		goto out_restore;
	}
	rc = fs_stat(ble_path, &entry);
	if (rc != 0) {
		printk("fr8016: stat failed (%d)\n", rc);
		goto out_close;
	}
	if (entry.size == 0 || entry.size > img.flash_size) {
		printk("fr8016: invalid size %u (max %u)\n",
		       (unsigned int)entry.size, (unsigned int)img.flash_size);
		rc = -EFBIG;
		goto out_close;
	}
	image_size = (uint32_t)entry.size;
	flash_base = img.flash_base;

	/* 6. Init the flash algorithm. */
	rc = flm_init();
	if (rc != 0) {
		printk("fr8016: flm_init failed (%d)\n", rc);
		goto out_close;
	}

	/* 7. Erase the sectors covering the image. */
	rc = fr_erase(&img, flash_base, image_size);
	if (rc != 0) {
		goto out_uninit;
	}

	/* 8. Program the image in page_size chunks. */
	rc = fr_program(&img, &file, flash_base, image_size, work.page_buf);
	if (rc != 0) {
		goto out_uninit;
	}

	/* 9. Verify the written image by reading flash back. */
	rc = fr_verify(&file, flash_base, image_size);
	if (rc != 0) {
		goto out_uninit;
	}

	/* 10. Uninit the flash algorithm. */
	rc = flm_uninit();
	if (rc != 0) {
		printk("fr8016: flm_uninit failed (%d)\n", rc);
		goto out_close;
	}

	/* 11. Reset-and-run the FR8016HA on its new firmware. */
	rc = cm_reset();
	if (rc != 0) {
		printk("fr8016: reset failed (%d)\n", rc);
		goto out_close;
	}
	rc = cm_run();
	if (rc != 0) {
		printk("fr8016: run failed (%d)\n", rc);
		goto out_close;
	}

	(void)fs_close(&file);
	(void)fr_restore_usart2();

	/* 12. Success cleanup: both files must be deleted. */
	rc = fs_unlink(ble_path);
	if (rc != 0) {
		printk("fr8016: unlink %s failed (%d)\n", ble_path, rc);
		return rc; /* keep the .FLM too so the pair retries next boot */
	}
	rc = fs_unlink(flm_path);
	if (rc != 0) {
		printk("fr8016: unlink %s failed (%d)\n", flm_path, rc);
		return rc; /* ble.bin is gone; the .FLM is now orphaned (no retry) */
	}

	printk("fr8016: success\n");
	return 0;

out_uninit:
	(void)flm_uninit();
out_close:
	(void)fs_close(&file);
out_restore:
	(void)fr_restore_usart2();
	return rc;
}

int fr8016_flm_selftest(const char *flm_path)
{
	struct flm_image img;
	const uint8_t *code = NULL;
	const uint8_t *data = NULL;
	int rc;

	if (!tf_file_exists(flm_path, NULL)) {
		printk("fr8016_flm_selftest: %s missing, skipping\n", flm_path);
		return 0;
	}
	rc = fr_load_flm(flm_path, &img, &code, &data);
	if (rc != 0) {
		return rc;
	}
	return flm_selftest(&img);
}
