/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "ble_update.h"
#include "cortexm.h"
#include "fr8016.h"
#include "tf.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/*
 * FR8016HA (BLE) firmware update, implemented over SWD.
 *
 * The FR8016HA is a separate Cortex-M3 co-processor. The update path does NOT
 * use the FREQCHIP private UART downloader protocol: the STM32 bit-bangs SWD on
 * PA2/PA3 (USART2 in normal operation) and programs the FR8016HA internal flash
 * through the vendor .FLM flash algorithm read from the TF card (see fr8016.c).
 *
 * update_apply() calls ble_update() only when /SD:ble.bin is present. The BLE
 * update then runs only when the .FLM is also present; either file missing means
 * "no update, keep whatever is there". On success both files are deleted by
 * fr8016_update().
 */
int ble_update(void)
{
#if defined(CONFIG_ALONZO_FR8016_SWD_SELFTEST)
	/*
	 * Bring-up hook (Phases 1-4): first the SWD/DAP/debug chain
	 * (reset -> DPIDR -> power up -> AP IDR -> halt -> PC/SP/xPSR -> run),
	 * then a static validation of the .FLM parsed from the TF card.
	 */
	int rc = cm_selftest();

	if (rc != 0) {
		return rc;
	}
	return fr8016_flm_selftest(ALONZO_FLM_PATH);
#else
	/*
	 * Both ble.bin and FR8010H.FLM must be present. update.c already gated on
	 * ble.bin; check the .FLM here and skip (keeping ble.bin) if it is absent.
	 */
	if (!tf_file_exists(ALONZO_FLM_PATH, NULL)) {
		printk("ble_update: %s missing, keeping ble.bin\n", ALONZO_FLM_PATH);
		return 0;
	}
	return fr8016_update(ALONZO_BLE_PATH, ALONZO_FLM_PATH);
#endif
}
