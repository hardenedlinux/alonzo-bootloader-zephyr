/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dap.h"
#include "swd.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* The FR8016HA exposes a single MEM-AP at APSEL 0 (ARM-standard assumption). */
#define DAP_MEM_AP_SEL 0u

/* Power-up poll bound. */
#define DAP_POWERUP_RETRIES 100u
#define DAP_POWERUP_POLL_US 1000u

int dap_power_up(void)
{
	uint32_t ctrlstat;
	int rc;

	rc = swd_write_dp(SWD_DP_CTRLSTAT,
			  DP_CTRLSTAT_CDBGPWRUPREQ | DP_CTRLSTAT_CSYSPWRUPREQ);
	if (rc != 0) {
		return rc;
	}

	for (uint32_t i = 0; i < DAP_POWERUP_RETRIES; i++) {
		rc = swd_read_dp(SWD_DP_CTRLSTAT, &ctrlstat);
		if (rc != 0) {
			return rc;
		}
		if ((ctrlstat & (DP_CTRLSTAT_CDBGPWRUPACK | DP_CTRLSTAT_CSYSPWRUPACK)) ==
		    (DP_CTRLSTAT_CDBGPWRUPACK | DP_CTRLSTAT_CSYSPWRUPACK)) {
			return 0;
		}
		k_busy_wait(DAP_POWERUP_POLL_US);
	}

	return -ETIMEDOUT;
}

int dap_select(uint8_t apsel, uint8_t apbanksel)
{
	/*
	 * SELECT = APSEL[31:24] | APBANKSEL[23:8]. APBANKSEL[3:0] (SELECT bits
	 * 11:8) becomes A[7:4] of the AP register address.
	 */
	uint32_t select = ((uint32_t)apsel << 24) | ((uint32_t)apbanksel << 8);

	return swd_write_dp(SWD_DP_SELECT, select);
}

int dap_clear_errors(void)
{
	return swd_write_dp(SWD_DP_ABORT, DP_ABORT_STKERRCLR | DP_ABORT_STKCMPCLR);
}

int dap_init(void)
{
	int rc;

	rc = dap_power_up();
	if (rc != 0) {
		return rc;
	}
	/* Select MEM-AP bank 0 (CSW/TAR/DRW). */
	return dap_select(DAP_MEM_AP_SEL, 0);
}

int dap_read_ap_idr(uint32_t *idr)
{
	int rc;

	rc = dap_select(DAP_MEM_AP_SEL, AP_IDR_BANK);
	if (rc != 0) {
		return rc;
	}
	rc = swd_read_ap(AP_IDR_REG, idr);
	(void)dap_select(DAP_MEM_AP_SEL, 0); /* restore bank 0 */
	return rc;
}

/* Select the MEM-AP bank 0, configure CSW for 32-bit, and set TAR. */
static int mem_ap_setup(uint32_t addr)
{
	int rc;

	rc = dap_select(DAP_MEM_AP_SEL, 0);
	if (rc != 0) {
		return rc;
	}
	rc = swd_write_ap(AP_CSW, CSW_WORD);
	if (rc != 0) {
		return rc;
	}
	return swd_write_ap(AP_TAR, addr);
}

int mem_read32(uint32_t addr, uint32_t *value)
{
	uint32_t dummy;
	int rc;

	if (value == NULL) {
		return -EINVAL;
	}

	rc = mem_ap_setup(addr);
	if (rc != 0) {
		return rc;
	}

	/*
	 * The first DRW read posts the memory read; its data lands in the DP read
	 * buffer (RDBUFF). Retrieve the posted result from RDBUFF.
	 */
	rc = swd_read_ap(AP_DRW, &dummy);
	if (rc != 0) {
		return rc;
	}
	return swd_read_dp(SWD_DP_RDBUFF, value);
}

int mem_write32(uint32_t addr, uint32_t value)
{
	int rc;

	rc = mem_ap_setup(addr);
	if (rc != 0) {
		return rc;
	}
	return swd_write_ap(AP_DRW, value);
}

int dap_selftest(void)
{
	uint32_t dp_idr = 0, ctrlstat = 0, ap_idr = 0;
	int rc;

	rc = swd_init();
	if (rc != 0) {
		printk("dap_selftest: swd_init failed (%d)\n", rc);
		return rc;
	}
	rc = swd_line_reset();
	if (rc != 0) {
		printk("dap_selftest: line reset failed (%d)\n", rc);
		return rc;
	}
	rc = swd_read_idcode(&dp_idr);
	if (rc != 0) {
		printk("dap_selftest: DPIDR read failed (%d)\n", rc);
		return rc;
	}
	printk("dap_selftest: DPIDR = 0x%08x\n", dp_idr);

	rc = dap_power_up();
	if (rc != 0) {
		printk("dap_selftest: power up failed (%d)\n", rc);
		return rc;
	}
	rc = swd_read_dp(SWD_DP_CTRLSTAT, &ctrlstat);
	if (rc != 0) {
		printk("dap_selftest: CTRL/STAT read failed (%d)\n", rc);
		return rc;
	}
	printk("dap_selftest: CTRL/STAT = 0x%08x\n", ctrlstat);

	rc = dap_read_ap_idr(&ap_idr);
	if (rc != 0) {
		printk("dap_selftest: AP IDR read failed (%d)\n", rc);
		return rc;
	}
	printk("dap_selftest: AP IDR = 0x%08x\n", ap_idr);

	printk("dap_selftest: PASS\n");
	return 0;
}
