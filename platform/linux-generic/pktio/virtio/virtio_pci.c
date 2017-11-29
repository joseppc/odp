/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
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

#include "config.h"

#ifdef ODP_PKTIO_VIRTIO

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>

#include <config.h>
#include <odp_drv.h>
#include <odp_debug_internal.h>
#include <drv_pci_internal.h>
#include <pktio/physmem/physmem.h>

#include "virtqueue.h"
#include "virtio_pci.h"

/* Features desired/implemented by this driver. */
#define VIRTIO_NET_DRIVER_FEATURES              \
	(1u << VIRTIO_NET_F_MAC           |     \
	 1u << VIRTIO_NET_F_STATUS        |     \
	 1u << VIRTIO_NET_F_MQ            |     \
	 1u << VIRTIO_NET_F_CTRL_MAC_ADDR |     \
	 1u << VIRTIO_NET_F_CTRL_VQ       |     \
	 1u << VIRTIO_NET_F_CTRL_RX       |     \
	 1u << VIRTIO_NET_F_CTRL_VLAN     |     \
	 1u << VIRTIO_NET_F_MRG_RXBUF     |     \
	 1ULL << VIRTIO_F_VERSION_1)

static inline int
is_power_of_2(uint64_t n)
{
	return n && !(n & (n - 1));
}

