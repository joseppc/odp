#/* Copyright (c) 2017, Linaro Limited
* All rights reserved.
*
* SPDX-License-Identifier:     BSD-3-Clause
*/

/* Many of this functions have been inspired by their dpdk counterpart,
 * hence the following copyright and license:
 */

/*
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/**
 * @file
 * PCI interface for VFIO drivers
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>

#if !defined (ARM_ARCHITECTURE)
#include <sys/io.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <linux/limits.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/version.h>
#include <linux/pci_regs.h>
#include <linux/vfio.h>
#include <_str_functions_internal.h>

#include <odp_posix_extensions.h>
#include <odp_config_internal.h>
#include <odp_internal.h>
#include <odp/drv/shm.h>
#include <odp_debug_internal.h>
#include <odp_bitmap_internal.h>
#include <drv_pci_internal.h>
#include <_str_functions_internal.h>

#define VFIO_MAX_GROUPS 64
#define VFIO_DIR "/dev/vfio"
#define VFIO_CONTAINER_PATH "/dev/vfio/vfio"
#define VFIO_GROUP_FMT "/dev/vfio/%u"
#define VFIO_NOIOMMU_GROUP_FMT "/dev/vfio/noiommu-%u"
#define VFIO_GET_REGION_ADDR(x) ((uint64_t) x << 40ULL)
#define VFIO_GET_REGION_IDX(x) (x >> 40)
#define VFIO_NOIOMMU_MODE      \
	"/sys/module/vfio/parameters/enable_unsafe_noiommu_mode"

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
#define VFIO_NOIOMMU_IOMMU 8
#endif

/*
 * we don't need to store device fd's anywhere since they can be obtained from
 * the group fd via an ioctl() call.
 */
struct vfio_group {
	int group_no;
	int fd;
	int devices;
};

struct vfio_config {
	int vfio_enabled;
	int vfio_container_fd;
	int vfio_active_groups;
	struct vfio_group vfio_groups[VFIO_MAX_GROUPS];
};

/* DMA mapping function prototype.
 * Takes VFIO container fd as a parameter.
 * Returns 0 on success, -1 on error.
 * */
typedef int (*vfio_dma_func_t)(int);

struct vfio_iommu_type {
	int type_id;
	const char *name;
	vfio_dma_func_t dma_map_func;
};

typedef struct user_access_context_t {
	int vfio_dev_fd;
	int uio_num; /**< device number assigned by UIO: /dev/uioX */
	int uio_fd; /**< file descriptor for /dev/uioX */
	int uio_cfg_fd; /**< file descriptor for accessing device's config space */
	int nb_maps;
	struct pci_map maps[PCI_MAX_RESOURCE];
	char path[PATH_MAX];
} user_access_context_t;

static int vfio_type1_dma_map(int);
static int vfio_noiommu_dma_map(int);

/* pick IOMMU type. returns a pointer to vfio_iommu_type or NULL for error */
const struct vfio_iommu_type *
vfio_set_iommu_type(int vfio_container_fd);

/* check if we have any supported extensions */
int vfio_has_supported_extensions(int vfio_container_fd);

/* open container fd or get an existing one */
int vfio_get_container_fd(void);

/* parse IOMMU group number for a device
 *  * returns 1 on success, -1 for errors, 0 for non-existent group
 *   */
int vfio_get_group_no(const char *sysfs_base,
	const char *dev_addr, int *iommu_group_no);

/* open group fd or get an existing one */
int vfio_get_group_fd(int iommu_group_no);

/* remove group fd from internal VFIO group fd array */
int clear_group(int vfio_group_fd);

int vfio_enable(void);
int vfio_is_enabled(void);
int vfio_noiommu_is_enabled(void);
int vfio_setup_device(const char *sysfs_base, const char *dev_addr,
		int *vfio_dev_fd, struct vfio_device_info *device_info);
int vfio_release_device(const char *sysfs_base, const char *dev_addr, int fd);

