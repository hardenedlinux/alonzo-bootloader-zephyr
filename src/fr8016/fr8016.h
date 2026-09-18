/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * FR8016HA flash-programming state machine (Phase 5).
 *
 * Programs the FR8016HA internal flash over SWD using the vendor .FLM flash
 * algorithm read from the TF card at runtime. The STM32 is only an SWD master +
 * Cortex-M debug register driver + MEM-AP memory-transfer host + FLM runtime
 * host; it never touches the FR8016HA QSPI flash controller directly.
 */

#ifndef ALONZO_FR8016_FR8016_H
#define ALONZO_FR8016_FR8016_H

/*
 * Update the FR8016HA firmware from @p ble_path using the flash algorithm at
 * @p flm_path (both read from the TF card).
 *
 * State machine: parse the .FLM -> disable USART2 -> PA2/PA3 as SWD -> SWD
 * attach -> halt the FR8016HA -> select the FR RAM work area -> flm_load() ->
 * flm_init() -> erase sectors -> program pages -> verify -> flm_uninit() -> FR
 * reset/run -> restore USART2 -> delete both files.
 *
 * Returns 0 on full success, -errno otherwise. On failure both files are kept
 * for the next boot; on success both are deleted.
 */
int fr8016_update(const char *ble_path, const char *flm_path);

/*
 * Bring-up self-test: parse the .FLM at @p flm_path and statically validate the
 * resulting image (no target access, no flash). Returns 0 on success, -errno on
 * a parse/validation failure; returns 0 (skip) if the file is absent.
 */
int fr8016_flm_selftest(const char *flm_path);

#endif /* ALONZO_FR8016_FR8016_H */
