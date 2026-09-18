/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ALONZO_UPDATE_H
#define ALONZO_UPDATE_H

/*
 * Apply any pending TF updates, in fixed order: firmware.bin first, then
 * ble.bin. Requires the TF card to already be mounted.
 *
 * Returns 0 if all applicable updates succeeded (or none were present), or a
 * negative errno on the first failure. On failure the offending file is kept
 * so the next boot retries.
 */
int update_apply(void);

#endif /* ALONZO_UPDATE_H */
