/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "swd.h"

#include <errno.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/*
 * SWD pins (verified against the Alonzo schematic):
 *   STM32 PA2 -> SWCLK (FR8016HA PC6)
 *   STM32 PA3 -> SWDIO (FR8016HA PC7)
 */
#define SWD_PORT_NODE  DT_NODELABEL(gpioa)
#define SWD_SWCLK_PIN  2u
#define SWD_SWDIO_PIN  3u

/*
 * Half-period of the bit-banged SWCLK, in microseconds. 1 us -> ~500 kHz SWCLK.
 * Deliberately conservative for first bring-up; raise it only after the
 * transport is proven on hardware.
 */
#define SWD_CLK_HALF_PERIOD_US 1u

/* Bounded retries when the target answers WAIT. */
#define SWD_MAX_RETRIES 64u

/* JTAG-to-SWD switch sequence (16 bits, LSB-first). */
#define SWD_SWITCH_SEQUENCE 0xE79Eu

static const struct device *swd_port;
static bool swdio_is_output;

static inline void swd_delay(void)
{
	k_busy_wait(SWD_CLK_HALF_PERIOD_US);
}

static inline void swclk_set(int level)
{
	gpio_pin_set_raw(swd_port, SWD_SWCLK_PIN, level);
}

static inline void swdio_set(int level)
{
	gpio_pin_set_raw(swd_port, SWD_SWDIO_PIN, level);
}

static inline int swdio_get(void)
{
	return gpio_pin_get_raw(swd_port, SWD_SWDIO_PIN);
}

/* Drive SWDIO (push-pull). */
static int swdio_dir_output(void)
{
	int rc;

	if (swdio_is_output) {
		return 0;
	}
	rc = gpio_pin_configure(swd_port, SWD_SWDIO_PIN, GPIO_OUTPUT_LOW);
	if (rc == 0) {
		swdio_is_output = true;
	}
	return rc;
}

/* Release SWDIO (input with pull-up: the line idles high). */
static int swdio_dir_input(void)
{
	int rc;

	if (!swdio_is_output) {
		return 0;
	}
	rc = gpio_pin_configure(swd_port, SWD_SWDIO_PIN,
				GPIO_INPUT | GPIO_PULL_UP);
	if (rc == 0) {
		swdio_is_output = false;
	}
	return rc;
}

/* One full SWCLK cycle (rising then falling edge). */
static void swclk_cycle(void)
{
	swclk_set(1);
	swd_delay();
	swclk_set(0);
	swd_delay();
}

/* Clock out 'count' bits of 'value', LSB first (host drives SWDIO). */
static void swd_write_bits(uint32_t value, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		swdio_set((int)((value >> i) & 1u));
		swclk_set(1);
		swd_delay();
		swclk_set(0);
		swd_delay();
	}
}

/* Clock in 'count' bits, LSB first (host samples on each rising edge). */
static uint32_t swd_read_bits(uint32_t count)
{
	uint32_t value = 0;

	for (uint32_t i = 0; i < count; i++) {
		swclk_set(1);
		swd_delay();
		if (swdio_get()) {
			value |= 1u << i;
		}
		swclk_set(0);
		swd_delay();
	}
	return value;
}

/* One turnaround cycle (SWDIO direction has already been switched). */
static void swd_turnaround(void)
{
	swclk_cycle();
}

/* Even parity over 32 bits (1 if the data has an odd number of ones). */
static int swd_parity32(uint32_t value)
{
	value ^= value >> 16u;
	value ^= value >> 8u;
	value ^= value >> 4u;
	value ^= value >> 2u;
	value ^= value >> 1u;
	return (int)(value & 1u);
}

/*
 * Build the 8-bit SWD request (transmitted LSB first):
 *   bit0 start=1, bit1 APnDP, bit2 RnW, bit3 A2, bit4 A3,
 *   bit5 parity (odd over bits 1..4), bit6 stop=0, bit7 park=1
 */
static uint32_t swd_build_request(bool apndp, bool rnw, uint8_t addr)
{
	uint8_t a2 = (addr >> 2u) & 1u;
	uint8_t a3 = (addr >> 3u) & 1u;
	uint8_t parity = (apndp ^ rnw ^ a2 ^ a3) ^ 1u;

	return 1u
	     | ((uint32_t)apndp << 1u)
	     | ((uint32_t)rnw   << 2u)
	     | ((uint32_t)a2    << 3u)
	     | ((uint32_t)a3    << 4u)
	     | ((uint32_t)parity << 5u)
	     | (0u << 6u)
	     | (1u << 7u);
}

