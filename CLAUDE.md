# CLAUDE.md

Guidance for working in **this directory** — the Alonzo **Maker Bootloader**.

## What this is

The Maker Bootloader for the Animula **Alonzo** board (STM32F411CEU6). It is a
self-contained **Zephyr application**, not a board support package. It lives in
`bootloader/` of the `alonzo-zephyr-bsp` repo, but it is a separate project from
the BSP module at the repo root (which only provides the `animula_alonzo` board).

- Target: **Zephyr 4.4.x** (4.4.2-rc1), HWMv2.
- Board: `animula_alonzo` (provided by the parent BSP; see below).
- MCU: STM32F411CEU6, Cortex-M4F, 512 KB flash, 128 KB SRAM.

The bootloader updates firmware from a TF (micro-SD) card:

1. `/SD:firmware.bin` — re-flashes the STM32 application region (self-programming
   via the Zephyr flash driver).
2. `/SD:ble.bin` + `/SD:FR8010H.FLM` — programs the external **FR8016HA** BLE
   co-processor over **SWD**, using the vendor .FLM flash algorithm.

Updates run in that fixed order (`firmware.bin` first, then BLE). On success the
update file(s) are deleted; on failure they are kept so the next boot retries.

## Where the dependencies live

- Zephyr source: `~/Project/zephyr-rtos/zephyr` (v4.4, branch `main`).
- West workspace topdir: `~/Project/zephyr-rtos` (west v1.5.0, `.venv`).
- Zephyr SDK: `~/zephyr-sdk-1.0.1`.
- The `animula_alonzo` board comes from the parent BSP
  (`~/Project/alonzo-zephyr-bsp`), discovered via `zephyr/module.yml`.

**Never modify anything inside `~/Project/zephyr-rtos`** — treat it as a
read-only dependency. Use its `.venv/bin/west`.

## Build / validate

Run from the west workspace, pointing at this directory as the app source and at
the parent BSP as an extra module:

```sh
cd ~/Project/zephyr-rtos
export ZEPHYR_EXTRA_MODULES=~/Project/alonzo-zephyr-bsp

# normal build
./.venv/bin/west build -b animula_alonzo \
  ~/Project/alonzo-zephyr-bsp/bootloader \
  -d ~/Project/alonzo-zephyr-bsp/bootloader/build

# SWD bring-up self-test build (replaces the BLE update path with a self-test)
./.venv/bin/west build -b animula_alonzo \
  ~/Project/alonzo-zephyr-bsp/bootloader \
  -d ~/Project/alonzo-zephyr-bsp/bootloader/build-selftest \
  -- -DCONFIG_ALONZO_FR8016_SWD_SELFTEST=y
```

Both configs must build with **0 warnings / 0 errors**.

`west flash` / `west debug` target a J-Link or ST-Link (OpenOCD) and need
physical hardware — do not run them in this environment.

## Layout

```
bootloader/
  CMakeLists.txt         app build; no generated FLM image (see below)
  prj.conf               bootloader config (flash placement, console, FAT FS)
  include/               public headers (update, tf, memory_layout, ...)
  src/
    main.c               mount TF -> update_apply -> jump to app
    update.c             orders firmware.bin then ble.bin
    firmware_update.c    STM32 self-programming from /SD:firmware.bin
    ble_update.c         BLE-update gate + selftest branch
    tf.c                 FAT FS mount / file helpers
    led.c, app_jump.c    status LEDs, Cortex-M handoff to the app
    fr8016/              FR8016HA SWD programming (see below)
  vendor/flash_algorithms/FR8010H.FLM   deployable reference, copied to the TF card
```

### FR8016HA SWD programming (`src/fr8016/`)

The STM32 programs the FR8016HA by acting as an SWD master and running the
vendor .FLM flash algorithm in the FR8016HA's own RAM. Layered bottom-up:

