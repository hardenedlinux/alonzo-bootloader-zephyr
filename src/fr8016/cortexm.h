/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

/*
 * Cortex-M debug control layer (Phase 3).
 *
 * Sits on top of dap.h (which provides mem_read32/mem_write32 through the
 * MEM-AP). Implements the minimum needed to load and run the FR8010H.FLM flash
 * algorithm on the FR8016HA Cortex-M3:
 *
 *   halt / run / reset, and core register access (R0-R12, SP, LR, PC, xPSR,
 *   MSP, PSP, CONTROL) via the standard debug registers.
 *
 * Only ARM-standard Cortex-M debug registers are used (SCS at 0xE000E000):
 *
 *   DHCSR 0xE000EDF0   Debug Halting Control and Status Register
 *   DCRSR 0xE000EDF4   Debug Core Register Selector Register
 *   DCRDR 0xE000EDF8   Debug Core Register Data Register
 *   DEMCR 0xE000EDFC   Debug Exception and Monitor Control Register
 *   DFSR  0xE000ED30   Debug Fault Status Register
 *   AIRCR 0xE000ED0C   Application Interrupt and Reset Control Register
 *
 * No GDB, no breakpoints/watchpoints, no trace, no CMSIS-DAP/JTAG, and no
 * FR8016HA-specific debug register is assumed: the register addresses and bit
 * layouts are the ARM-defined ones. Timing-sensitive behaviour (reset vector
 * catch handshake, S_REGRDY latency) is marked NEED HARDWARE VALIDATION.
 */

#ifndef ALONZO_FR8016_CORTEXM_H
#define ALONZO_FR8016_CORTEXM_H

#include <stdbool.h>
#include <stdint.h>

/* ARM-standard Cortex-M debug register addresses (System Control Space). */
#define CM_DHCSR 0xE000EDF0u
#define CM_DCRSR 0xE000EDF4u
#define CM_DCRDR 0xE000EDF8u
#define CM_DEMCR 0xE000EDFCu
#define CM_DFSR  0xE000ED30u
#define CM_AIRCR 0xE000ED0Cu

/* DHCSR: DBGKEY must be written to bits [31:16] or the write is ignored. */
#define CM_DBGKEY            (0xA05Fu << 16)
/* DHCSR control bits (write, with DBGKEY). */
#define CM_DHCSR_C_DEBUGEN   (1u << 0)
#define CM_DHCSR_C_HALT      (1u << 1)
#define CM_DHCSR_C_STEP      (1u << 2)
#define CM_DHCSR_C_MASKINTS  (1u << 3)
/* DHCSR status bits (read). */
#define CM_DHCSR_S_REGRDY    (1u << 16)
#define CM_DHCSR_S_HALT      (1u << 17)
#define CM_DHCSR_S_SLEEP     (1u << 18)
#define CM_DHCSR_S_LOCKUP    (1u << 19)
#define CM_DHCSR_S_RETIRE_ST (1u << 24)
#define CM_DHCSR_S_RESET_ST  (1u << 25)

/* DCRSR: REGWnR selects write (1) vs read (0); REGSEL is 7 bits [6:0]. */
#define CM_DCRSR_REGWnR       (1u << 16)
#define CM_DCRSR_REGSEL_MASK  0x7Fu

/* DEMCR: reset vector catch (VC_CORERESET). */
#define CM_DEMCR_VC_CORERESET (1u << 0)

/* AIRCR: VECTKEY must be written to bits [31:16]; SYSRESETREQ resets the core. */
#define CM_AIRCR_VECTKEY      (0x05FAu << 16)
#define CM_AIRCR_SYSRESETREQ  (1u << 2)

/* Core register selectors (ARM-standard M-profile numbering). */
#define CM_REG_R0       0u
#define CM_REG_R1       1u
#define CM_REG_R2       2u
#define CM_REG_R3       3u
#define CM_REG_R4       4u
#define CM_REG_R5       5u
#define CM_REG_R6       6u
#define CM_REG_R7       7u
#define CM_REG_R8       8u
#define CM_REG_R9       9u
#define CM_REG_R10      10u
#define CM_REG_R11      11u
#define CM_REG_R12      12u
#define CM_REG_SP       13u  /* R13, the currently-selected stack pointer */
#define CM_REG_LR       14u
#define CM_REG_PC       15u
#define CM_REG_xPSR     16u
#define CM_REG_MSP      17u
#define CM_REG_PSP      18u
#define CM_REG_CONTROL  20u

/*
 * Halt the target core (set C_DEBUGEN | C_HALT) and wait for S_HALT.
 * Returns 0 / -errno.
 */
int cm_halt(void);

/*
 * Run the target core (keep C_DEBUGEN, clear C_HALT) and wait for S_HALT to
 * clear. Returns 0 / -errno.
 */
int cm_run(void);

/*
 * Resume execution (clear C_HALT) WITHOUT waiting for the core to leave halt.
 * Use cm_wait_halted() afterwards when running a routine that halts itself via
 * a BKPT stub; avoids racing the fast "run -> BKPT -> halt" round trip.
 */
int cm_resume(void);

/*
 * Reset-and-halt: enable the reset vector catch (DEMCR.VC_CORERESET), halt,
 * issue a system reset (AIRCR.SYSRESETREQ), wait for the core to halt at the
 * reset vector, then clear VC_CORERESET. Returns 0 / -errno.
 */
int cm_reset(void);

/* Report whether the core is halted (S_HALT). Returns 0 / -errno. */
int cm_is_halted(bool *halted);

/*
 * Wait for the core to halt on its own (S_HALT set) without writing DHCSR.
 * Used after running a routine that returns into a BKPT stub. Returns 0 on
 * halt, -errno on a transport error, -ETIMEDOUT on timeout.
 */
int cm_wait_halted(void);

/* Read/write a core register selected by @p regsel (CM_REG_*). Returns 0/-errno. */
int cm_read_reg(uint8_t regsel, uint32_t *value);
int cm_write_reg(uint8_t regsel, uint32_t value);

/* Convenience helpers for the two registers the FLM loader needs most. */
int cm_read_pc(uint32_t *pc);
int cm_write_pc(uint32_t pc);
int cm_read_sp(uint32_t *sp);
int cm_write_sp(uint32_t sp);

/* Read the debug fault status register (DFSR). Report-only. Returns 0/-errno. */
int cm_read_dfsr(uint32_t *dfsr);

/*
 * Clear debug/DP sticky state: DP ABORT (STICKYERR/STICKYCMP) plus the DFSR
 * bits (write-1-to-clear). Call after any FAULT/error before retrying.
 * Returns 0 / -errno.
 */
int cm_clear_errors(void);

/*
 * Phase-3 self-test: dap_selftest() then halt -> read PC/SP/xPSR -> run.
 * This is the third hardware validation target. Returns 0 / -errno.
 */
int cm_selftest(void);

#endif /* ALONZO_FR8016_CORTEXM_H */