/* IOMMU types we support */
static const struct vfio_iommu_type iommu_types[] = {
	/* x86 IOMMU, otherwise known as type 1 */
	{ VFIO_TYPE1_IOMMU, "Type 1", &vfio_type1_dma_map},
	/* IOMMU-less mode */
	{ VFIO_NOIOMMU_IOMMU, "No-IOMMU", &vfio_noiommu_dma_map},
};

static struct vfio_config vfio_cfg;

int
vfio_get_group_fd(int iommu_group_no)
{
	int i;
	int vfio_group_fd;
	char filename[PATH_MAX];
	struct vfio_group *cur_grp;

	/* check if we already have the group descriptor open */
	for (i = 0; i < VFIO_MAX_GROUPS; i++)
		if (vfio_cfg.vfio_groups[i].group_no == iommu_group_no)
			return vfio_cfg.vfio_groups[i].fd;

	/* Lets see first if there is room for a new group */
	if (vfio_cfg.vfio_active_groups == VFIO_MAX_GROUPS) {
		ODP_ERR("Maximum number of VFIO groups reached!\n");
		return -1;
	}

	/* Now lets get an index for the new group */
	for (i = 0; i < VFIO_MAX_GROUPS; i++)
		if (vfio_cfg.vfio_groups[i].group_no == -1) {
			cur_grp = &vfio_cfg.vfio_groups[i];
			break;
		}

	/* This should not happen */
	if (i == VFIO_MAX_GROUPS) {
		ODP_ERR("No VFIO group free slot found\n");
		return -1;
	}

	/* try regular group format */
	snprintf(filename, sizeof(filename),
			 VFIO_GROUP_FMT, iommu_group_no);
	vfio_group_fd = open(filename, O_RDWR);
	if (vfio_group_fd < 0) {
		/* if file not found, it's not an error */
		if (errno != ENOENT) {
			ODP_ERR("Cannot open %s: %s\n", filename,
					strerror(errno));
			return -1;
		}

		/* special case: try no-IOMMU path as well */
		snprintf(filename, sizeof(filename),
				VFIO_NOIOMMU_GROUP_FMT, iommu_group_no);
		vfio_group_fd = open(filename, O_RDWR);
		if (vfio_group_fd < 0) {
			if (errno != ENOENT) {
				ODP_ERR("Cannot open %s: %s\n", filename,
						strerror(errno));
				return -1;
			}
			return 0;
		}
		/* noiommu group found */
	}

	cur_grp->group_no = iommu_group_no;
	cur_grp->fd = vfio_group_fd;
	vfio_cfg.vfio_active_groups++;
	return vfio_group_fd;
}


static int
get_vfio_group_idx(int vfio_group_fd)
{
	int i;
	for (i = 0; i < VFIO_MAX_GROUPS; i++)
		if (vfio_cfg.vfio_groups[i].fd == vfio_group_fd)
			return i;
	return -1;
}

static void
vfio_group_device_get(int vfio_group_fd)
{
	int i;

	i = get_vfio_group_idx(vfio_group_fd);
	if (i < 0 || i > (VFIO_MAX_GROUPS - 1))
		ODP_ERR("  wrong vfio_group index (%d)\n", i);
	else
		vfio_cfg.vfio_groups[i].devices++;
}

static void
vfio_group_device_put(int vfio_group_fd)
{
	int i;

	i = get_vfio_group_idx(vfio_group_fd);
	if (i < 0 || i > (VFIO_MAX_GROUPS - 1))
		ODP_ERR("  wrong vfio_group index (%d)\n", i);
	else
		vfio_cfg.vfio_groups[i].devices--;
}

static int
vfio_group_device_count(int vfio_group_fd)
{
	int i;

	i = get_vfio_group_idx(vfio_group_fd);
	if (i < 0 || i > (VFIO_MAX_GROUPS - 1)) {
		ODP_ERR("  wrong vfio_group index (%d)\n", i);
		return -1;
	}

	return vfio_cfg.vfio_groups[i].devices;
}

