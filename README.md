## Flash layout

No MCUboot, no A/B slots, no scratch partition. A single **Maker Bootloader**
resides at the start of flash and jumps to a single application image placed
immediately after it.

```
0x08000000 ┌──────────────────────────┐
           │  Maker Bootloader         │  ALONZO_BOOTLOADER_SIZE (default 128 KB)
0x08020000 ├──────────────────────────┤  = FLASH_BASE + BOOTLOADER_SIZE
           │  Application              │  ALONZO_APP_SIZE (default 384 KB)
0x08080000 └──────────────────────────┘
```

`ALONZO_BOOTLOADER_SIZE` is parameterised via `CONFIG_ALONZO_BOOTLOADER_SIZE`
(board `Kconfig.animula_alonzo`), default `0x20000` (128 KB). Valid values are
sector-aligned: 16 / 32 / 48 / 64 / 128 / 256 / 384 / 512 KB. A non-aligned
value is rejected at build time (`memory_layout.h`).

### Build the bootloader

```sh
west build -b animula_alonzo /path/to/alonzo-zephyr-bsp/bootloader
```

### Build the application (linked after the bootloader)

The application must be linked at `FLASH_BASE + BOOTLOADER_SIZE`. Use the
provided fragment, or pass the two linker symbols directly:

```sh
west build -b animula_alonzo samples/hello_world \
  -- -DEXTRA_CONF_FILE=/path/to/alonzo-zephyr-bsp/configs/app.conf
# equivalent:
west build -b animula_alonzo samples/hello_world \
  -- -DCONFIG_FLASH_LOAD_OFFSET=0x20000 -DCONFIG_FLASH_LOAD_SIZE=0x60000
```

## Flash / debug

```sh
west flash        # J-Link (default) or OpenOCD/ST-Link
west debug        # attach a debugger
```

## License

LGPL-3.0-or-later. See `COPYING.LESSER` (the LGPLv3 text) and `COPYING`
(the GPLv3 text it incorporates).

Copyright (c) 2026 HardenedLinux Animula.


