/* Copyright (c) 2013, Linaro Limited
* Copyright (c) 2013, Nokia Solutions and Networks
* All rights reserved.
*
* SPDX-License-Identifier:     BSD-3-Clause
*/

#include <odp_api.h>
#include <odp_packet_internal.h>
#include <odp_packet_io_internal.h>
#include <odp_classification_internal.h>
#include <odp_debug_internal.h>
#include <odp/api/hints.h>

#include <protocols/eth.h>
#include <protocols/ip.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include "virtio_logs.h"
#include "virtqueue.h"
#include "virtio_rxtx.h"
#include "virtio.h"
#include <odp/drv/shm.h>
#include <drv_pci_internal.h>
#include "virtio_pci.h"


#define ETHER_LOCAL_ADMIN_ADDR 0x02 /**< Locally assigned Eth. address. */
#define ETHER_GROUP_ADDR       0x01 /**< Multicast or broadcast Eth. address. */

static inline int
is_power_of_2(uint32_t n)
{
	return n && !(n & (n - 1));
}

#define       RTE_DIM(a)      (sizeof (a) / sizeof ((a)[0]))

/**
* Ethernet address:
* A universally administered address is uniquely assigned to a device by its
* manufacturer. The first three octets (in transmission order) contain the
* Organizationally Unique Identifier (OUI). The following three (MAC-48 and
* EUI-48) octets are assigned by that organization with the only constraint
* of uniqueness.
* A locally administered address is assigned to a device by a network
* administrator and does not contain OUIs.
* See http://standards.ieee.org/regauth/groupmac/tutorial.html
*/
struct ether_addr {
	uint8_t addr_bytes[ETHER_ADDR_LEN]; /**< Address bytes in transmission order */
} __attribute__((__packed__));



static int virtio_negotiate_features(struct virtio_hw *hw)
{
	uint64_t host_features;

	/* Prepare guest_features: feature that driver wants to support */
	hw->guest_features = VIRTIO_PMD_GUEST_FEATURES;
	PMD_INIT_LOG(1, "guest_features before negotiate = %" PRIx64,
		hw->guest_features);

	/* Read device(host) feature bits */
	host_features = hw->vtpci_ops->get_features(hw);
	PMD_INIT_LOG(1, "host_features before negotiate = %" PRIx64,
		host_features);

	/*
	* Negotiate features: Subset of device feature bits are written back
	* guest feature bits.
	*/
	hw->guest_features = vtpci_negotiate_features(hw, host_features);
	PMD_INIT_LOG(1, "features after negotiate = %" PRIx64,
		     hw->guest_features);

	if (hw->modern) {
		if (!vtpci_with_feature(hw, VIRTIO_F_VERSION_1)) {
			PMD_INIT_LOG(1,
				"VIRTIO_F_VERSION_1 features is not enabled.");
			return -1;
		}
		vtpci_set_status(hw, VIRTIO_CONFIG_STATUS_FEATURES_OK);
		if (!(vtpci_get_status(hw) & VIRTIO_CONFIG_STATUS_FEATURES_OK)) {
			PMD_INIT_LOG(1,
				"failed to set FEATURES_OK status!");
			return -1;
		}
	}

	return 0;
}


static void rx_func_get(pktio_entry_t *pktio_entry ODP_UNUSED)
{
#ifdef ACTIVATED
	struct virtio_hw *hw = hw = &pktio_entry->s.pkt_virtio.hw;
	if (vtpci_with_feature(hw, VIRTIO_NET_F_MRG_RXBUF))
		pktio_entry->s.ops->recv = &virtio_recv_mergeable_pkts;
	else
		pktio_entry->s.ops->recv = &virtio_recv_pkts;
#endif
}


/**
* Generate a random Ethernet address that is locally administered
* and not multicast.
* @param addr
*   A pointer to Ethernet address.
*/
static inline void eth_random_addr(uint8_t *addr)
{
	// just do nothing for the moment...
	addr[0] &= ~ETHER_GROUP_ADDR;       /* clear multicast bit */
	addr[0] |= ETHER_LOCAL_ADMIN_ADDR;  /* set local assignment bit */
}


