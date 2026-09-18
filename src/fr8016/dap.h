/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * DAP (Debug Access Port) layer over the SWD transport (Phase 2).
 *
 * Builds DP control (CTRL/STAT, SELECT, ABORT) and MEM-AP word access
 * (CSW / TAR / DRW) on top of swd.h. This is the minimum needed to read and
 * write the FR8016HA memory space from the STM32.
 */

#ifndef ALONZO_FR8016_DAP_H
#define ALONZO_FR8016_DAP_H

#include <stdint.h>

/* DP CTRL/STAT bits (ARM-standard). */
#define DP_CTRLSTAT_CDBGPWRUPREQ (1u << 28)
#define DP_CTRLSTAT_CDBGPWRUPACK (1u << 29)
#define DP_CTRLSTAT_CSYSPWRUPREQ (1u << 30)
#define DP_CTRLSTAT_CSYSPWRUPACK (1u << 31)

/* DP ABORT bits (ARM-standard). */
#define DP_ABORT_DAPABORT  (1u << 0)
#define DP_ABORT_STKCMPCLR (1u << 1)
#define DP_ABORT_STKERRCLR (1u << 2)

/* AP register offsets (low 2 address bits; bank comes from SELECT.APBANKSEL). */
#define AP_CSW 0x00u
#define AP_TAR 0x04u
#define AP_DRW 0x0cu

/* AP IDR lives in bank 0xF at register offset 0xC. */
#define AP_IDR_REG  0x0cu
#define AP_IDR_BANK 0x0fu

/* MEM-AP CSW: 32-bit word, single address increment. */
#define CSW_SIZE_32        (0x2u)
#define CSW_ADDRINC_SINGLE (0x1u << 4)
#define CSW_WORD           (CSW_SIZE_32 | CSW_ADDRINC_SINGLE)

/*
 * Power up the debug + system domains and select the MEM-AP (bank 0).
 * Returns 0 on success, -errno otherwise.
 */
int dap_init(void);

/* Request and poll debug/system power (CDBGPWRUPREQ / CSYSPWRUPREQ). */
int dap_power_up(void);

/* Select AP @p apsel and register bank @p apbanksel. */
int dap_select(uint8_t apsel, uint8_t apbanksel);

/* Clear sticky errors via the DP ABORT register. */
int dap_clear_errors(void);

/* Read the MEM-AP IDR (bank 0xF). Report-only: no value is asserted. */
int dap_read_ap_idr(uint32_t *idr);

/*
 * Read/write a 32-bit word in the FR8016HA memory space via the MEM-AP.
 * Requires dap_init() to have been called first. Returns 0 / -errno.
 */
int mem_read32(uint32_t addr, uint32_t *value);
int mem_write32(uint32_t addr, uint32_t value);

/*
 * Phase-2 self-test: reset -> DPIDR -> power up -> CTRL/STAT -> AP IDR.
 * This is the second hardware validation target. Returns 0 / -errno.
 */
int dap_selftest(void);

#endif /* ALONZO_FR8016_DAP_H */
