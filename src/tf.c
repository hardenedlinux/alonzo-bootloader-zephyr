/*
 * Copyright (c) 2026 HardenedLinux Animula
 * Author: Nala Ginrut <roy@hardenedlinux.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tf.h"

#include <ff.h>
#include <zephyr/fs/fs.h>

static FATFS tf_fs;
static struct fs_mount_t tf_mp = {
	.type = FS_FATFS,
	.mnt_point = ALONZO_TF_MOUNT_POINT,
	.fs_data = &tf_fs,
};

int tf_mount(void)
{
	return fs_mount(&tf_mp);
}

int tf_unmount(void)
{
	return fs_unmount(&tf_mp);
}

bool tf_file_exists(const char *path, size_t *size_out)
{
	struct fs_dirent entry;
	int rc = fs_stat(path, &entry);

	if (rc != 0) {
		return false;
	}
	if (size_out != NULL) {
		*size_out = entry.size;
	}
	return true;
}