static void virtio_set_hwaddr(struct virtio_hw *hw)
{
	vtpci_write_dev_config(hw,
			       offsetof(struct virtio_net_config, mac),
			       &hw->mac_addr, ETHER_ADDR_LEN);
}

static void virtio_get_hwaddr(struct virtio_hw *hw)
{
	if (vtpci_with_feature(hw, VIRTIO_NET_F_MAC)) {
		vtpci_read_dev_config(hw,
				      offsetof(struct virtio_net_config, mac),
				      &hw->mac_addr, ETHER_ADDR_LEN);
	} else {
		eth_random_addr(&hw->mac_addr[0]);
		virtio_set_hwaddr(hw);
	}
}

/**
* Fast copy an Ethernet address.
*
* @param ea_from
*   A pointer to a ether_addr structure holding the Ethernet address to copy.
* @param ea_to
*   A pointer to a ether_addr structure where to copy the Ethernet address.
*/
static inline void ether_addr_copy(struct ether_addr *ea_from,
				   struct ether_addr *ea_to)
{
	uint16_t *from_words = (uint16_t *)(ea_from);
	uint16_t *to_words = (uint16_t *)(ea_to);

	to_words[0] = from_words[0];
	to_words[1] = from_words[1];
	to_words[2] = from_words[2];

}

static void virtio_dev_queue_release(struct virtqueue *vq)
{
	struct virtio_hw *hw;

	if (vq) {
		hw = vq->hw;
		if (vq->configured)
			hw->vtpci_ops->del_queue(hw, vq);

		odpdrv_shm_free_by_address(vq->sw_ring);
		odpdrv_shm_free_by_address(vq);
	}
}

