/* Copyright (c) 2016, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

 /*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
 /*   BSD LICENSE
 *
 *   Copyright 2013-2014 6WIND S.A.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of 6WIND S.A. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef DRV_PCI_INTERNAL_H_
#define DRV_PCI_INTERNAL_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define unlikely(x) (x)
#define likely(x) (x)

#define PATH_MAX 4096

/* path where PCI devices are shown in sysfs: */
#define PCI_SYSFS_DEVICES_ROOT "/sys/bus/pci/devices"

/* Nb. of values in PCI resource file format. */
#define PCI_RESOURCE_FMT_NVAL 3
/** Nb. of values in PCI device address string. */
#define PCI_FMT_NVAL 4

/* Maximum number of PCI resources (BAR regions). */
#define PCI_MAX_RESOURCE 6

/* IO resource type (flag section of PCI resource file): */
#define IORESOURCE_IO		0x00000100
#define IORESOURCE_MEM		0x00000200

/* Any PCI device identifier (vendor, device, ...) */
#define PCI_CLASS_ANY_ID (0xffffff)

/* name of the shmem area containing the list of enumerated PCI devices: */
#define PCI_ENUMED_DEV "_ODP_PCI_ENUMERATED_DEVICES"

struct pci_dev_t;

/**
* A structure used to access io resources for a pci device.
* rte_pci_ioport is arch, os, driver specific, and should not be used outside
* of pci ioport api.
*/
typedef struct pci_ioport_t {
	struct pci_dev_t *dev;
	uint64_t base;
	uint64_t len; /* only filled for memory mapped ports */
} pci_ioport_t;


/* structure describing a PCI address: */
typedef struct pci_addr_t {
	uint16_t domain;		/* Device domain */
	uint8_t bus;			/* Device bus */
	uint8_t devid;			/* Device ID */
	uint8_t function;		/* Device function. */
} pci_addr_t;

/* structure describing an ID for a PCI device: */
typedef struct pci_id_t {
	uint32_t class_id;	      /* Class ID */
	uint16_t vendor_id;	      /* Vendor ID or PCI_ANY_ID. */
	uint16_t device_id;	      /* Device ID or PCI_ANY_ID. */
	uint16_t subsystem_vendor_id; /* Subsystem vendor ID or PCI_ANY_ID. */
	uint16_t subsystem_device_id; /* Subsystem device ID or PCI_ANY_ID. */
} pci_id_t;

/* structure describing a PCI resource (BAR region): */
typedef struct pci_resource_t {
	uint64_t phys_addr;/* Physical address, 0 if no resource. */
	void *addr;	   /* address (virtual, user space) of the BAR region */
	uint64_t len;      /* size of the region, in bytes */
} pci_resource_t;

/* enum telling which kernel driver is currentely bound to the pci device: */
enum pci_kernel_driver {
	PCI_KDRV_UNKNOWN = 0,
	PCI_KDRV_IGB_UIO,
	PCI_KDRV_VFIO,
	PCI_KDRV_UIO_GENERIC,
	PCI_KDRV_NIC_UIO,
	PCI_KDRV_NONE,
};


/**
* A structure describing a PCI mapping.
*/
struct pci_map {
	void *addr;
	char *path;
	uint64_t offset;
	uint64_t size;
	uint64_t phaddr;
};

typedef struct user_access_context
{
	union {
		struct {
			int vfio_dev_fd;  /**< VFIO device file descriptor */
			int vfio_fd;
		};
		struct {
			int uio_cfg_fd;  /**< UIO config file descriptor for uio_pci_generic */
			int uio_fd;
			int nb_maps;
			struct pci_map maps[PCI_MAX_RESOURCE];
			char path[PATH_MAX];
		};
	};
} user_access_context_t;


typedef struct user_access_ops
{
	int(*map_resource)(struct pci_dev_t *dev);
	int(*unmap_resource)(struct pci_dev_t *dev);
	int(*read_config)(struct pci_dev_t *dev, void *buf, size_t len, off_t offset);
	int(*write_config)(struct pci_dev_t *dev, void *buf, size_t len, off_t offset);
	int(*ioport_map)(struct pci_dev_t *dev, int idx, pci_ioport_t *p);
	int(*ioport_unmap)(struct pci_dev_t *dev, pci_ioport_t *p);
	void(*ioport_read)(struct pci_dev_t *dev, pci_ioport_t *p,
		void *data, size_t len, off_t offset);
	void(*ioport_write)(struct pci_dev_t *dev, pci_ioport_t *p,
		const void *data, size_t len, off_t offset);
} user_access_ops_t;


/* structure for PCI device: */
typedef struct pci_dev_t {
	struct pci_dev_t *next;
	pci_addr_t addr;						/* PCI location. */
	pci_id_t id;							/* PCI ID. */
	pci_resource_t bar[PCI_MAX_RESOURCE];	/* PCI Resources */
	uint16_t max_vfs;						/* sriov enable if not zero */
	enum pci_kernel_driver kdrv;			/* Kernel driver */
	user_access_ops_t* user_access_ops;
	user_access_context_t* user_access_context;
} pci_dev_t;

//minimal numa node support!!!
#define SOCKET_ID_ANY -1

#define PCI_PRI_FMT "%.4" PRIx16 ":%.2" PRIx8 ":%.2" PRIx8 ".%" PRIx8

void *
pci_map_resource(void *requested_addr, int fd, off_t offset, size_t size,
	int additional_flags);

const char *pci_get_sysfs_path(void);

int parse_pci_addr_format(const char *buf, uint16_t *domain,
	uint8_t *bus, uint8_t *devid,
	uint8_t *function);

pci_dev_t* _odp_pci_lookup_by_addr(pci_addr_t* addr);
int pci_parse_sysfs_value(const char *filename, unsigned long *val);


int pci_map_device(pci_dev_t *dev);
int pci_unmap_device(pci_dev_t *dev);
int pci_read_config(pci_dev_t *dev,
	void *buf, size_t len, off_t offset);
int pci_write_config(pci_dev_t *dev,
	void *buf, size_t len, off_t offset);

int
pci_ioport_map(pci_dev_t *dev, int bar,
	pci_ioport_t *p);
int
pci_ioport_unmap(pci_dev_t *dev, pci_ioport_t *p);
void
pci_ioport_read(pci_dev_t *dev, pci_ioport_t *p,
	void *data, size_t len, off_t offset);
void
pci_ioport_write(pci_dev_t *dev, pci_ioport_t *p,
	const void *data, size_t len, off_t offset);


#ifdef __cplusplus
}
#endif

#endif