int
clear_group(int vfio_group_fd)
{
	int i;


	i = get_vfio_group_idx(vfio_group_fd);
	if (i < 0)
		return -1;
	vfio_cfg.vfio_groups[i].group_no = -1;
	vfio_cfg.vfio_groups[i].fd = -1;
	vfio_cfg.vfio_groups[i].devices = 0;
	vfio_cfg.vfio_active_groups--;
	return 0;
}

int
vfio_setup_device(const char *sysfs_base, const char *dev_addr,
		int *vfio_dev_fd, struct vfio_device_info *device_info)
{
	struct vfio_group_status group_status = {
			.argsz = sizeof(group_status)
	};
	int vfio_group_fd;
	int iommu_group_no;
	int ret;

	/* get group number */
	ret = vfio_get_group_no(sysfs_base, dev_addr, &iommu_group_no);
	if (ret == 0) {
		ODP_ERR("  %s not managed by VFIO driver, skipping\n",
			dev_addr);
		return 1;
	}

	/* if negative, something failed */
	if (ret < 0)
		return -1;

	/* get the actual group fd */
	vfio_group_fd = vfio_get_group_fd(iommu_group_no);
	if (vfio_group_fd < 0)
		return -1;

	/* if group_fd == 0, that means the device isn't managed by VFIO */
	if (vfio_group_fd == 0) {
		ODP_ERR(" %s not managed by VFIO driver, skipping\n",
				dev_addr);
		return 1;
	}

	/*
	 * at this point, we know that this group is viable (meaning, all devices
	 * are either bound to VFIO or not bound to anything)
	 */

	/* check if the group is viable */
	ret = ioctl(vfio_group_fd, VFIO_GROUP_GET_STATUS, &group_status);
	if (ret) {
		ODP_ERR("  %s cannot get group status, "
				"error %i (%s)\n", dev_addr, errno, strerror(errno));
		close(vfio_group_fd);
		clear_group(vfio_group_fd);
		return -1;
	} else if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		ODP_ERR("  %s VFIO group is not viable!\n", dev_addr);
		close(vfio_group_fd);
		clear_group(vfio_group_fd);
		return -1;
	}

	/* check if group does not have a container yet */
	if (!(group_status.flags & VFIO_GROUP_FLAGS_CONTAINER_SET)) {

		/* add group to a container */
		ret = ioctl(vfio_group_fd, VFIO_GROUP_SET_CONTAINER,
				&vfio_cfg.vfio_container_fd);
		if (ret) {
			ODP_ERR("  %s cannot add VFIO group to container, "
					"error %i (%s)\n", dev_addr, errno, strerror(errno));
			close(vfio_group_fd);
			clear_group(vfio_group_fd);
			return -1;
		}

		/*
		 * pick an IOMMU type and set up DMA mappings for container
		 *
		 * needs to be done only once, only when first group is
		 * assigned to a container and only in primary process.
		 * Note this can happen several times with the hotplug
		 * functionality.
		 */
		if (vfio_cfg.vfio_active_groups == 1) {
			/* select an IOMMU type which we will be using */
			const struct vfio_iommu_type *t =
				vfio_set_iommu_type(vfio_cfg.vfio_container_fd);
			if (!t) {
				ODP_ERR("  %s failed to select IOMMU type\n",
					dev_addr);
				close(vfio_group_fd);
				clear_group(vfio_group_fd);
				return -1;
			}
			ret = t->dma_map_func(vfio_cfg.vfio_container_fd);
			if (ret) {
				ODP_ERR("  %s DMA remapping failed, error %i (%s)\n",
					dev_addr, errno, strerror(errno));
				close(vfio_group_fd);
				clear_group(vfio_group_fd);
				return -1;
			}
		}
	}

	/* get a file descriptor for the device */
	*vfio_dev_fd = ioctl(vfio_group_fd, VFIO_GROUP_GET_DEVICE_FD, dev_addr);
	if (*vfio_dev_fd < 0) {
		/* if we cannot get a device fd, this implies a problem with
		 * the VFIO group or the container not having IOMMU configured.
		 */

		ODP_ERR("Getting a vfio_dev_fd for %s failed\n",
				dev_addr);
		close(vfio_group_fd);
		clear_group(vfio_group_fd);
		return -1;
	}

	/* test and setup the device */
	ret = ioctl(*vfio_dev_fd, VFIO_DEVICE_GET_INFO, device_info);
	if (ret) {
		ODP_ERR("  %s cannot get device info, "
				"error %i (%s)\n", dev_addr, errno,
				strerror(errno));
		close(*vfio_dev_fd);
		close(vfio_group_fd);
		clear_group(vfio_group_fd);
		return -1;
	}
	vfio_group_device_get(vfio_group_fd);

	return 0;
}