static int virtio_dev_queue_setup(pktio_entry_t *pktio_entry,
				  int queue_type,
				  uint16_t queue_idx,
				  uint16_t vtpci_queue_idx,
				  uint16_t nb_desc,
				  unsigned int socket_id ODP_UNUSED,
				  void **pvq)
{
	char vq_name[VIRTQUEUE_MAX_NAME_SZ];
	char vq_hdr_name[VIRTQUEUE_MAX_NAME_SZ];
	odpdrv_shm_t mz = ODPDRV_SHM_INVALID;
	odpdrv_shm_t hdr_mz = ODPDRV_SHM_INVALID;
	unsigned int vq_size, size;
	struct virtio_hw *hw = &pktio_entry->s.pkt_virtio.hw;
	struct virtnet_rx *rxvq = NULL;
	struct virtnet_tx *txvq = NULL;
	struct virtnet_ctl *cvq = NULL;
	struct virtqueue *vq;
	const char *queue_names[] = { "rvq", "txq", "cvq" };
	size_t sz_vq, sz_q = 0, sz_hdr_mz = 0;
	void *sw_ring = NULL;
	int ret;
	odpdrv_shm_t memory;
	void* location;

	PMD_INIT_LOG(1, "setting up queue: %u", vtpci_queue_idx);

	/*
	* Read the virtqueue size from the Queue Size field
	* Always power of 2 and if 0 virtqueue does not exist
	*/
	vq_size = hw->vtpci_ops->get_queue_num(hw, vtpci_queue_idx);
	PMD_INIT_LOG(1, "vq_size: %u nb_desc:%u", vq_size, nb_desc);
	if (vq_size == 0) {
		PMD_INIT_LOG(1, "virtqueue does not exist");
		return -EINVAL;
	}

	if (!is_power_of_2(vq_size)) {
		PMD_INIT_LOG(1, "virtqueue size is not powerof 2");
		return -EINVAL;
	}

	snprintf(vq_name, sizeof(vq_name), "port%d_%s%d",
		 pktio_entry->s.pkt_virtio.port_id, queue_names[queue_type], queue_idx);

	sz_vq = RTE_ALIGN_CEIL(sizeof(*vq) +
			       vq_size * sizeof(struct vq_desc_extra),
			       ODP_CACHE_LINE_SIZE);
	if (queue_type == VTNET_RQ) {
		sz_q = sz_vq + sizeof(*rxvq);
	} else if (queue_type == VTNET_TQ) {
		sz_q = sz_vq + sizeof(*txvq);
		/*
		* For each xmit packet, allocate a virtio_net_hdr
		* and indirect ring elements
		*/
		sz_hdr_mz = vq_size * sizeof(struct virtio_tx_region);
	} else if (queue_type == VTNET_CQ) {
		sz_q = sz_vq + sizeof(*cvq);
		/* Allocate a page for control vq command, data and status */
		sz_hdr_mz = PAGE_SIZE;
	}
	
	memory = odpdrv_shm_reserve(NULL, sz_q, ODP_CACHE_LINE_SIZE, ODPDRV_SHM_LOCK);
	vq = odpdrv_shm_addr(memory);
	if (vq == NULL) {
		PMD_INIT_LOG(1, "can not allocate vq");
		return -ENOMEM;
	}
	vq->hw = hw;
	vq->vq_queue_index = vtpci_queue_idx;
	vq->vq_nentries = vq_size;

	if (nb_desc == 0 || nb_desc > vq_size)
		nb_desc = vq_size;
	vq->vq_free_cnt = nb_desc;

	/*
	* Reserve a memzone for vring elements
	*/
	size = vring_size(vq_size, VIRTIO_PCI_VRING_ALIGN);
	vq->vq_ring_size = RTE_ALIGN_CEIL(size, VIRTIO_PCI_VRING_ALIGN);
	PMD_INIT_LOG(1, "vring_size: %d, rounded_vring_size: %d",
		     size, vq->vq_ring_size);

	mz = odpdrv_shm_reserve(NULL, vq->vq_ring_size, ODP_CACHE_LINE_SIZE,
				ODPDRV_SHM_LOCK);

	location = odpdrv_shm_addr(mz);
	vq->vq_ring_mem = _odp_ishmphy_getphy(location);
	vq->vq_ring_virt_mem = mz;
	PMD_INIT_LOG(1, "vq->vq_ring_mem:      0x%" PRIx64,
		     (uint64_t)vq->vq_ring_mem);
	PMD_INIT_LOG(DEBUG, "vq->vq_ring_virt_mem: 0x%" PRIx64,
		     (uint64_t)(uintptr_t)vq->vq_ring_virt_mem);

	if (sz_hdr_mz) {
		snprintf(vq_hdr_name, sizeof(vq_hdr_name), "port%d_%s%d_hdr",
			 pktio_entry->s.pkt_virtio.port_id,
			 queue_names[queue_type],
			 queue_idx);
		hdr_mz = odpdrv_shm_reserve(NULL, sz_hdr_mz,
					    ODP_CACHE_LINE_SIZE,
					    ODPDRV_SHM_LOCK);
		if (hdr_mz == ODPDRV_SHM_INVALID) {
			ret = -ENOMEM;
			goto fail_q_alloc;
		}
	}

	if (queue_type == VTNET_RQ) {
		size_t sz_sw = (RTE_PMD_VIRTIO_RX_MAX_BURST + vq_size) *
				sizeof(vq->sw_ring[0]);

		memory = odpdrv_shm_reserve(NULL, sz_sw, ODP_CACHE_LINE_SIZE,
					    ODPDRV_SHM_LOCK);
		if (memory == ODPDRV_SHM_INVALID) {
			PMD_INIT_LOG(ERR, "can not allocate RX soft ring");
			ret = -ENOMEM;
			goto fail_q_alloc;
		}
		sw_ring = odpdrv_shm_addr(memory);
		memset(sw_ring, 0, sz_sw);

		vq->sw_ring = sw_ring;
		rxvq = (struct virtnet_rx *)RTE_PTR_ADD(vq, sz_vq);
		rxvq->vq = vq;
		rxvq->port_id = pktio_entry->s.pkt_virtio.port_id;
		rxvq->queue_id = queue_idx;
		rxvq->mz = mz;
		*pvq = rxvq;
	} else if (queue_type == VTNET_TQ) {
		txvq = (struct virtnet_tx *)RTE_PTR_ADD(vq, sz_vq);
		txvq->vq = vq;
		txvq->port_id = pktio_entry->s.pkt_virtio.port_id;
		txvq->queue_id = queue_idx;
		txvq->mz = mz;
		txvq->virtio_net_hdr_mz = hdr_mz;
		location = odpdrv_shm_addr(hdr_mz);
		txvq->virtio_net_hdr_mem = _odp_ishmphy_getphy(location);

		*pvq = txvq;
	} else if (queue_type == VTNET_CQ) {
		cvq = (struct virtnet_ctl *)RTE_PTR_ADD(vq, sz_vq);
		cvq->vq = vq;
		cvq->mz = mz;
		cvq->virtio_net_hdr_mz = hdr_mz;
		location = odpdrv_shm_addr(hdr_mz);
		cvq->virtio_net_hdr_mem = _odp_ishmphy_getphy(location);
		memset(location, 0, PAGE_SIZE);
		*pvq = cvq;
	}

#ifdef ACTIVATED
	/* For virtio_user case (that is when dev->pci_dev is NULL), we use
	* virtual address. And we need properly set _offset_, please see
	* VIRTIO_MBUF_DATA_DMA_ADDR in virtqueue.h for more information.
	*/
	if (dev->pci_dev) {
		vq->offset = offsetof(struct rte_mbuf, buf_physaddr);
	} else {
		vq->vq_ring_mem = (uintptr_t)mz;
		vq->offset = offsetof(struct rte_mbuf, buf_addr);
		if (queue_type == VTNET_TQ)
			txvq->virtio_net_hdr_mem = (uintptr_t)hdr_mz;
		else if (queue_type == VTNET_CQ)
			cvq->virtio_net_hdr_mem = (uintptr_t)hdr_mz;
	}
#endif

	if (queue_type == VTNET_TQ) {
		struct virtio_tx_region *txr;
		unsigned int i;

		txr = odpdrv_shm_addr(hdr_mz);
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

	if (hw->vtpci_ops->setup_queue(hw, vq) < 0) {
		PMD_INIT_LOG(1, "setup_queue failed");
		virtio_dev_queue_release(vq);
		return -EINVAL;
	}

	vq->configured = 1;
	return 0;

fail_q_alloc:
	odpdrv_shm_free_by_address(sw_ring);
	odpdrv_shm_free_by_handle(hdr_mz);
	odpdrv_shm_free_by_handle(mz);
	odpdrv_shm_free_by_address(vq);

	return ret;
}


static int virtio_dev_cq_queue_setup(pktio_entry_t *pktio_entry,
				     uint16_t vtpci_queue_idx,
				     uint32_t socket_id)
{
	struct virtnet_ctl *cvq;
	int ret;
	struct virtio_hw *hw = &pktio_entry->s.pkt_virtio.hw;

	PMD_INIT_FUNC_TRACE();
	ret = virtio_dev_queue_setup(pktio_entry,
				     VTNET_CQ,
				     VTNET_SQ_CQ_QUEUE_IDX,
				     vtpci_queue_idx, 0, socket_id,
				     (void **)&cvq);
	if (ret < 0) {
		PMD_INIT_LOG(1, "control vq initialization failed");
		return ret;
	}

	hw->cvq = cvq;
	return 0;
}

static int virtio_open(odp_pktio_t id ODP_UNUSED, pktio_entry_t *pktio_entry,
		       const char *devname, odp_pool_t pool ODP_UNUSED)
{
	const char *prefix = "pci:";
	pci_addr_t addr;
	int ret = 0;
	pci_dev_t* dev;
	struct virtio_hw* hw;
	struct virtio_net_config *config;
	struct virtio_net_config local_config;

	/* allow interface to be opened with or without the 'netmap:' prefix */

	if (strncmp(devname, prefix, strlen(prefix)) == 0)
		devname += strlen(prefix);

	ret =  parse_pci_addr_format(devname, &addr.domain,
				     &addr.bus, &addr.devid,
				     &addr.function);
	if (ret < 0)
		return -1;

	dev = _odp_pci_lookup_by_addr(&addr);

	if (dev != NULL) {
		ODP_PRINT("Found device %04" PRIx16 ":"
			  "%02" PRIx8 ":"
			  "%02" PRIx8 ":"
			  "%02" PRIx8 ": "
			  "vendor:%04" PRIx16 ", "
			  "device:%04" PRIx16 ", "
			  "class:%04" PRIx16 "\n",
			  dev->addr.domain,
			  dev->addr.bus,
			  dev->addr.devid,
			  dev->addr.function,
			  dev->id.vendor_id,
			  dev->id.device_id,
			  dev->id.class_id);
	} else {
		ODP_PRINT("Device %s not found\n", devname);
		return -1;
	}

	if (dev->id.vendor_id != 0x1af4 || dev->id.device_id != 0x1000)
	{
		ODP_PRINT("Device %d from vendor %d is not known\n",
			  dev->id.device_id, dev->id.vendor_id);
		return -2;
	}

	// now we know this is a virtio-net device
	ODP_PRINT("Found virtio-net at %s\n", devname);
	pktio_entry->s.ops = &virtio_pktio_ops;

#ifdef ACTIVATED
	if (!_odp_ishmphy_can_getphy()) {
		ODP_ERR("Platform does not allow to retrieve physical memory. Ignoring this device.\n");
		return -ENOMEM;
	}
#endif

	/* Allocate memory for storing MAC addresses */
	pktio_entry->s.pkt_virtio.mac_addrs = malloc(VIRTIO_MAX_MAC_ADDRS * ETHER_ADDR_LEN);
	if (pktio_entry->s.pkt_virtio.mac_addrs == NULL) {
		PMD_INIT_LOG(1,
			"Failed to allocate %d bytes needed to store MAC addresses",
			VIRTIO_MAX_MAC_ADDRS * ETHER_ADDR_LEN);
		return -ENOMEM;
	}
	memset(pktio_entry->s.pkt_virtio.mac_addrs, 0, VIRTIO_MAX_MAC_ADDRS * ETHER_ADDR_LEN);

	hw = &pktio_entry->s.pkt_virtio.hw;

	ret = vtpci_init(dev, hw);
	if (ret)
		return ret;

	/* Reset the device although not necessary at startup */
	vtpci_reset(hw);

	/* Tell the host we've noticed this device. */
	vtpci_set_status(hw, VIRTIO_CONFIG_STATUS_ACK);

	/* Tell the host we've known how to drive the device. */
	vtpci_set_status(hw, VIRTIO_CONFIG_STATUS_DRIVER);

	if (virtio_negotiate_features(hw) < 0)
		return -1;

	/* If host does not support status then disable LSC */
	if (!vtpci_with_feature(hw, VIRTIO_NET_F_STATUS))
		ODP_PRINT("dev_flags &= ~RTE_ETH_DEV_INTR_LSC\n");

	rx_func_get(pktio_entry);

	/* Setting up rx_header size for the device */
	if (vtpci_with_feature(hw, VIRTIO_NET_F_MRG_RXBUF) ||
	    vtpci_with_feature(hw, VIRTIO_F_VERSION_1))
		hw->vtnet_hdr_size = sizeof(struct virtio_net_hdr_mrg_rxbuf);
	else
		hw->vtnet_hdr_size = sizeof(struct virtio_net_hdr);

	/* Copy the permanent MAC address to: virtio_hw */
	virtio_get_hwaddr(hw);
	ether_addr_copy((struct ether_addr *)hw->mac_addr,
			(struct ether_addr *)&pktio_entry->s.pkt_virtio.mac_addrs[0]);
	PMD_INIT_LOG(1,
		     "PORT MAC: %02X:%02X:%02X:%02X:%02X:%02X",
		     hw->mac_addr[0], hw->mac_addr[1], hw->mac_addr[2],
		     hw->mac_addr[3], hw->mac_addr[4], hw->mac_addr[5]);

	if (vtpci_with_feature(hw, VIRTIO_NET_F_CTRL_VQ)) {
		config = &local_config;

		vtpci_read_dev_config(hw,
				      offsetof(struct virtio_net_config, mac),
				      &config->mac, sizeof(config->mac));

		if (vtpci_with_feature(hw, VIRTIO_NET_F_STATUS)) {
			vtpci_read_dev_config(hw,
					      offsetof(struct virtio_net_config,
						       status),
					      &config->status,
					      sizeof(config->status));
		} else {
			PMD_INIT_LOG(1,
				     "VIRTIO_NET_F_STATUS is not supported");
			config->status = 0;
		}

		if (vtpci_with_feature(hw, VIRTIO_NET_F_MQ)) {
			vtpci_read_dev_config(hw,
				offsetof(struct virtio_net_config, max_virtqueue_pairs),
				&config->max_virtqueue_pairs,
				sizeof(config->max_virtqueue_pairs));
		} else {
			PMD_INIT_LOG(1,
				"VIRTIO_NET_F_MQ is not supported");
			config->max_virtqueue_pairs = 1;
		}

		hw->max_rx_queues =
			(VIRTIO_MAX_RX_QUEUES < config->max_virtqueue_pairs) ?
			VIRTIO_MAX_RX_QUEUES : config->max_virtqueue_pairs;
		hw->max_tx_queues =
			(VIRTIO_MAX_TX_QUEUES < config->max_virtqueue_pairs) ?
			VIRTIO_MAX_TX_QUEUES : config->max_virtqueue_pairs;

		virtio_dev_cq_queue_setup(pktio_entry,
					  config->max_virtqueue_pairs * 2,
					  SOCKET_ID_ANY);


		PMD_INIT_LOG(1, "config->max_virtqueue_pairs=%d",
			     config->max_virtqueue_pairs);
		PMD_INIT_LOG(1, "config->status=%d", config->status);
		PMD_INIT_LOG(DEBUG,
			     "PORT MAC: %02X:%02X:%02X:%02X:%02X:%02X",
			     config->mac[0], config->mac[1],
			     config->mac[2], config->mac[3],
			     config->mac[4], config->mac[5]);
	} else {
		hw->max_rx_queues = 1;
		hw->max_tx_queues = 1;
	}

	PMD_INIT_LOG(1, "hw->max_rx_queues=%d   hw->max_tx_queues=%d",
		     hw->max_rx_queues, hw->max_tx_queues);
	if (dev)
		PMD_INIT_LOG(DEBUG, "port %d vendorID=0x%x deviceID=0x%x",
			     eth_dev->data->port_id, pci_dev->id.vendor_id,
			     pci_dev->id.device_id);

	/* Setup interrupt callback  */
#ifdef ACTIVATED
	if (pktio_entry->s.pkt_virtio.dev_flags & RTE_ETH_DEV_INTR_LSC)
		rte_intr_callback_register(&dev->intr_handle,
					   virtio_interrupt_handler, eth_dev);
#endif
#ifdef ACTIVATED
	virtio_dev_cq_start(pktio_entry);
#endif

	return 0;
}


#define pull_head(a, b)


static inline void
virtqueue_enqueue_xmit(struct virtnet_tx *txvq, odp_packet_t packet,
		       uint16_t needed, int use_indirect, int can_push)
{
	struct vq_desc_extra *dxp;
	struct virtqueue *vq = txvq->vq;
	struct vring_desc *start_dp;
	uint16_t seg_num = 1; // cookie->nb_segs;
	uint16_t head_idx, idx;
	uint16_t head_size ODP_UNUSED = vq->hw->vtnet_hdr_size ;
	unsigned long offs;
	odp_packet_hdr_t *pkt_hdr = odp_packet_hdr(packet);
	uint32_t remaining;
	void* location;

	head_idx = vq->vq_desc_head_idx;
	idx = head_idx;
	dxp = &vq->vq_descx[idx];
	dxp->cookie = (void *)packet;
	dxp->ndescs = needed;

	start_dp = vq->vq_ring.desc;

	if (can_push) {
		/* put on zero'd transmit header (no offloads) */
		void *hdr;
		pull_head(pkt_hdr, head_size);

		//void *hdr =  rte_pktmbuf_prepend(cookie, head_size);
		hdr = odp_packet_offset(packet, 0, &remaining, NULL);
		memset(hdr, 0, head_size);
	} else if (use_indirect) {
		/* setup tx ring slot to point to indirect
		* descriptor list stored in reserved region.
		*
		* the first slot in indirect ring is already preset
		* to point to the header in reserved region
		*/
		struct virtio_tx_region *txr =
			(struct virtio_tx_region *)odpdrv_shm_addr(txvq->virtio_net_hdr_mz);

		offs = idx * sizeof(struct virtio_tx_region)
			+ offsetof(struct virtio_tx_region, tx_indir);

		start_dp[idx].addr = txvq->virtio_net_hdr_mem + offs;
		start_dp[idx].len = (seg_num + 1) * sizeof(struct vring_desc);
		start_dp[idx].flags = VRING_DESC_F_INDIRECT;

		/* loop below will fill in rest of the indirect elements */
		start_dp = txr[idx].tx_indir;
		idx = 1;
	} else {
		/* setup first tx ring slot to point to header
		* stored in reserved region.
		*/
		offs = idx * sizeof(struct virtio_tx_region)
			+ offsetof(struct virtio_tx_region, tx_hdr);

		start_dp[idx].addr = txvq->virtio_net_hdr_mem + offs;
		start_dp[idx].len = vq->hw->vtnet_hdr_size;
		start_dp[idx].flags = VRING_DESC_F_NEXT;
		idx = start_dp[idx].next;
	}

	//do {
	   //start_dp[idx].addr = VIRTIO_MBUF_DATA_DMA_ADDR(cookie, vq);
	    location = odp_packet_offset(packet, 0, &remaining, NULL);
		start_dp[idx].addr = odpdrv_getphy(location); // is it physical or virtual address ????
		start_dp[idx].len = pkt_hdr->frame_len; // cookie->data_len;
		start_dp[idx].flags = 0; // cookie->next ? VRING_DESC_F_NEXT : 0;
		idx = start_dp[idx].next;
	//} while ((cookie = cookie->next) != NULL);

	if (use_indirect)
		idx = vq->vq_ring.desc[head_idx].next;

	vq->vq_desc_head_idx = idx;
	if (vq->vq_desc_head_idx == VQ_RING_DESC_CHAIN_END)
		vq->vq_desc_tail_idx = idx;

	vq->vq_free_cnt = (uint16_t)(vq->vq_free_cnt - needed);
	vq_update_avail_ring(vq, head_idx);
}



static int
virtio_xmit_pkts(pktio_entry_t *entry, int queue_index,
	const odp_packet_t packets[], int num)
	
{
	struct virtnet_tx *txvq = entry->s.pkt_virtio.tx_queues[queue_index];// tx_queue;
	struct virtqueue *vq = txvq->vq;
	struct virtio_hw *hw = vq->hw;
	uint16_t hdr_size ODP_UNUSED = hw->vtnet_hdr_size;
	uint16_t nb_used ODP_UNUSED;
	uint16_t nb_tx;
	//int error;

	if (odp_unlikely(num < 1))
		return num;

	PMD_TX_LOG(1, "%d packets to xmit", num);
	nb_used = VIRTQUEUE_NUSED(vq);

	virtio_rmb();
#ifdef ACTIVATED
	if (odp_likely(nb_used > vq->vq_nentries - vq->vq_free_thresh))
		virtio_xmit_cleanup(vq, nb_used);
#endif

	for (nb_tx = 0; nb_tx < num; nb_tx++) {
		int can_push = 0, use_indirect = 0, slots, need ODP_UNUSED;
#ifdef ACTIVATED
		struct rte_mbuf *txm = tx_pkts[nb_tx];

		/* Do VLAN tag insertion */
		if (odp_unlikely(txm->ol_flags & PKT_TX_VLAN_PKT)) {
			error = rte_vlan_insert(&txm);
			if (odp_unlikely(error)) {
				rte_pktmbuf_free(txm);
				continue;
			}
		}

		/* optimize ring usage */
		if (vtpci_with_feature(hw, VIRTIO_F_ANY_LAYOUT) &&
		    rte_mbuf_refcnt_read(txm) == 1 &&
		    RTE_MBUF_DIRECT(txm) &&
		    txm->nb_segs == 1 &&
		    rte_pktmbuf_headroom(txm) >= hdr_size &&
		    rte_is_aligned(rte_pktmbuf_mtod(txm, char *),
				   __alignof__(struct virtio_net_hdr_mrg_rxbuf)))
			can_push = 1;
		else if (vtpci_with_feature(hw, VIRTIO_RING_F_INDIRECT_DESC) &&
			 txm->nb_segs < VIRTIO_MAX_TX_INDIRECT)
			use_indirect = 1;

		/* How many main ring entries are needed to this Tx?
		* any_layout => number of segments
		* indirect   => 1
		* default    => number of segments + 1
		*/
		slots = use_indirect ? 1 : (txm->nb_segs + !can_push);
		need = slots - vq->vq_free_cnt;
#else
		slots = 1;
		need = slots - vq->vq_free_cnt;
#endif
#ifdef ACTIVATED
		/* Positive value indicates it need free vring descriptors */
		if (odp_unlikely(need > 0)) {
			nb_used = VIRTQUEUE_NUSED(vq);
			virtio_rmb();
			need = RTE_MIN(need, (int)nb_used);

			virtio_xmit_cleanup(vq, need);
			need = slots - vq->vq_free_cnt;
			if (odp_unlikely(need > 0)) {
				PMD_TX_LOG(ERR,
					"No free tx descriptors to transmit");
				break;
			}
		}
#endif
		/* Enqueue Packet buffers */
		virtqueue_enqueue_xmit(txvq, packets[nb_tx], slots,
				       use_indirect, can_push);

		txvq->stats.bytes += odp_packet_len(packets[nb_tx]);
#ifdef ACTIVATED
		virtio_update_packet_stats(&txvq->stats, txm);
#endif
	}

	txvq->stats.packets += nb_tx;

	if (odp_likely(nb_tx)) {
		vq_update_avail_idx(vq);

		if (odp_unlikely(virtqueue_kick_prepare(vq))) {
			virtqueue_notify(vq);
			PMD_TX_LOG(DEBUG, "Notified backend after xmit");
		}
	}

	return nb_tx;
}


static int virtio_close(pktio_entry_t *pktio_entry ODP_UNUSED)
{
	return 0;
}


static int virtio_init_global(void)
{
	// should not do anything at this level as the PCI bus has not been scanned yet
	return 0;
}

static int virtio_init_local(void)
{
	ODP_PRINT("PKTIO: local initializion of virtio interfaces.\n");
	return 0;
}

const pktio_if_ops_t virtio_pktio_ops = {
	.name = "virtio-net",
	.print = NULL,
	.init_global = virtio_init_global,
	.init_local = virtio_init_local,
	.term = NULL,
	.open = virtio_open,
	.close = virtio_close,
	.start = NULL,
	.stop = NULL,
	.stats = NULL,
	.stats_reset = NULL,
	.recv = NULL,
	.send = virtio_xmit_pkts,
	.mtu_get = NULL,
	.promisc_mode_set = NULL,
	.promisc_mode_get = NULL,
	.mac_get = NULL,
	.link_status = NULL,
	.capability = NULL,
	.pktin_ts_res = NULL,
	.pktin_ts_from_ns = NULL,
	.config = NULL,
	.input_queues_config = NULL,
	.output_queues_config = NULL,
};
