/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "update.h"
#include "ble_update.h"
#include "firmware_update.h"
#include "tf.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int update_apply(void)
{
	size_t size;
	int rc = 0;

	/* Hard rule: always firmware.bin first, then ble.bin. */
	if (tf_file_exists(ALONZO_FW_PATH, &size)) {
		printk("update: %s found (%u bytes)\n", ALONZO_FW_PATH,
		       (unsigned int)size);
		rc = firmware_update();
		if (rc < 0) {
			printk("update: firmware failed (%d), keeping file\n", rc);
			return rc;
		}
	}

	if (tf_file_exists(ALONZO_BLE_PATH, &size)) {
		printk("update: %s found (%u bytes)\n", ALONZO_BLE_PATH,
		       (unsigned int)size);
		rc = ble_update();
		if (rc < 0) {
			printk("update: ble failed (%d), keeping file\n", rc);
			return rc;
		}
	}

	return 0;
}
