/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef ALONZO_TF_H
#define ALONZO_TF_H

#include <stdbool.h>
#include <stddef.h>

/* The TF (micro-SD) card carries update files at the root of a FAT volume. */
#define ALONZO_TF_MOUNT_POINT "/SD:"
#define ALONZO_FW_PATH        ALONZO_TF_MOUNT_POINT "/firmware.bin"
#define ALONZO_BLE_PATH       ALONZO_TF_MOUNT_POINT "/ble.bin"
#define ALONZO_FLM_PATH       ALONZO_TF_MOUNT_POINT "/FR8010H.FLM"

/* Mount the TF card FAT filesystem. Returns 0 on success, -errno otherwise. */
int tf_mount(void);

/* Unmount the TF card filesystem. Returns 0 on success, -errno otherwise. */
int tf_unmount(void);

/* Return true if @p path exists and, if @p size_out is non-NULL, its size. */
bool tf_file_exists(const char *path, size_t *size_out);

#endif /* ALONZO_TF_H */