- `swd.c` (Phase 1) — bit-banged SWD transport on PA2 (SWCLK) / PA3 (SWDIO).
- `dap.c` (Phase 2) — DP control + MEM-AP word read/write (`mem_read32`/`mem_write32`).
- `cortexm.c` (Phase 3) — Cortex-M debug registers (halt/run/reset, register R/W).
- `flm.c` (Phase 4) — **generic** FLM runtime loader; copies code/data to target
  RAM and calls Init/EraseSector/ProgramPage/UnInit. Knows nothing about the
  FR8016HA flash controller.
- `flm_elf.c` (Phase 4) — runtime .FLM (ELF32) parser; produces `struct flm_image`
  + code/data pointers from a raw .FLM read off the TF card.
- `fr8016.c` (Phase 5) — the update state machine.

Key rules (enforced by design, do not regress):

- The .FLM is **loaded from the TF card at runtime and never compiled in**.
  `vendor/flash_algorithms/FR8010H.FLM` is only a deployable reference. The bootloader
  must stay able to switch flash algorithms by swapping the .FLM on the card with no
  rebuild.
- `flm.c`/`flm.h` must stay **generic**: no FR8016HA-specific QSPI/Flash
  re-implementation (`flash_write`/`flash_erase`/`qspi_flash_init`/`qspi_stig_cmd`).
  The .FLM raw machine code is the single source of truth and executes verbatim.
- The FR8016HA update is **SWD-only** — do not reintroduce the FREQCHIP private
  UART downloader protocol. Normal operation keeps PA2/PA3 as USART2 (TX/RX);
  the update briefly re-muxes them as SWCLK/SWDIO and restores USART2 afterward.
- No FR8016HA RAM hardcoding: the work-area window is `CONFIG_ALONZO_FR8016_RAM_BASE`
  / `CONFIG_ALONZO_FR8016_RAM_SIZE`, laid out dynamically by `fr_work_layout()`.

## FR8016HA hardware facts (from the FR801xH SDK User Guide)

- Cortex-M3 co-processor.
- SRAM base `0x20000000`, size **48 KB** (`0xC000`), range `0x20000000..0x2000BFFF`
  (`0x2000C000` is the exclusive end, used only as a boundary).
- Flash base `0x01000000`, size 512 KB (`0x80000`); erase sector 4 KB (`0x1000`),
  program page 256 B (`0x100`), erased value `0xFF`.
- SWD pins: STM32 PA2 → FR8016HA PC6 (SWCLK), STM32 PA3 ↔ FR8016HA PC7 (SWDIO).
- Kconfig defaults: `CONFIG_ALONZO_FR8016_RAM_BASE=0x20000000`,
  `CONFIG_ALONZO_FR8016_RAM_SIZE=0xC000`.

## Flash layout

`include/memory_layout.h` is the single source of truth; everything derives from
`ALONZO_BOOTLOADER_SIZE` (Kconfig `CONFIG_ALONZO_BOOTLOADER_SIZE`, default `0x20000`):

- Bootloader: `CONFIG_FLASH_LOAD_OFFSET = 0`, `CONFIG_FLASH_LOAD_SIZE = ALONZO_BOOTLOADER_SIZE`.
- Application: linked at `ALONZO_APP_START = FLASH_BASE + ALONZO_BOOTLOADER_SIZE`.

Keep `prj.conf`'s `CONFIG_FLASH_LOAD_SIZE`, the board Kconfig's
`ALONZO_BOOTLOADER_SIZE`, and `memory_layout.h` in agreement (a `BUILD_ASSERT`
catches mismatches). There is no MCUboot / A-B slot / scratch partition.

## Conventions

- Every source file carries the license header:
  `Copyright (c) 2026 HardenedLinux Animula` /
  `Author: Nala Ginrut <roy@hardenedlinux.org>` /
  `SPDX-License-Identifier: GPL-3.0-or-later`.
- No SoC/HAL code here — reuse upstream Zephyr STM32F411 support.
- Keep the bootloader minimal: no MPU / HW stack protection (the application
  re-enables these); the bootloader must hand off cleanly to the app.
- Self-test hooks (`*_selftest`) are bring-up only, gated by
  `CONFIG_ALONZO_FR8016_SWD_SELFTEST`; keep them out of the production path.