static void virtio_print_features(const struct virtio_hw *hw)
{
	if (vtpci_with_feature(hw, VIRTIO_NET_F_CSUM))
		ODP_PRINT("VIRTIO_NET_F_CSUM\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_GUEST_CSUM))
		ODP_PRINT("VIRTIO_NET_F_GUEST_CSUM\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_MAC))
		ODP_PRINT("VIRTIO_NET_F_MAC\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_GUEST_TSO4))
		ODP_PRINT("VIRTIO_NET_F_GUEST_TSO4\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_GUEST_TSO6))
		ODP_PRINT("VIRTIO_NET_F_GUEST_TSO6\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_GUEST_ECN))
		ODP_PRINT("VIRTIO_NET_F_GUEST_ECN\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_GUEST_UFO))
		ODP_PRINT("VIRTIO_NET_F_GUEST_UFO\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_HOST_TSO4))
		ODP_PRINT("VIRTIO_NET_F_HOST_TSO4\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_HOST_TSO6))
		ODP_PRINT("VIRTIO_NET_F_HOST_TSO6\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_HOST_ECN))
		ODP_PRINT("VIRTIO_NET_F_HOST_ECN\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_HOST_UFO))
		ODP_PRINT("VIRTIO_NET_F_HOST_UFO\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_MRG_RXBUF))
		ODP_PRINT("VIRTIO_NET_F_MRG_RXBUF\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_STATUS))
		ODP_PRINT("VIRTIO_NET_F_STATUS\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_CTRL_VQ))
		ODP_PRINT("VIRTIO_NET_F_CTRL_VQ\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_CTRL_RX))
		ODP_PRINT("VIRTIO_NET_F_CTRL_RX\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_CTRL_VLAN))
		ODP_PRINT("VIRTIO_NET_F_CTRL_VLAN\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_CTRL_RX_EXTRA))
		ODP_PRINT("VIRTIO_NET_F_CTRL_RX_EXTRA\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_GUEST_ANNOUNCE))
		ODP_PRINT("VIRTIO_NET_F_GUEST_ANNOUNCE\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_MQ))
		ODP_PRINT("VIRTIO_NET_F_MQ\n");
	if (vtpci_with_feature(hw, VIRTIO_NET_F_CTRL_MAC_ADDR))
		ODP_PRINT("VIRTIO_NET_F_CTRL_MAC_ADDR\n");
}

static inline uint8_t io_read8(uint8_t *addr)
{
	return *(volatile uint8_t *)addr;
}

static inline void io_write8(uint8_t val, uint8_t *addr)
{
	*(volatile uint8_t *)addr = val;
}

static inline uint16_t io_read16(uint16_t *addr)
{
	return *(volatile uint16_t *)addr;
}

static inline void io_write16(uint16_t val, uint16_t *addr)
{
	*(volatile uint16_t *)addr = val;
}

static inline uint32_t io_read32(uint32_t *addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void io_write32(uint32_t val, uint32_t *addr)
{
	*(volatile uint32_t *)addr = val;
}

#if 0
static inline void io_write64_twopart(uint64_t val, uint32_t *lo, uint32_t *hi)
{
	io_write32(val & ((1ULL << 32) - 1), lo);
	io_write32(val >> 32,		     hi);
}
#endif

static void modern_read_dev_config(struct virtio_hw *hw, size_t offset,
				   void *dst, int length)
{
	int i;
	uint8_t *p;
	uint8_t old_gen, new_gen;

	do {
		old_gen = io_read8(&hw->common_cfg->config_generation);

		p = dst;
		for (i = 0;  i < length; i++)
			*p++ = io_read8((uint8_t *)hw->dev_cfg + offset + i);

		new_gen = io_read8(&hw->common_cfg->config_generation);
	} while (old_gen != new_gen);
}

static void modern_write_dev_config(struct virtio_hw *hw, size_t offset,
				    const void *src, int length)
{
	int i;
	const uint8_t *p = src;

	for (i = 0;  i < length; i++)
		io_write8(*p++, (uint8_t *)hw->dev_cfg + offset + i);
}

static uint64_t modern_get_features(struct virtio_hw *hw)
{
	uint32_t features_lo, features_hi;

	io_write32(0, &hw->common_cfg->device_feature_select);
	features_lo = io_read32(&hw->common_cfg->device_feature);

	io_write32(1, &hw->common_cfg->device_feature_select);
	features_hi = io_read32(&hw->common_cfg->device_feature);

	return ((uint64_t)features_hi << 32) | features_lo;
}

static void modern_set_features(struct virtio_hw *hw, uint64_t features)
{
	io_write32(0, &hw->common_cfg->guest_feature_select);
	io_write32(features & ((1ULL << 32) - 1),
		&hw->common_cfg->guest_feature);

	io_write32(1, &hw->common_cfg->guest_feature_select);
	io_write32(features >> 32,
		&hw->common_cfg->guest_feature);
}

static uint8_t modern_get_status(struct virtio_hw *hw)
{
	return io_read8(&hw->common_cfg->device_status);
}

static void modern_set_status(struct virtio_hw *hw, uint8_t status)
{
	io_write8(status, &hw->common_cfg->device_status);
}

static void modern_reset(struct virtio_hw *hw)
{
	modern_set_status(hw, VIRTIO_CONFIG_STATUS_RESET);
	modern_get_status(hw);
}

static uint8_t modern_get_isr(struct virtio_hw *hw)
{
	return io_read8(hw->isr);
}

static uint16_t modern_set_config_irq(struct virtio_hw *hw, uint16_t vec)
{
	io_write16(vec, &hw->common_cfg->msix_config);
	return io_read16(&hw->common_cfg->msix_config);
}

static uint16_t modern_get_queue_num(struct virtio_hw *hw, uint16_t queue_id)
{
	io_write16(queue_id, &hw->common_cfg->queue_select);
	return io_read16(&hw->common_cfg->queue_size);
}

static int modern_setup_queue(struct virtio_hw *hw, struct virtqueue *vq)
{
	(void)hw;
	(void)vq;
	return 0;
}

static void modern_del_queue(struct virtio_hw *hw, struct virtqueue *vq)
{
	(void)hw;
	(void)vq;
}

static void modern_notify_queue(struct virtio_hw *hw, struct virtqueue *vq)
{
	(void)hw;
	(void)vq;
}

static const struct virtio_pci_ops modern_ops = {
	.read_dev_cfg	= modern_read_dev_config,
	.write_dev_cfg	= modern_write_dev_config,
	.reset		= modern_reset,
	.get_status	= modern_get_status,
	.set_status	= modern_set_status,
	.get_features	= modern_get_features,
	.set_features	= modern_set_features,
	.get_isr	= modern_get_isr,
	.set_config_irq	= modern_set_config_irq,
	.get_queue_num	= modern_get_queue_num,
	.setup_queue	= modern_setup_queue,
	.del_queue	= modern_del_queue,
	.notify_queue	= modern_notify_queue,
};


void vtpci_read_dev_config(struct virtio_hw *hw, size_t offset,
			   void *dst, int length)
{
	hw->vtpci_ops->read_dev_cfg(hw, offset, dst, length);
}

void vtpci_write_dev_config(struct virtio_hw *hw, size_t offset,
			    const void *src, int length)
{
	hw->vtpci_ops->write_dev_cfg(hw, offset, src, length);
}

uint64_t vtpci_negotiate_features(struct virtio_hw *hw, uint64_t host_features)
{
	uint64_t features;

	/*
	 * Limit negotiated features to what the driver, virtqueue, and
	 * host all support.
	 */
	features = host_features & hw->guest_features;
	hw->vtpci_ops->set_features(hw, features);

	return features;
}

void vtpci_reset(struct virtio_hw *hw)
{
	hw->vtpci_ops->set_status(hw, VIRTIO_CONFIG_STATUS_RESET);
	/* flush status write */
	hw->vtpci_ops->get_status(hw);
}

void vtpci_reinit_complete(struct virtio_hw *hw)
{
	vtpci_set_status(hw, VIRTIO_CONFIG_STATUS_DRIVER_OK);
}

void vtpci_set_status(struct virtio_hw *hw, uint8_t status)
{
	if (status != VIRTIO_CONFIG_STATUS_RESET)
		status |= hw->vtpci_ops->get_status(hw);

	hw->vtpci_ops->set_status(hw, status);
}

uint8_t vtpci_get_status(struct virtio_hw *hw)
{
	return hw->vtpci_ops->get_status(hw);
}

uint8_t vtpci_isr(struct virtio_hw *hw)
{
	return hw->vtpci_ops->get_isr(hw);
}


/* Enable one vector (0) for Link State Intrerrupt */
uint16_t vtpci_irq_config(struct virtio_hw *hw, uint16_t vec)
{
	return hw->vtpci_ops->set_config_irq(hw, vec);
}

static void *get_cfg_addr(pci_dev_t *dev, struct virtio_pci_cap *cap)
{
	uint8_t  bar    = cap->bar;
	uint32_t length = cap->length;
	uint32_t offset = cap->offset;
	uint8_t *base;

	if (bar > 5) {
		ODP_ERR("invalid bar: %u\n", bar);
		return NULL;
	}

	if (offset + length < offset) {
		ODP_ERR("offset(%u) + length(%u) overflows\n",
			offset, length);
		return NULL;
	}

	if (offset + length > dev->bar[bar].len) {
		ODP_ERR("invalid cap: overflows bar space: %u > %\n" PRIu64,
			offset + length, dev->bar[bar].len);
		return NULL;
	}

	base = dev->bar[bar].addr;
	if (base == NULL) {
		ODP_ERR("bar %u base addr is NULL\n", bar);
		return NULL;
	}

	return base + offset;
}

static int virtio_read_caps(pci_dev_t *dev, struct virtio_hw *hw)
{
	uint8_t pos;
	struct virtio_pci_cap cap;
	int ret;

	if (dev->user_access_ops->map_resource(dev)) {
		ODP_DBG("failed to map pci device!\n");
		return -1;
	}

	ret = pci_read_config(dev, &pos, 1, PCI_CAPABILITY_LIST);
	if (ret < 0) {
		ODP_DBG("failed to read pci capability list\n");
		return -1;
	}

	while (pos) {
		ret = pci_read_config(dev, &cap, sizeof(cap), pos);
		if (ret < 0) {
			ODP_ERR("failed to read pci cap at pos: %x\n", pos);
			break;
		}

		if (cap.cap_vndr != PCI_CAP_ID_VNDR) {
			ODP_DBG("[%2x] skipping non VNDR cap id: %02x\n",
				pos, cap.cap_vndr);
			goto next;
		}

		ODP_DBG("[%2x] cfg type: %u, bar: %u, offset: %04x, len: %u\n",
			pos, cap.cfg_type, cap.bar, cap.offset, cap.length);

		switch (cap.cfg_type) {
		case VIRTIO_PCI_CAP_COMMON_CFG:
			hw->common_cfg = get_cfg_addr(dev, &cap);
			break;
		case VIRTIO_PCI_CAP_NOTIFY_CFG:
			pci_read_config(dev, &hw->notify_off_multiplier,
					4, pos + sizeof(cap));
			hw->notify_base = get_cfg_addr(dev, &cap);
			break;
		case VIRTIO_PCI_CAP_DEVICE_CFG:
			hw->dev_cfg = get_cfg_addr(dev, &cap);
			break;
		case VIRTIO_PCI_CAP_ISR_CFG:
			hw->isr = get_cfg_addr(dev, &cap);
			break;
		}

next:
		pos = cap.cap_next;
	}

	if (hw->common_cfg == NULL || hw->notify_base == NULL ||
	    hw->dev_cfg == NULL    || hw->isr == NULL) {
		ODP_DBG("no modern virtio pci device found.\n");
		return -1;
	}

	ODP_DBG("found modern virtio pci device.\n");

	ODP_DBG("common cfg mapped at: %p\n", hw->common_cfg);
	ODP_DBG("device cfg mapped at: %p\n", hw->dev_cfg);
	ODP_DBG("isr cfg mapped at: %p\n", hw->isr);
	ODP_DBG("notify base: %p, notify off multiplier: %u\n",
		hw->notify_base, hw->notify_off_multiplier);

	return 0;
}

static void virtio_get_hwaddr(struct virtio_hw *hw)
{
	if (vtpci_with_feature(hw, VIRTIO_NET_F_MAC)) {
		vtpci_read_dev_config(hw,
				      ODPDRV_OFFSETOF(struct virtio_net_config,
						      mac),
				      &hw->mac_addr,
				      6);
		ODP_PRINT("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
			  hw->mac_addr[0],
			  hw->mac_addr[1],
			  hw->mac_addr[2],
			  hw->mac_addr[3],
			  hw->mac_addr[4],
			  hw->mac_addr[5]);
	} else {
		ODP_PRINT("No support for VIRTIO_NET_F_MAC\n");
	}
}

static void virtio_get_status(struct virtio_hw *hw)
{
	struct virtio_net_config config;

	if (!vtpci_with_feature(hw, VIRTIO_NET_F_STATUS)) {
		ODP_PRINT("No support for VIRTIO_NET_F_STATUS\n");
		return;
	}

	vtpci_read_dev_config(hw,
			      ODPDRV_OFFSETOF(struct virtio_net_config,
					      status),
			      &config.status, sizeof(config.status));

	ODP_PRINT("Status is %u\n", config.status);
}

static int virtio_get_queue_type(struct virtio_hw *hw, uint16_t vtpci_queue_idx)
{
	if (vtpci_queue_idx == hw->max_queue_pairs * 2)
		return VTNET_CQ;
	else if (vtpci_queue_idx % 2 == 0)
		return VTNET_RQ;
	else
		return VTNET_TQ;
}

static void
virtio_init_vring(struct virtqueue *vq)
{
	int size = vq->vq_nentries;
	struct vring *vr = &vq->vq_ring;
	uint8_t *ring_mem = vq->vq_ring_virt_mem;

	/*
	 * Reinitialise since virtio port might have been stopped and restarted
	 */
	memset(ring_mem, 0, vq->vq_ring_size);
	vring_init(vr, size, ring_mem, VIRTIO_PCI_VRING_ALIGN);
	vq->vq_used_cons_idx = 0;
	vq->vq_desc_head_idx = 0;
	vq->vq_avail_idx = 0;
	vq->vq_desc_tail_idx = (uint16_t)(vq->vq_nentries - 1);
	vq->vq_free_cnt = vq->vq_nentries;
	memset(vq->vq_descx, 0, sizeof(struct vq_desc_extra) * vq->vq_nentries);

	vring_desc_init(vr->desc, size);

	/*
	 * Disable device(host) interrupting guest
	 */
	virtqueue_disable_intr(vq);
}

static uint16_t virtio_get_num_queues(struct virtio_hw *hw)
{
	uint16_t max_pairs;

	if (!vtpci_with_feature(hw, VIRTIO_NET_F_MQ)) {
		ODP_PRINT("No support for VIRTIO_NET_F_MQ\n");
		return 1;
	}

	vtpci_read_dev_config(hw,
			      ODPDRV_OFFSETOF(struct virtio_net_config,
					      max_virtqueue_pairs),
			      &max_pairs, sizeof(max_pairs));

	ODP_PRINT("Device supports maximum of %u virtqueue pairs\n", max_pairs);

	return max_pairs;
}

static int virtio_init_queue(struct virtio_hw *hw, int vtpci_queue_idx)
{
	char vq_hdr_name[VIRTQUEUE_MAX_NAME_SZ];
	struct physmem_block *block;
#ifdef HAS_RTE_MEMZONE
	const struct rte_memzone *mz = NULL, *hdr_mz = NULL;
#endif
	unsigned int vq_size, size;
	struct virtnet_rx *rxvq = NULL;
	struct virtnet_tx *txvq = NULL;
	struct virtnet_ctl *cvq = NULL;
	struct virtqueue *vq;
	size_t sz_hdr_mz = 0;
	void *sw_ring = NULL;
	int queue_type = virtio_get_queue_type(hw, vtpci_queue_idx);
	int ret = 0;

	ODP_PRINT("setting up queue: %u\n", vtpci_queue_idx);

	/*
	 * Read the virtqueue size from the Queue Size field
	 * Always power of 2 and if 0 virtqueue does not exist
	 */
	vq_size = modern_get_queue_num(hw, vtpci_queue_idx);
	ODP_PRINT("vq_size: %u\n", vq_size);
	if (vq_size == 0) {
		ODP_ERR("virtqueue does not exist");
		return -EINVAL;
	}

	if (!is_power_of_2(vq_size)) {
		ODP_ERR("virtqueue size is not powerof 2\n");
		return -EINVAL;
	}

	size = ROUNDUP_CACHE_LINE(sizeof(*vq) +
				  vq_size * sizeof(struct vq_desc_extra));
	ODP_PRINT("Size: %u\nType: %d\n", size, queue_type);
	if (queue_type == VTNET_TQ) {
		ODP_PRINT("Type: VNET_TQ\n");
		/*
		 * For each xmit packet, allocate a virtio_net_hdr
		 * and indirect ring elements
		 */
		sz_hdr_mz = vq_size * sizeof(struct virtio_tx_region);
	} else if (queue_type == VTNET_CQ) {
		ODP_PRINT("Type: VNET_CQ\n");
		/* Allocate a page for control vq command, data and status */
		sz_hdr_mz = ODP_PAGE_SIZE;
	} else {
		ODP_PRINT("Type: VNET_RQ\n");
	}

	vq = malloc(size); /* FXME: alloc from pool */
	if (vq == NULL) {
		ODP_DBG("can not allocate vq");
		return -ENOMEM;
	}
	hw->vqs[vtpci_queue_idx] = vq;

	vq->hw = hw;
	vq->vq_queue_index = vtpci_queue_idx;
	vq->vq_nentries = vq_size;

	/*
	 * Reserve a memzone for vring elements
	 */
	size = vring_size(vq_size, VIRTIO_PCI_VRING_ALIGN);
	vq->vq_ring_size = ROUNDUP_ALIGN(size, VIRTIO_PCI_VRING_ALIGN);
	ODP_DBG("vring_size: %d, rounded_vring_size: %d\n",
		size, vq->vq_ring_size);

	block = physmem_block_alloc(size);
	if (block == NULL) {
		ODP_ERR("Could not allocate block\n");
		goto exit_failure;
	}

	memset(block->va, 0, block->size);

	vq->vq_ring_mem = block->pa;
	vq->vq_ring_virt_mem = block->va;
	ODP_DBG("vq->vq_ring_mem:      0x%" PRIx64 "\n",
		(uint64_t)block->pa);
	ODP_DBG("vq->vq_ring_virt_mem: 0x%" PRIx64 "\n",
		(uint64_t)(uintptr_t)block->va);

	virtio_init_vring(vq);

	(void)sw_ring;
	(void)sz_hdr_mz;
	(void)vq;
	(void)cvq;
	(void)txvq;
	(void)rxvq;
	(void)size;
	(void)vq_hdr_name;

	return ret;

exit_failure:
	physmem_block_free(block);

	return -1;

#if 0
	if (sz_hdr_mz) {
		snprintf(vq_hdr_name, sizeof(vq_hdr_name), "port%d_vq%d_hdr",
			 dev->data->port_id, vtpci_queue_idx);
		hdr_mz = rte_memzone_reserve_aligned(vq_hdr_name, sz_hdr_mz,
						     SOCKET_ID_ANY, 0,
						     RTE_CACHE_LINE_SIZE);
		if (hdr_mz == NULL) {
			if (rte_errno == EEXIST)
				hdr_mz = rte_memzone_lookup(vq_hdr_name);
			if (hdr_mz == NULL) {
				ret = -ENOMEM;
				goto fail_q_alloc;
			}
		}
	}

	if (queue_type == VTNET_RQ) {
		size_t sz_sw = (RTE_PMD_VIRTIO_RX_MAX_BURST + vq_size) *
			       sizeof(vq->sw_ring[0]);

		sw_ring = rte_zmalloc_socket("sw_ring", sz_sw,
				RTE_CACHE_LINE_SIZE, SOCKET_ID_ANY);
		if (!sw_ring) {
			PMD_INIT_LOG(ERR, "can not allocate RX soft ring");
			ret = -ENOMEM;
			goto fail_q_alloc;
		}

		vq->sw_ring = sw_ring;
		rxvq = &vq->rxq;
		rxvq->vq = vq;
		rxvq->port_id = dev->data->port_id;
		rxvq->mz = mz;
	} else if (queue_type == VTNET_TQ) {
		txvq = &vq->txq;
		txvq->vq = vq;
		txvq->port_id = dev->data->port_id;
		txvq->mz = mz;
		txvq->virtio_net_hdr_mz = hdr_mz;
		txvq->virtio_net_hdr_mem = hdr_mz->iova;
	} else if (queue_type == VTNET_CQ) {
		cvq = &vq->cq;
		cvq->vq = vq;
		cvq->mz = mz;
		cvq->virtio_net_hdr_mz = hdr_mz;
		cvq->virtio_net_hdr_mem = hdr_mz->iova;
		memset(cvq->virtio_net_hdr_mz->addr, 0, PAGE_SIZE);

		hw->cvq = cvq;
	}

	/* For virtio_user case (that is when hw->dev is NULL), we use
	 * virtual address. And we need properly set _offset_, please see
	 * VIRTIO_MBUF_DATA_DMA_ADDR in virtqueue.h for more information.
	 */
	if (!hw->virtio_user_dev)
		vq->offset = offsetof(struct rte_mbuf, buf_iova);
	else {
		vq->vq_ring_mem = (uintptr_t)mz->addr;
		vq->offset = offsetof(struct rte_mbuf, buf_addr);
		if (queue_type == VTNET_TQ)
			txvq->virtio_net_hdr_mem = (uintptr_t)hdr_mz->addr;
		else if (queue_type == VTNET_CQ)
			cvq->virtio_net_hdr_mem = (uintptr_t)hdr_mz->addr;
	}

	if (queue_type == VTNET_TQ) {
		struct virtio_tx_region *txr;
		unsigned int i;

		txr = hdr_mz->addr;
		memset(txr, 0, vq_size * sizeof(*txr));
		for (i = 0; i < vq_size; i++) {
			struct vring_desc *start_dp = txr[i].tx_indir;

			vring_desc_init(start_dp, RTE_DIM(txr[i].tx_indir));

			/* first indirect descriptor is always the tx header */
			start_dp->addr = txvq->virtio_net_hdr_mem
				+ i * sizeof(*txr)
				+ offsetof(struct virtio_tx_region, tx_hdr);

			start_dp->len = hw->vtnet_hdr_size;
			start_dp->flags = VRING_DESC_F_NEXT;
		}
	}

	if (VTPCI_OPS(hw)->setup_queue(hw, vq) < 0) {
		PMD_INIT_LOG(ERR, "setup_queue failed");
		return -EINVAL;
	}

	return 0;

fail_q_alloc:
	rte_free(sw_ring);
	rte_memzone_free(hdr_mz);
	rte_memzone_free(mz);
	rte_free(vq);

	return ret;
#endif
}

static void virtio_free_queues(struct virtio_hw *hw ODP_UNUSED)
{
	return;
}

static uint16_t virtio_get_nr_vq(struct virtio_hw *hw)
{
	uint16_t nr_vq = hw->max_queue_pairs * 2;

	if (vtpci_with_feature(hw, VIRTIO_NET_F_CTRL_VQ))
		nr_vq += 1;

	return nr_vq;
}

static int virtio_alloc_queues(struct virtio_hw *hw)
{
	uint16_t nr_vq = virtio_get_nr_vq(hw);
	uint16_t i;
	int ret;

	hw->vqs = malloc(sizeof(struct virtqueue *) * nr_vq);
	if (!hw->vqs) {
		ODP_ERR("failed to allocate vqs");
		return -ENOMEM;
	}

	for (i = 0; i < nr_vq; i++) {
		ret = virtio_init_queue(hw, i);
		if (ret < 0) {
			virtio_free_queues(hw);
			return ret;
		}
	}

	return 0;
}

static int virtio_init_ethdev(struct virtio_hw *hw)
{
	const struct virtio_pci_ops *ops = hw->vtpci_ops;
	uint64_t device_features = 0;
	int ret;

	ODP_PRINT("Init VirtIO Net device\n");

	ops->reset(hw);
	ops->set_status(hw, VIRTIO_CONFIG_STATUS_ACK);
	ops->set_status(hw, VIRTIO_CONFIG_STATUS_DRIVER);
	device_features = ops->get_features(hw);

	/* accept only those feature this driver also supports */
	hw->guest_features = device_features & VIRTIO_NET_DRIVER_FEATURES;

	if (!vtpci_with_feature(hw, VIRTIO_F_VERSION_1)) {
		ODP_ERR("VirtIO device does not comply with VirtIO 1.0 spec\n");
		return -1;
	}

	ops->set_features(hw, hw->guest_features);
	virtio_print_features(hw);

	ops->set_status(hw, VIRTIO_CONFIG_STATUS_FEATURES_OK);
	if (!(ops->get_status(hw) & VIRTIO_CONFIG_STATUS_FEATURES_OK)) {
		ODP_ERR("VirtIO device error negotiationg features\n");
		return -1;
	}

	virtio_get_hwaddr(hw);
	virtio_get_status(hw);

	hw->cvq = NULL;
	/* some features depend on F_CTRL_VQ being available */
	if (vtpci_with_feature(hw, VIRTIO_NET_F_CTRL_VQ)) {
		uint16_t num_pairs;

		num_pairs = virtio_get_num_queues(hw);
		/* FIXME: find out minimum available */
		hw->max_queue_pairs = num_pairs;
		hw->max_tx_queues = num_pairs;
		hw->max_rx_queues = num_pairs;
	} else {
		hw->max_queue_pairs = 1;
		hw->max_tx_queues = 1;
		hw->max_rx_queues = 1;
	}

	ret = virtio_alloc_queues(hw);
	if (ret != 0) {
		ops->reset(hw);
		return ret;
	}

	return ret;
}

/* FIXME: this should be registered as a DevIO */
extern const user_access_ops_t uio_access_ops;

/*
 * Return -1:
 *   if there is error mapping with VFIO/UIO.
 *   if port map error when driver type is KDRV_NONE.
 *   if whitelisted but driver type is KDRV_UNKNOWN.
 * Return 1 if kernel driver is managing the device.
 * Return 0 on success.
 */
int virtio_pci_init(pci_dev_t *dev)
{
	struct virtio_hw *hw = NULL;

	if (dev->id.vendor_id != VIRTIO_PCI_VENDOR_ID)
		return -1;

	if (dev->id.device_id != VIRTIO_PCI_LEGACY_DEVICE_ID_NET &&
	    dev->id.device_id != VIRTIO_PCI_MODERN_DEVICE_ID_NET)
		return -1;

	hw = malloc(sizeof(struct virtio_hw));
	if (hw == NULL)
		return -1;
	memset(hw, 0, sizeof(struct virtio_hw));

	/* Find suitable DevIO module that works with this device */
	if (dev->kdrv == PCI_KDRV_UIO_GENERIC) {
		/* probing would be done for each possible DevIO */
		if (uio_access_ops.probe(dev) != 0)
			goto err_free;
		dev->user_access_ops = &uio_access_ops;
	} else {
		ODP_ERR("Could not find suitable DevIO for device\n");
		goto err_free;
	}

	/*
	 * Try if we can succeed reading virtio pci caps, which exists
	 * only on modern pci device.
	 */
	if (virtio_read_caps(dev, hw) != 0) {
		/* we only support modern interface */
		ODP_ERR("virtio_pci: could not read device capabilities\n");
		goto err_free;
	}

	hw->dev = dev;
	hw->vtpci_ops = &modern_ops;
	hw->modern    = 1;
	if (virtio_init_ethdev(hw) != 0)
		goto err_free;
	dev->driver_data = (void *)hw;

	return 0;

err_free:
	free(hw);
	return -1;
}

#endif
