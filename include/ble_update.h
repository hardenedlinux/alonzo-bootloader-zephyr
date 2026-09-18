/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ALONZO_BLE_UPDATE_H
#define ALONZO_BLE_UPDATE_H

/*
 * FR8016HA (BLE) firmware update.
 *
 * The FR8016HA is a separate BLE co-processor. The update is performed over SWD
 * (PA2/PA3, USART2 in normal operation) using the vendor .FLM flash algorithm
 * running on the FR8016HA itself -- see src/fr8016/fr8016.c. The STM32 does not
 * implement a BLE stack and does not use the private FREQCHIP UART protocol.
 */

/* Update the FR8016HA firmware from /SD:ble.bin. Returns 0 / -errno. */
int ble_update(void);

#endif /* ALONZO_BLE_UPDATE_H */
