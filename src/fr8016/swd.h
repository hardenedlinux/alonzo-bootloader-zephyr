/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

/*
 * FR8016HA SWD transport (Phase 1).
 *
 * Bit-bangs the ARM Serial Wire Debug protocol over two GPIOs so the STM32
 * acts as an SWD *master* for the FR8016HA Cortex-M3:
 *
 *   STM32 PA2 (SWCLK)  ->  FR8016HA PC6 (SWCLK)
 *   STM32 PA3 (SWDIO)  <-> FR8016HA PC7 (SWDIO)
 *
 * Only the DP (Debug Port) register space is exposed here. AP / MEM-AP access,
 * Cortex-M debug control and the FLM loader are built on top in later phases.
 *
 * NOTE: these pins are USART2 (PA2 = TX, PA3 = RX) during normal operation.
 * This module only drives them as GPIO -- it never touches the USART2
 * peripheral. Mode switching (disable USART2 -> enter SWD -> restore USART2)
 * is owned by the upper layer (fr8016.c); see the FR8016 update flow.
 */

#ifndef ALONZO_FR8016_SWD_H
#define ALONZO_FR8016_SWD_H

#include <stdint.h>

/* DP register addresses (the A[3:2] field carried in the SWD request). */
#define SWD_DP_DPIDR    0x0u  /* read:  ID code */
#define SWD_DP_ABORT    0x0u  /* write: abort */
#define SWD_DP_CTRLSTAT 0x4u  /* read/write: control/status */
#define SWD_DP_SELECT   0x8u  /* write: AP bank select */
#define SWD_DP_RDBUFF   0xcu  /* read:  buffered read result */

/* ACK values returned by the target (3 bits, LSB-first). */
#define SWD_ACK_OK    0x1u
#define SWD_ACK_WAIT  0x2u
#define SWD_ACK_FAULT 0x4u

/*
 * Reconfigure PA2/PA3 as SWCLK/SWDIO and leave the SWD line idle.
 * Returns 0 on success, or a negative errno if the GPIO port is unavailable.
 */
int swd_init(void);

/*
 * Perform a full SWD line reset + JTAG-to-SWD switch sequence. After this the
 * target is in SWD mode and ready for a DPIDR read. Returns 0 / -errno.
 */
int swd_line_reset(void);

/*
 * Read a DP register into *value. Returns 0 on success, -errno otherwise
 * (-EIO on FAULT, -EAGAIN/-ETIMEDOUT on persistent WAIT, -EBADMSG on a read
 * parity error, -EPROTO on an invalid ACK).
 */
int swd_read_dp(uint8_t addr, uint32_t *value);

/*
 * Write a DP register. Returns 0 on success, -errno otherwise
 * (-EIO on FAULT, -EAGAIN/-ETIMEDOUT on persistent WAIT).
 */
int swd_write_dp(uint8_t addr, uint32_t value);

/*
 * Read/write an AP register (APnDP = 1). @p addr is the low 2-bit register
 * offset within the bank selected by SELECT.APBANKSEL.
 */
int swd_read_ap(uint8_t addr, uint32_t *value);
int swd_write_ap(uint8_t addr, uint32_t value);

/*
 * Convenience: read the DPIDR (ID code). The FR8016HA value is reported but
 * NOT asserted here -- it needs hardware validation.
 */
int swd_read_idcode(uint32_t *idcode);

/*
 * Phase-1 self-test: init -> line reset -> DPIDR read, printing the result.
 * This is the first hardware validation target. Returns 0 / -errno.
 */
int swd_selftest(void);

#endif /* ALONZO_FR8016_SWD_H */
