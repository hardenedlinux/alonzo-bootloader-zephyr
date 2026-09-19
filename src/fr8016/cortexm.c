/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "cortexm.h"
#include "dap.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/*
 * Poll bounds. Deliberately generous: the SWD master is bit-banged at ~500 kHz,
 * so a single DHCSR read costs on the order of 100 us. The exact latencies for
 * S_HALT / S_REGRDY on the FR8016HA are NEED HARDWARE VALIDATION; these bounds
 * are worst-case ceilings, not performance targets.
 */
#define CM_POLL_US         1000u
#define CM_HALT_RETRIES    1000u
#define CM_REGRDY_RETRIES  1000u

/*
 * Poll DHCSR until (value & mask) == want. Returns 0 on match, -errno on a
 * transport error or -ETIMEDOUT when the bound is exhausted.
 */
static int cm_wait_dhcsr(uint32_t mask, uint32_t want, uint32_t retries)
{
	uint32_t dhcsr;
	int rc;

	for (uint32_t i = 0; i < retries; i++) {
		rc = mem_read32(CM_DHCSR, &dhcsr);
		if (rc != 0) {
			return rc;
		}
		if ((dhcsr & mask) == want) {
			return 0;
		}
		k_busy_wait(CM_POLL_US);
	}

	return -ETIMEDOUT;
}

int cm_halt(void)
{
	int rc;

	rc = mem_write32(CM_DHCSR,
			 CM_DBGKEY | CM_DHCSR_C_DEBUGEN | CM_DHCSR_C_HALT);
	if (rc != 0) {
		return rc;
	}
	return cm_wait_dhcsr(CM_DHCSR_S_HALT, CM_DHCSR_S_HALT, CM_HALT_RETRIES);
}

int cm_run(void)
{
	int rc;

	rc = mem_write32(CM_DHCSR, CM_DBGKEY | CM_DHCSR_C_DEBUGEN);
	if (rc != 0) {
		return rc;
	}
	return cm_wait_dhcsr(CM_DHCSR_S_HALT, 0u, CM_HALT_RETRIES);
}

int cm_resume(void)
{
	return mem_write32(CM_DHCSR, CM_DBGKEY | CM_DHCSR_C_DEBUGEN);
}

int cm_reset(void)
{
	uint32_t demcr;
	int rc;

	/* Catch the reset vector so the core halts before running the app. */
	rc = mem_read32(CM_DEMCR, &demcr);
	if (rc != 0) {
		return rc;
	}
	rc = mem_write32(CM_DEMCR, demcr | CM_DEMCR_VC_CORERESET);
	if (rc != 0) {
		return rc;
	}

	/* Halt (also sets C_DEBUGEN). */
	rc = cm_halt();
	if (rc != 0) {
		return rc;
	}

	/* System reset request. */
	rc = mem_write32(CM_AIRCR, CM_AIRCR_VECTKEY | CM_AIRCR_SYSRESETREQ);
	if (rc != 0) {
		return rc;
	}

	/*
	 * Wait for the core to come out of reset halted at the reset vector.
	 * NEED HARDWARE VALIDATION: the core was already halted before the reset,
	 * so the first poll may observe a stale S_HALT; on real silicon a short
	 * settle delay and/or waiting for S_RESET_ST to clear may be required.
	 */
	rc = cm_wait_dhcsr(CM_DHCSR_S_HALT, CM_DHCSR_S_HALT, CM_HALT_RETRIES);
	if (rc != 0) {
		return rc;
	}

	/* Clear the reset vector catch. */
	rc = mem_read32(CM_DEMCR, &demcr);
	if (rc != 0) {
		return rc;
	}
	return mem_write32(CM_DEMCR, demcr & ~CM_DEMCR_VC_CORERESET);
}

int cm_is_halted(bool *halted)
{
	uint32_t dhcsr;
	int rc;

	if (halted == NULL) {
		return -EINVAL;
	}
	rc = mem_read32(CM_DHCSR, &dhcsr);
	if (rc != 0) {
		return rc;
	}
	*halted = (dhcsr & CM_DHCSR_S_HALT) != 0u;
	return 0;
}

