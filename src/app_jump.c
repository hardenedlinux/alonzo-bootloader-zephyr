/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "app_jump.h"
#include "memory_layout.h"

#include <cmsis_core.h>
#include <zephyr/kernel.h>

typedef void (*reset_handler_t)(void);

static bool msp_in_sram(uint32_t msp)
{
	return msp > ALONZO_SRAM_BASE && msp < ALONZO_SRAM_END;
}

static bool addr_in_app_flash(uint32_t addr)
{
	return addr >= ALONZO_APP_START && addr < ALONZO_APP_END;
}

bool app_image_valid(uint32_t app_start)
{
	const uint32_t *vt = (const uint32_t *)app_start;
	uint32_t msp = vt[0];
	uint32_t reset = vt[1];

	/* Reset handler must be within the app flash and thumb (bit 0 clear). */
	return msp_in_sram(msp) && addr_in_app_flash(reset) && ((reset & 1u) == 0u);
}

void app_jump(uint32_t app_start)
{
	const uint32_t *vt = (const uint32_t *)app_start;
	uint32_t msp = vt[0];
	uint32_t reset = vt[1];

	if (!app_image_valid(app_start)) {
		return;
	}

	/* No interrupts past this point. */
	__disable_irq();

	/* Stop and clear SysTick so the app gets a clean slate. */
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;
	SCB->ICSR |= SCB_ICSR_PENDSTCLR_Msk;
	SCB->ICSR |= SCB_ICSR_PENDSVCLR_Msk;

	/* Clear every pending NVIC interrupt. */
	uint32_t n_irq_lines = ((SCnSCB->ICTR & SCnSCB_ICTR_INTLINESNUM_Msk)
				>> SCnSCB_ICTR_INTLINESNUM_Pos) + 1;
	for (uint32_t i = 0; i < n_irq_lines; i++) {
		NVIC->ICER[i] = 0xFFFFFFFFu;
	}

	/* Relocate the vector table and install the new stack pointer. */
	SCB->VTOR = app_start;
	__set_MSP(msp);

	/* Jump to the application reset handler; it never returns. */
	((reset_handler_t)reset)();

	__builtin_unreachable();
}