int
vfio_release_device(const char *sysfs_base, const char *dev_addr,
		    int vfio_dev_fd)
{
	struct vfio_group_status group_status = {
			.argsz = sizeof(group_status)
	};
	int vfio_group_fd;
	int iommu_group_no;
	int ret;

	/* get group number */
	ret = vfio_get_group_no(sysfs_base, dev_addr, &iommu_group_no);
	if (ret <= 0) {
		ODP_ERR("  %s not managed by VFIO driver\n",
			dev_addr);
		/* This is an error at this point. */
		return -1;
	}

	/* get the actual group fd */
	vfio_group_fd = vfio_get_group_fd(iommu_group_no);
	if (vfio_group_fd <= 0) {
		ODP_PRINT("vfio_get_group_fd failed for %s\n",
				   dev_addr);
		return -1;
	}

	/* At this point we got an active group. Closing it will make the
	 * container detachment. If this is the last active group, VFIO kernel
	 * code will unset the container and the IOMMU mappings.
	 */

	/* Closing a device */
	if (close(vfio_dev_fd) < 0) {
		ODP_PRINT("Error when closing vfio_dev_fd for %s\n",
				   dev_addr);
		return -1;
	}

	/* An VFIO group can have several devices attached. Just when there is
	 * no devices remaining should the group be closed.
	 */
	vfio_group_device_put(vfio_group_fd);
	if (!vfio_group_device_count(vfio_group_fd)) {

		if (close(vfio_group_fd) < 0) {
			ODP_PRINT("Error when closing vfio_group_fd for %s\n",
				dev_addr);
			return -1;
		}

		if (clear_group(vfio_group_fd) < 0) {
			ODP_PRINT("Error when clearing group for %s\n",
					   dev_addr);
			return -1;
		}
	}

	return 0;
}

int
vfio_enable(void)
{
	/* initialize group list */
	int i;

	for (i = 0; i < VFIO_MAX_GROUPS; i++) {
		vfio_cfg.vfio_groups[i].fd = -1;
		vfio_cfg.vfio_groups[i].group_no = -1;
		vfio_cfg.vfio_groups[i].devices = 0;
	}

	/* inform the user that we are probing for VFIO */
	ODP_PRINT("Probing VFIO support...\n");

	vfio_cfg.vfio_container_fd = vfio_get_container_fd();

	/* check if we have VFIO driver enabled */
	if (vfio_cfg.vfio_container_fd != -1) {
		ODP_PRINT("VFIO support initialized\n");
		vfio_cfg.vfio_enabled = 1;
	} else {
		ODP_PRINT("VFIO support could not be initialized\n");
	}

	return 0;
}

int
vfio_is_enabled(void)
{
	return vfio_cfg.vfio_enabled;
}

