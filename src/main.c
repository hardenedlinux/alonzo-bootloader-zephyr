/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "app_jump.h"
#include "led.h"
#include "memory_layout.h"
#include "tf.h"
#include "update.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static void error_loop(const char *reason)
{
	led_all_off();
	led_set(ALONZO_LED_RED, true);
	printk("bootloader: %s\n", reason);
	for (;;) {
		k_sleep(K_SECONDS(1));
	}
}

int main(void)
{
	int rc;

	printk("Alonzo Maker Bootloader\n");

	led_init();
	led_all_off();

	/*
	 * Mount the TF card. If it is absent/unreadable we still boot the
	 * existing application: a missing SD card must not brick the device.
	 */
	rc = tf_mount();
	if (rc == 0) {
		rc = update_apply();
	} else {
		printk("bootloader: TF mount failed (%d), skipping update\n", rc);
		rc = 0;
	}

	/*
	 * Final LED state is resolved here, exactly once, from the update result:
	 *   failure -> red on, green off   (error_loop)
	 *   success -> green on, red off   (below)
	 * The burn helpers only blink red while flashing; they never decide the
	 * final state.
	 */
	if (rc < 0) {
		error_loop("update failed");
	}

	led_all_off();
	led_set(ALONZO_LED_GREEN, true);

	if (!app_image_valid(ALONZO_APP_START)) {
		error_loop("no valid application");
	}

	printk("bootloader: jumping to application at 0x%x\n",
	       (unsigned int)ALONZO_APP_START);
	app_jump(ALONZO_APP_START);

	/* app_jump() does not return. */
	error_loop("jump failed");
	return 0;
}