int cm_wait_halted(void)
{
	return cm_wait_dhcsr(CM_DHCSR_S_HALT, CM_DHCSR_S_HALT, CM_HALT_RETRIES);
}

int cm_read_reg(uint8_t regsel, uint32_t *value)
{
	int rc;

	if (value == NULL) {
		return -EINVAL;
	}

	/*
	 * Request a read of the selected core register. Writing DCRSR clears
	 * S_REGRDY and initiates the transfer.
	 */
	rc = mem_write32(CM_DCRSR, regsel & CM_DCRSR_REGSEL_MASK);
	if (rc != 0) {
		return rc;
	}

	rc = cm_wait_dhcsr(CM_DHCSR_S_REGRDY, CM_DHCSR_S_REGRDY,
			   CM_REGRDY_RETRIES);
	if (rc != 0) {
		return rc;
	}

	return mem_read32(CM_DCRDR, value);
}

int cm_write_reg(uint8_t regsel, uint32_t value)
{
	int rc;

	/* Load the data register first, then trigger the write. */
	rc = mem_write32(CM_DCRDR, value);
	if (rc != 0) {
		return rc;
	}
	rc = mem_write32(CM_DCRSR,
			 CM_DCRSR_REGWnR | (regsel & CM_DCRSR_REGSEL_MASK));
	if (rc != 0) {
		return rc;
	}

	return cm_wait_dhcsr(CM_DHCSR_S_REGRDY, CM_DHCSR_S_REGRDY,
			     CM_REGRDY_RETRIES);
}

int cm_read_pc(uint32_t *pc)
{
	return cm_read_reg(CM_REG_PC, pc);
}

int cm_write_pc(uint32_t pc)
{
	return cm_write_reg(CM_REG_PC, pc);
}

int cm_read_sp(uint32_t *sp)
{
	return cm_read_reg(CM_REG_SP, sp);
}

int cm_write_sp(uint32_t sp)
{
	return cm_write_reg(CM_REG_SP, sp);
}

int cm_read_dfsr(uint32_t *dfsr)
{
	if (dfsr == NULL) {
		return -EINVAL;
	}
	return mem_read32(CM_DFSR, dfsr);
}

int cm_clear_errors(void)
{
	uint32_t dfsr;
	int rc;

	/* Clear DP sticky errors (STICKYERR / STICKYCMP). */
	rc = dap_clear_errors();
	if (rc != 0) {
		return rc;
	}

	/* Clear the debug fault status register (all bits are write-1-to-clear). */
	rc = mem_read32(CM_DFSR, &dfsr);
	if (rc != 0) {
		return rc;
	}
	if (dfsr != 0u) {
		return mem_write32(CM_DFSR, dfsr);
	}
	return 0;
}

int cm_selftest(void)
{
	uint32_t pc = 0, sp = 0, xpsr = 0;
	int rc;

	/* Phase 2 chain: reset -> DPIDR -> power up -> AP IDR. */
	rc = dap_selftest();
	if (rc != 0) {
		return rc;
	}

	rc = cm_halt();
	if (rc != 0) {
		printk("cm_selftest: halt failed (%d)\n", rc);
		return rc;
	}
	printk("cm_selftest: core halted\n");

	rc = cm_read_pc(&pc);
	if (rc != 0) {
		printk("cm_selftest: read PC failed (%d)\n", rc);
		return rc;
	}
	rc = cm_read_sp(&sp);
	if (rc != 0) {
		printk("cm_selftest: read SP failed (%d)\n", rc);
		return rc;
	}
	rc = cm_read_reg(CM_REG_xPSR, &xpsr);
	if (rc != 0) {
		printk("cm_selftest: read xPSR failed (%d)\n", rc);
		return rc;
	}
	printk("cm_selftest: PC=0x%08x SP=0x%08x xPSR=0x%08x\n", pc, sp, xpsr);

	rc = cm_run();
	if (rc != 0) {
		printk("cm_selftest: run failed (%d)\n", rc);
		return rc;
	}
	printk("cm_selftest: core running\n");

	printk("cm_selftest: PASS\n");
	return 0;
}