const struct vfio_iommu_type *
vfio_set_iommu_type(int vfio_container_fd)
{
	unsigned idx;
	for (idx = 0; idx < ARRAY_SIZE(iommu_types); idx++) {
		const struct vfio_iommu_type *t = &iommu_types[idx];

		int ret = ioctl(vfio_container_fd, VFIO_SET_IOMMU,
				t->type_id);
		if (!ret) {
			ODP_PRINT("  using IOMMU type %d (%s)\n",
					t->type_id, t->name);
			return t;
		}
		/* not an error, there may be more supported IOMMU types */
		ODP_PRINT("  set IOMMU type %d (%s) failed, "
				"error %i (%s)\n", t->type_id, t->name, errno,
				strerror(errno));
	}
	/* if we didn't find a suitable IOMMU type, fail */
	return NULL;
}

int
vfio_has_supported_extensions(int vfio_container_fd)
{
	int ret;
	unsigned idx, n_extensions = 0;
	for (idx = 0; idx < ARRAY_SIZE(iommu_types); idx++) {
		const struct vfio_iommu_type *t = &iommu_types[idx];

		ret = ioctl(vfio_container_fd, VFIO_CHECK_EXTENSION,
				t->type_id);
		if (ret < 0) {
			ODP_ERR("  could not get IOMMU type, "
				"error %i (%s)\n", errno,
				strerror(errno));
			close(vfio_container_fd);
			return -1;
		} else if (ret == 1) {
			/* we found a supported extension */
			n_extensions++;
		}
		ODP_PRINT("  IOMMU type %d (%s) is %s\n",
				t->type_id, t->name,
				ret ? "supported" : "not supported");
	}

	/* if we didn't find any supported IOMMU types, fail */
	if (!n_extensions) {
		close(vfio_container_fd);
		return -1;
	}

	return 0;
}

int
vfio_get_container_fd(void)
{
	int ret, vfio_container_fd;

	vfio_container_fd = open(VFIO_CONTAINER_PATH, O_RDWR);
	if (vfio_container_fd < 0) {
		ODP_ERR("  cannot open VFIO container, "
				"error %i (%s)\n", errno, strerror(errno));
		return -1;
	}

	/* check VFIO API version */
	ret = ioctl(vfio_container_fd, VFIO_GET_API_VERSION);
	if (ret != VFIO_API_VERSION) {
		if (ret < 0)
			ODP_ERR("  could not get VFIO API version, "
					"error %i (%s)\n", errno, strerror(errno));
		else
			ODP_ERR("  unsupported VFIO API version!\n");
		close(vfio_container_fd);
		return -1;
	}

	ret = vfio_has_supported_extensions(vfio_container_fd);
	if (ret) {
		ODP_ERR("  no supported IOMMU "
				"extensions found!\n");
		return -1;
	}

	return vfio_container_fd;
}

int
vfio_get_group_no(const char *sysfs_base,
		const char *dev_addr, int *iommu_group_no)
{
	char linkname[PATH_MAX];
	char filename[PATH_MAX];
	char *tok[16], *group_tok, *end;
	int ret;

	memset(linkname, 0, sizeof(linkname));
	memset(filename, 0, sizeof(filename));

	/* try to find out IOMMU group for this device */
	snprintf(linkname, sizeof(linkname),
			 "%s/%s/iommu_group", sysfs_base, dev_addr);

	ret = readlink(linkname, filename, sizeof(filename));

	/* if the link doesn't exist, no VFIO for us */
	if (ret < 0)
		return 0;

	ret = _odp_strsplit(filename, sizeof(filename),
			tok, RTE_DIM(tok), '/');

	if (ret <= 0) {
		ODP_ERR("  %s cannot get IOMMU group\n", dev_addr);
		return -1;
	}

	/* IOMMU group is always the last token */
	errno = 0;
	group_tok = tok[ret - 1];
	end = group_tok;
	*iommu_group_no = strtol(group_tok, &end, 10);
	if ((end != group_tok && *end != '\0') || errno != 0) {
		ODP_ERR("  %s error parsing IOMMU number!\n", dev_addr);
		return -1;
	}

	return 1;
}

