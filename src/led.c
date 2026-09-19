/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "led.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

/*
 * LED -> GPIO mapping.
 *
 * HARDWARE VERIFICATION STATUS: UNVERIFIED.
 *
 * The physical colour of each LED has not been confirmed on real hardware.
 * The mapping below (led0 -> RED, led1 -> GREEN) is a placeholder that must
 * be validated before field use. Polarity (active-low) is NOT assumed here:
 * it is taken from the devicetree gpio flags, so led_set(.., true) always
 * means "on" regardless of active-low/active-high.
 */
#define LED_RED_NODE   DT_ALIAS(led0)
#define LED_GREEN_NODE DT_ALIAS(led1)

static const struct gpio_dt_spec led_specs[ALONZO_LED_COUNT] = {
	[ALONZO_LED_RED]   = GPIO_DT_SPEC_GET(LED_RED_NODE, gpios),
	[ALONZO_LED_GREEN] = GPIO_DT_SPEC_GET(LED_GREEN_NODE, gpios),
};

void led_init(void)
{
	for (int i = 0; i < ALONZO_LED_COUNT; i++) {
		const struct gpio_dt_spec *spec = &led_specs[i];

		if (!gpio_is_ready_dt(spec)) {
			continue;
		}
		gpio_pin_configure_dt(spec, GPIO_OUTPUT_INACTIVE);
	}
}

void led_set(enum alonzo_led_id id, bool on)
{
	const struct gpio_dt_spec *spec;

	if ((unsigned int)id >= ALONZO_LED_COUNT) {
		return;
	}
	spec = &led_specs[id];
	if (!gpio_is_ready_dt(spec)) {
		return;
	}
	gpio_pin_set_dt(spec, on ? 1 : 0);
}

void led_toggle(enum alonzo_led_id id)
{
	const struct gpio_dt_spec *spec;

	if ((unsigned int)id >= ALONZO_LED_COUNT) {
		return;
	}
	spec = &led_specs[id];
	if (!gpio_is_ready_dt(spec)) {
		return;
	}
	gpio_pin_toggle_dt(spec);
}

void led_all_off(void)
{
	for (int i = 0; i < ALONZO_LED_COUNT; i++) {
		led_set((enum alonzo_led_id)i, false);
	}
}
