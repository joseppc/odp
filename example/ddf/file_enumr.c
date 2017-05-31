/* Copyright (c) 2017, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <odp_drv.h>

#include "file_enumr.h"

static odpdrv_enumr_class_t file_class_handle = ODPDRV_ENUMR_CLASS_INVALID;
static int file_enumr_register(const char *);

static int file_class_probe(void)
{
	char *path, *p;

	path = getenv(FILE_ENUMR_ENV_VAR);
	if (path == NULL)
		return 1;

	while (p = strchr(path, ':')) {
		*p = '\0';
		file_enumr_register(path);
		*p = ':';
		path = p + 1;
	}
	file_enumr_register(path);

	return 0;
}

static int file_class_remove(void)
{
	return 0;
}

int file_class_enumr_register(void)
{
	static struct odpdrv_enumr_class_param_t param = {
		.name = FILE_ENUMR_CLASS_NAME,
		.probe = file_class_probe,
		.remove = file_class_remove
	};

	if (file_class_handle != ODPDRV_ENUMR_CLASS_INVALID) {
		ODP_ERR(FILE_ENUMR_CLASS_NAME " class already registered\n");
		return -2;
	}

	file_class_handle = odpdrv_enumr_class_register(&param);
	if (file_class_handle == ODPDRV_ENUMR_CLASS_INVALID) {
		ODP_ERR("Failed to register class " FILE_ENUMR_CLASS_NAME "\n");
		return -1;
	}

	return 0;
}

static void ODPDRV_CONSTRUCTOR file_class_enumr_register_constructor(void)
{
	file_class_enumr_register();
}

struct file_dev_data {
	odpdrv_device_t dev_handle;
	int index;
	char path[1];
};

struct file_enumr_data {
	odpdrv_enumr_t handle;
	struct dirent **namelist;
	int num_entries;
	char path[1];
};

static int filter(const struct dirent *de)
{
	struct stat st;

	stat(de->d_name, &st);
	return S_ISREG(st.st_mode);
}

static odpdrv_device_t register_file_device(struct file_enumr_data *enumr_data,
					    int index)
{
	struct odpdrv_device_param_t dp;
	struct file_dev_data *dev_data;
	size_t size;

	size = sizeof(*dev_data) +
	       strlen(data->path) +
	       strlen(data->namelist[index]) +
	       1;
	dev_data = malloc(size); /* FIXME: use ishmem */
	if (dev_data == NULL)
		return ODPDRV_DEVICE_INVALID;

	sprintf(dev_data->path, size, "%s/%s", enumr_data->path,
		enumr_data->namelist[index]);

	dp.enumerator = enumr_data->handle;
	memcpy(dp.address, index, sizeof(index));
	dp.enum_dev = dev_data;

	dev_data->dev_handle = odpdrv_device_create(&dp);
	if (data->dev_hanle == ODPDRV_DEVICE_INVALID) {
		free(data);
		return ODPDRV_DEVICE_INVALID;
	}

	return data->dev_hanle;
}

/* for now scan only one directory, do not recurse */
static int file_enumr_probe(void *priv)
{
	struct file_enumr_data *data = (struct file_enumr_data *)priv;

	if (data->namelist != NULL)
		return -1;

	data->num_entries = scandir(data->path, &data->namelist, filter,
				    alphasort);

	if (data->num_entries == -1)
		return -1;

	for (int i = 0; i < data->num_entries; i++) {
		/* FIXME: check return value */
		register_file_device(data, i);
	}

	return 0;
}

static int file_enumr_remove(void *priv)
{
	struct file_enumr_data *data = (struct file_enumr_data *)priv;

	/* FIXME: this can result in num_entries = -1 */
	while (data->num_entries--)
		free(data->namelist[data->num_entries]);

	free(data->namelist);
	free(data);

	return 0;
}

/* what calls this function? */
static int file_enumr_register_notifier(void (*event_handler)(uint64_t event),
					int64_t event_mask)
{
	/* NOTE: use inotify */
	return 0;
}

/*
 * This enumerator, lists all files in a directory. Each file is considered
 * to be a "device". Later when a driver is registered, all files will be
 * passed to the device for probing.
 */
static int file_enumr_register(const char *path)
{
	/* FIXME: we can do this because register does a shallow copy
	 * of this structure, but otherwise passing a pointer allocated
	 * from the stack might be problematic, we need to review this.
	 */
	struct odpdrv_enumr_param_t param = {
		.enumr_class = file_class_handle;
		.api_name = FILE_ENUMR_API_NAME,
		.api_version = FILE_ENUMR_API_VERSION,
		.probe = file_enumr_probe,
		.remove = file_enumr_remove,
		.register_notifier = file_enumr_register_notifier
	};
	struct file_enumr_data *data;

	if (path == NULL || strlen(path) == 0)
		return -2; /* EINVAL */

	/* FIXME: allocated using ishmem */
	data = malloc(sizeof(*data) + strlen(path));
	if (data == NULL)
		return -1; /* ENOMEM */
	memset(data, '0', sizeof(*data));

	strcpy(data->path, path);
	param->probe_data = data;

	data->handle = odpdrv_enumr_register(&param);
	if (data->handle == ODPDRV_ENUMR_INVALID) {
		free(data);
		return -1;
	}

	return 0;
}
