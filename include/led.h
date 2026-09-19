/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef ALONZO_LED_H
#define ALONZO_LED_H

#include <stdbool.h>

/*
 * Status LEDs. Only two are used by the bootloader:
 *   RED   - burning / error
 *   GREEN - success
 */
enum alonzo_led_id {
	ALONZO_LED_RED = 0,
	ALONZO_LED_GREEN,
	ALONZO_LED_COUNT,
};

/* Configure every status LED (inactive) and turn them off. */
void led_init(void);

/* Set a status LED on (true) or off (false). Polarity is encapsulated. */
void led_set(enum alonzo_led_id id, bool on);

/* Toggle a status LED. */
void led_toggle(enum alonzo_led_id id);

/* Turn every status LED off. */
void led_all_off(void);

#endif /* ALONZO_LED_H */