static int
vfio_type1_dma_map(int vfio_container_fd ODP_UNUSED)
{
	/* TODO */

	return 0;
}

static int
vfio_noiommu_dma_map(int vfio_container_fd ODP_UNUSED)
{
	/* No-IOMMU mode does not need DMA mapping */
	return 0;
}

int
vfio_noiommu_is_enabled(void)
{
	int fd, ret, cnt ODP_UNUSED;
	char c;

	ret = -1;
	fd = open(VFIO_NOIOMMU_MODE, O_RDONLY);
	if (fd < 0)
		return -1;

	cnt = read(fd, &c, 1);
	if (c == 'Y')
		ret = 1;

	close(fd);
	return ret;
}

#if 0
int
pci_vfio_read_config(const struct rte_intr_handle *intr_handle,
		    void *buf, size_t len, off_t offs)
{
	return pread64(intr_handle->vfio_dev_fd, buf, len,
	       VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) + offs);
}

int
pci_vfio_write_config(const struct rte_intr_handle *intr_handle,
		    const void *buf, size_t len, off_t offs)
{
	return pwrite64(intr_handle->vfio_dev_fd, buf, len,
	       VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) + offs);
}
#endif

/* get PCI BAR number where MSI-X interrupts are */
static int
pci_vfio_get_msix_bar(int fd, int *msix_bar, uint32_t *msix_table_offset,
		      uint32_t *msix_table_size)
{
	int ret;
	uint32_t reg;
	uint16_t flags;
	uint8_t cap_id, cap_offset;

	/* read PCI capability pointer from config space */
	ret = pread64(fd, &reg, sizeof(reg),
			VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
			PCI_CAPABILITY_LIST);
	if (ret != sizeof(reg)) {
		ODP_ERR("Cannot read capability pointer from PCI "
				"config space!\n");
		return -1;
	}

	/* we need first byte */
	cap_offset = reg & 0xFF;

	while (cap_offset) {

		/* read PCI capability ID */
		ret = pread64(fd, &reg, sizeof(reg),
				VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
				cap_offset);
		if (ret != sizeof(reg)) {
			ODP_ERR("Cannot read capability ID from PCI "
					"config space!\n");
			return -1;
		}

		/* we need first byte */
		cap_id = reg & 0xFF;

		/* if we haven't reached MSI-X, check next capability */
		if (cap_id != PCI_CAP_ID_MSIX) {
			ret = pread64(fd, &reg, sizeof(reg),
					VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
					cap_offset);
			if (ret != sizeof(reg)) {
				ODP_ERR("Cannot read capability pointer from PCI "
						"config space!\n");
				return -1;
			}

			/* we need second byte */
			cap_offset = (reg & 0xFF00) >> 8;

			continue;
		}
		/* else, read table offset */
		else {
			/* table offset resides in the next 4 bytes */
			ret = pread64(fd, &reg, sizeof(reg),
					VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
					cap_offset + 4);
			if (ret != sizeof(reg)) {
				ODP_ERR("Cannot read table offset from PCI config "
						"space!\n");
				return -1;
			}

			ret = pread64(fd, &flags, sizeof(flags),
					VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
					cap_offset + 2);
			if (ret != sizeof(flags)) {
				ODP_ERR("Cannot read table flags from PCI config "
						"space!\n");
				return -1;
			}

			*msix_bar = reg & PCI_MSIX_TABLE_BIR;
			*msix_table_offset = reg & PCI_MSIX_TABLE_OFFSET;
			*msix_table_size =
				16 * (1 + (flags & PCI_MSIX_FLAGS_QSIZE));

			return 0;
		}
	}
	return 0;
}

