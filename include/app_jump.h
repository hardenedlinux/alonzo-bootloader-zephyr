/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef ALONZO_APP_JUMP_H
#define ALONZO_APP_JUMP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Validate a Cortex-M application image at @p app_start:
 *   - initial stack pointer (MSP) is inside SRAM
 *   - reset handler is inside the application flash region and thumb-aligned
 */
bool app_image_valid(uint32_t app_start);

/*
 * Perform a clean Cortex-M handoff to the application at @p app_start.
 * Does not return on success; returns (doing nothing) if the image is invalid.
 */
void app_jump(uint32_t app_start);

#endif /* ALONZO_APP_JUMP_H */
