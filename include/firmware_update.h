/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef ALONZO_FIRMWARE_UPDATE_H
#define ALONZO_FIRMWARE_UPDATE_H

/*
 * Update the STM32 application firmware from /SD:firmware.bin.
 *
 * On success the update file is deleted. On failure the file is kept so the
 * next boot retries, and the red LED is left on.
 *
 * Returns 0 on success, a negative errno otherwise.
 */
int firmware_update(void);

#endif /* ALONZO_FIRMWARE_UPDATE_H */