/* set PCI bus mastering */
static int
pci_vfio_set_bus_master(int dev_fd, bool op)
{
	uint16_t reg;
	int ret;

	ret = pread64(dev_fd, &reg, sizeof(reg),
			VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
			PCI_COMMAND);
	if (ret != sizeof(reg)) {
		ODP_ERR("Cannot read command from PCI config space!\n");
		return -1;
	}

	if (op)
		/* set the master bit */
		reg |= PCI_COMMAND_MASTER;
	else
		reg &= ~(PCI_COMMAND_MASTER);

	ret = pwrite64(dev_fd, &reg, sizeof(reg),
			VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
			PCI_COMMAND);

	if (ret != sizeof(reg)) {
		ODP_ERR("Cannot write command to PCI config space!\n");
		return -1;
	}

	return 0;
}

static int
pci_vfio_is_ioport_bar(int vfio_dev_fd, int bar_index)
{
	uint32_t ioport_bar;
	int ret;

	ret = pread64(vfio_dev_fd, &ioport_bar, sizeof(ioport_bar),
			  VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX)
			  + PCI_BASE_ADDRESS_0 + bar_index*4);
	if (ret != sizeof(ioport_bar)) {
		ODP_ERR("Cannot read command (%x) from config space!\n",
			PCI_BASE_ADDRESS_0 + bar_index*4);
		return -1;
	}

	return (ioport_bar & PCI_BASE_ADDRESS_SPACE_IO) != 0;
}