/* Low-level SWD transaction. Returns 0 on success, -errno otherwise. */
static int swd_transaction(bool apndp, bool rnw, uint8_t addr, uint32_t *data)
{
	uint32_t ack;
	int rc;

	if (swd_port == NULL) {
		return -ENODEV;
	}

	/* Request (host drives). */
	rc = swdio_dir_output();
	if (rc != 0) {
		return rc;
	}
	swd_write_bits(swd_build_request(apndp, rnw, addr), 8);

	/* Turnaround: release SWDIO so the target can drive ACK. */
	rc = swdio_dir_input();
	if (rc != 0) {
		return rc;
	}
	swd_turnaround();

	/* ACK. */
	ack = swd_read_bits(3);
	switch (ack) {
	case SWD_ACK_OK:
		break;
	case SWD_ACK_WAIT:
		return -EAGAIN;
	case SWD_ACK_FAULT:
		return -EIO;
	default:
		/* No/invalid response: line floating or not an SWD target. */
		return -EPROTO;
	}

	if (rnw) {
		/* Read: target drives data; keep SWDIO released. */
		swd_turnaround();
		*data = swd_read_bits(32);
		uint32_t parity = swd_read_bits(1);

		/* Trailing turnaround: host takes the line back. */
		rc = swdio_dir_output();
		if (rc != 0) {
			return rc;
		}
		swd_turnaround();

		if (swd_parity32(*data) != (int)(parity & 1u)) {
			return -EBADMSG;
		}
	} else {
		/* Write: host drives data. */
		rc = swdio_dir_output();
		if (rc != 0) {
			return rc;
		}
		swd_turnaround();
		swd_write_bits(*data, 32);
		swd_write_bits((uint32_t)swd_parity32(*data), 1);
		/* Leave SWDIO low (idle) for the next request. */
		swdio_set(0);
	}

	return 0;
}

int swd_init(void)
{
	int rc;

	swd_port = DEVICE_DT_GET(SWD_PORT_NODE);
	if (!device_is_ready(swd_port)) {
		printk("swd: GPIOA not ready\n");
		return -ENODEV;
	}

	/*
	 * SWCLK is an output. SWDIO starts as an input (released / idle-high) so
	 * the line settles before the first transaction drives it.
	 */
	rc = gpio_pin_configure(swd_port, SWD_SWCLK_PIN, GPIO_OUTPUT_LOW);
	if (rc != 0) {
		return rc;
	}
	rc = gpio_pin_configure(swd_port, SWD_SWDIO_PIN,
				GPIO_INPUT | GPIO_PULL_UP);
	if (rc != 0) {
		return rc;
	}
	swdio_is_output = false;

	return 0;
}

int swd_line_reset(void)
{
	int rc;

	if (swd_port == NULL) {
		return -ENODEV;
	}

	rc = swdio_dir_output();
	if (rc != 0) {
		return rc;
	}

	/* 1. Line reset: >= 50 SWCLK cycles with SWDIO high. */
	swdio_set(1);
	for (int i = 0; i < 51; i++) {
		swclk_cycle();
	}

	/* 2. JTAG-to-SWD switch sequence (16 bits, LSB first). */
	swd_write_bits(SWD_SWITCH_SEQUENCE, 16);

	/* 3. Line reset again: >= 50 cycles high. */
	swdio_set(1);
	for (int i = 0; i < 51; i++) {
		swclk_cycle();
	}

	/* 4. Idle: a couple of cycles with SWDIO low. */
	swdio_set(0);
	swclk_cycle();
	swclk_cycle();

	return 0;
}

/* Raw read/write with the WAIT retry loop, parameterised by APnDP. */
static int swd_read_raw(bool apndp, uint8_t addr, uint32_t *value)
{
	int rc = -EAGAIN;

	for (uint32_t attempt = 0; attempt < SWD_MAX_RETRIES; attempt++) {
		rc = swd_transaction(apndp, true, addr, value);
		if (rc != -EAGAIN) {
			return rc;
		}
	}
	return -ETIMEDOUT;
}

static int swd_write_raw(bool apndp, uint8_t addr, uint32_t value)
{
	int rc = -EAGAIN;

	for (uint32_t attempt = 0; attempt < SWD_MAX_RETRIES; attempt++) {
		rc = swd_transaction(apndp, false, addr, &value);
		if (rc != -EAGAIN) {
			return rc;
		}
	}
	return -ETIMEDOUT;
}

int swd_read_dp(uint8_t addr, uint32_t *value)
{
	return swd_read_raw(false, addr, value);
}

int swd_write_dp(uint8_t addr, uint32_t value)
{
	return swd_write_raw(false, addr, value);
}

int swd_read_ap(uint8_t addr, uint32_t *value)
{
	return swd_read_raw(true, addr, value);
}

int swd_write_ap(uint8_t addr, uint32_t value)
{
	return swd_write_raw(true, addr, value);
}

int swd_read_idcode(uint32_t *idcode)
{
	return swd_read_dp(SWD_DP_DPIDR, idcode);
}

int swd_selftest(void)
{
	uint32_t idcode = 0;
	int rc;

	rc = swd_init();
	if (rc != 0) {
		printk("swd_selftest: init failed (%d)\n", rc);
		return rc;
	}

	rc = swd_line_reset();
	if (rc != 0) {
		printk("swd_selftest: line reset failed (%d)\n", rc);
		return rc;
	}

	rc = swd_read_idcode(&idcode);
	if (rc != 0) {
		printk("swd_selftest: DPIDR read failed (%d)\n", rc);
		return rc;
	}

	printk("swd_selftest: DPIDR = 0x%08x\n", idcode);
	return 0;
}
