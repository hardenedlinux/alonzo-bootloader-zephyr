# Copyright (c) 2026 HardenedLinux Animula
# Author: Nala Ginrut <roy@hardenedlinux.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alonzo_bootloader)

target_include_directories(app PRIVATE include src/fr8016)

# The FR8016HA flash algorithm (vendor .FLM) is NOT built into the bootloader:
# it is read from the TF card at runtime and parsed by src/fr8016/flm_elf.c.
# The file under vendor/flash_algorithms/ is only a deployable reference to copy
# onto the card.

target_sources(app PRIVATE
  src/main.c
  src/update.c
  src/firmware_update.c
  src/ble_update.c
  src/app_jump.c
  src/led.c
  src/tf.c
  src/fr8016/swd.c
  src/fr8016/dap.c
  src/fr8016/cortexm.c
  src/fr8016/flm.c
  src/fr8016/flm_elf.c
  src/fr8016/fr8016.c
)