static int
pci_vfio_setup_device(struct pci_dev_t *dev ODP_UNUSED, int vfio_dev_fd)
{
	/* set bus mastering for the device */
	if (pci_vfio_set_bus_master(vfio_dev_fd, true)) {
		ODP_ERR("Cannot set up bus mastering!\n");
		return -1;
	}

	/*
	 * Reset the device. If the device is not capable of resetting,
	 * then it updates errno as EINVAL.
	 */
	if (ioctl(vfio_dev_fd, VFIO_DEVICE_RESET) && errno != EINVAL) {
		ODP_ERR("Unable to reset device! Error: %d (%s)\n",
				errno, strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * map the PCI resources of a PCI device in virtual memory (VFIO version).
 */
static int
pci_vfio_map_resource(struct pci_dev_t *dev)
{
	char pci_addr[PATH_MAX] = {0};
	int vfio_dev_fd;
	pci_addr_t *loc = &dev->addr;
	int i, ret, msix_bar;

	uint32_t msix_table_offset = 0;
	uint32_t msix_table_size = 0;

	/* store PCI address string */
	snprintf(pci_addr, sizeof(pci_addr), PCI_PRI_FMT,
			loc->domain, loc->bus, loc->devid, loc->function);

	vfio_dev_fd = dev->user_access_context->vfio_dev_fd;

	/* get MSI-X BAR, if any (we have to know where it is because we can't
	 * easily mmap it when using VFIO)
	 */
	msix_bar = -1;
	ret = pci_vfio_get_msix_bar(vfio_dev_fd, &msix_bar,
				&msix_table_offset, &msix_table_size);
	if (ret < 0) {
		ODP_ERR("  %s cannot get MSI-X BAR number!\n",
				pci_addr);
		goto err_vfio_dev_fd;
	}

	for (i = 0; i < PCI_MAX_RESOURCE; i++) {
		struct vfio_region_info reg = { .argsz = sizeof(reg) };
		void *bar_addr;

		reg.index = i;

		ret = ioctl(vfio_dev_fd, VFIO_DEVICE_GET_REGION_INFO, &reg);
		if (ret) {
			ODP_ERR("  %s cannot get device region info "
					"error %i (%s)\n", pci_addr, errno,
					strerror(errno));
			goto err_vfio_dev_fd;
		}

		/* chk for io port region */
		ret = pci_vfio_is_ioport_bar(vfio_dev_fd, i);
		if (ret < 0)
			goto err_vfio_dev_fd;
		else if (ret) {
			ODP_PRINT("Ignore mapping IO port bar(%d)\n", i);
			continue;
		}

		/* skip non-mmapable BARs */
		if ((reg.flags & VFIO_REGION_INFO_FLAG_MMAP) == 0)
			continue;

		if (i == msix_bar) {
			/* TODO: interrupt */
			continue;
		}

		bar_addr = mmap(0, reg.size, PROT_READ | PROT_WRITE, MAP_SHARED,
				vfio_dev_fd, reg.offset);

		if (bar_addr == MAP_FAILED) {
			ODP_ERR("  %s mapping BAR%i failed: %s\n", pci_addr, i,
					strerror(errno));
			close(vfio_dev_fd);
			return -1;
		}

		dev->bar[i].addr = bar_addr;
		dev->bar[i].len = reg.size;
	}

	if (pci_vfio_setup_device(dev, vfio_dev_fd) < 0) {
		ODP_ERR("  %s setup device failed\n", pci_addr);
		goto err_vfio_dev_fd;
	}

	return 0;
err_vfio_dev_fd:
	close(vfio_dev_fd);
	return -1;
}

static int
pci_vfio_unmap_resource(struct pci_dev_t *dev)
{
	char pci_addr[PATH_MAX] = {0};
	struct pci_addr_t *loc = &dev->addr;
	int i, ret;

	/* store PCI address string */
	snprintf(pci_addr, sizeof(pci_addr), PCI_PRI_FMT,
			loc->domain, loc->bus, loc->devid, loc->function);

	if (pci_vfio_set_bus_master(dev->user_access_context->vfio_dev_fd,
				    false)) {
		ODP_ERR("  %s cannot unset bus mastering for PCI device!\n",
				pci_addr);
		return -1;
	}

	ret = vfio_release_device(pci_get_sysfs_path(), pci_addr,
				  dev->user_access_context->vfio_dev_fd);
	if (ret < 0) {
		ODP_ERR("%s(): cannot release device\n", __func__);
		return ret;
	}

	ODP_PRINT("Releasing pci mapped resource for %s\n",
		pci_addr);
	for (i = 0; i < PCI_MAX_RESOURCE; i++) {

		/*
		 * We do not need to be aware of MSI-X table BAR mappings as
		 * when mapping. Just using current maps array is enough
		 */
		if (dev->bar[i].addr) {
			// ODP_PRINT("Calling pci_unmap_resource for %s at %p\n",
		//		pci_addr, maps[i].addr);
			munmap(dev->bar[i].addr, dev->bar[i].len);
		}
	}

	return 0;
}

static int
pci_vfio_prepare_context(pci_dev_t *dev)
{
	int vfio_dev_fd, ret;
	char pci_addr[PATH_MAX] = {0};
	pci_addr_t *loc = &dev->addr;
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	user_access_context_t *ctx;

	ctx = malloc(sizeof(user_access_context_t));
	if (ctx == NULL)
		return -1;

	memset(ctx, 0, sizeof(user_access_context_t));

	/* store PCI address string */
	snprintf(pci_addr, sizeof(pci_addr), PCI_PRI_FMT,
			loc->domain, loc->bus, loc->devid, loc->function);

	ret = vfio_setup_device(pci_get_sysfs_path(), pci_addr,
					&vfio_dev_fd, &device_info);
	if (ret) {
		free(ctx);
		return ret;
	}

	ctx->vfio_dev_fd = vfio_dev_fd;
	dev->user_access_context = ctx;

	return 0;
}

static int
pci_vfio_probe(struct pci_dev_t *dev)
{
	int ret;

	if (!vfio_is_enabled())
		vfio_enable();

	ret = pci_vfio_prepare_context(dev);
	if (ret)
		return ret;

	ret = pci_vfio_map_resource(dev);

	return ret;
}

const user_access_ops_t vfio_access_ops = {
	.probe           = pci_vfio_probe,
	.map_resource    = pci_vfio_map_resource,
	.unmap_resource  = pci_vfio_unmap_resource,
	.read_config     = NULL,
	.write_config    = NULL,
	.ioport_map      = NULL,
	.ioport_unmap    = NULL,
	.ioport_read     = NULL,
	.ioport_write    = NULL
};
