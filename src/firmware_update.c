/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "firmware_update.h"
#include "led.h"
#include "memory_layout.h"
#include "tf.h"

#include <errno.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/*
 * Read/write chunk size. It is a multiple of the flash write block size (the
 * STM32F411 reports 1 byte, so any size is fine) and of the SD sector size
 * (512 bytes) for efficient TF reads.
 */
#define FW_CHUNK_SIZE 512u

/*
 * Blink cadence for the red LED during the write phase (ms). The flash API is
 * synchronous (the CPU is blocked inside flash_write), so per-chunk toggling
 * would be an invisible flicker; gate the toggle on this period instead so the
 * red LED shows a visible few-Hz blink.
 */
#define BURN_BLINK_PERIOD_MS 100

static const struct device *flash_device(void)
{
	return DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
}

/*
 * Toggle-only blink helper. It never decides the LED's final state: it only
 * flips RED while the burn is in progress. The final RED/GREEN state is
 * resolved once, by main(), after the update succeeds or fails.
 */
static void burn_blink(int64_t *last_toggle_ms)
{
	int64_t now = k_uptime_get();

	if ((now - *last_toggle_ms) >= BURN_BLINK_PERIOD_MS) {
		led_toggle(ALONZO_LED_RED);
		*last_toggle_ms = now;
	}
}

/* Erase [app_offset, app_offset + app_size) sector by sector. */
static int erase_app_region(const struct device *dev, uint32_t app_offset,
			    uint32_t app_size)
{
	off_t end = (off_t)app_offset + app_size;
	off_t off = (off_t)app_offset;

	while (off < end) {
		struct flash_pages_info info;
		int rc = flash_get_page_info_by_offs(dev, off, &info);

		if (rc != 0) {
			return rc;
		}
		rc = flash_erase(dev, info.start_offset, info.size);
		if (rc != 0) {
			return rc;
		}
		led_toggle(ALONZO_LED_RED); /* blink once per erased sector */
		off = info.start_offset + info.size;
	}

	return 0;
}

int firmware_update(void)
{
	const struct device *dev = flash_device();
	struct fs_file_t file;
	struct fs_dirent entry;
	uint32_t app_offset = ALONZO_APP_OFFSET;
	int rc;

	if (!device_is_ready(dev)) {
		printk("firmware_update: flash device not ready\n");
		return -ENODEV;
	}

	fs_file_t_init(&file);

	rc = fs_open(&file, ALONZO_FW_PATH, FS_O_READ);
	if (rc != 0) {
		printk("firmware_update: open %s failed (%d)\n", ALONZO_FW_PATH, rc);
		return rc;
	}

	/* Size / range validation: image must fit inside the app region. */
	rc = fs_stat(ALONZO_FW_PATH, &entry);
	if (rc != 0) {
		printk("firmware_update: stat failed (%d)\n", rc);
		goto out_close;
	}
	if (entry.size == 0 || entry.size > ALONZO_APP_SIZE) {
		printk("firmware_update: invalid size %u (max %u)\n",
		       (unsigned int)entry.size, (unsigned int)ALONZO_APP_SIZE);
		rc = -EFBIG;
		goto out_close;
	}

	printk("firmware_update: erasing app 0x%x..0x%x\n",
	       (unsigned int)app_offset, (unsigned int)(app_offset + ALONZO_APP_SIZE));

	/*
	 * Actual burn: red blinks while erasing (per sector) and writing. The
	 * set(true) here only starts the blink; it is not a final state. Final
	 * RED/GREEN is resolved by main() after update_apply() returns.
	 */
	led_set(ALONZO_LED_RED, true);
	rc = erase_app_region(dev, app_offset, ALONZO_APP_SIZE);
	if (rc != 0) {
		printk("firmware_update: erase failed (%d)\n", rc);
		goto out_close;
	}

	{
		static uint8_t buf[FW_CHUNK_SIZE];
		int64_t last_blink_ms = k_uptime_get();
		off_t off = (off_t)app_offset;
		ssize_t r;

		while ((r = fs_read(&file, buf, sizeof(buf))) > 0) {
			rc = flash_write(dev, off, buf, (size_t)r);
			if (rc != 0) {
				printk("firmware_update: write at 0x%x failed (%d)\n",
				       (unsigned int)off, rc);
				goto out_close;
			}
			off += (size_t)r;
			burn_blink(&last_blink_ms);
		}

		if (r < 0) {
			printk("firmware_update: read failed (%d)\n", (int)r);
			rc = (int)r;
			goto out_close;
		}
	}

	rc = fs_close(&file);
	if (rc != 0) {
		printk("firmware_update: close failed (%d)\n", rc);
		return rc;
	}

	/* Success: delete the update file. */
	rc = fs_unlink(ALONZO_FW_PATH);
	if (rc != 0) {
		printk("firmware_update: unlink failed (%d)\n", rc);
		return rc;
	}

	printk("firmware_update: success\n");
	return 0;

out_close:
	/* Failure: keep the file for the next boot; main() drives the final LED. */
	(void)fs_close(&file);
	return rc;
}
