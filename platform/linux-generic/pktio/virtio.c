/*
 * Copyright (c) 2017, Linaro Limited
 *
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

static int virtio_init_global(void)
{
	ODP_PRINT("PKTIO: initialized virtio interface.\n");
	return 0;
}

static int virtio_open(odp_pktio_t id ODP_UNUSED, pktio_entry_t *pktio_entry,
		       const char *devname, odp_pool_t pool ODP_UNUSED)
{
	if (strcmp(devname, "pci"))
		return -1;

	strcpy(pktio_entry->s.pkt_virtio.name, "virtio_net");

	return 0;
}

static int virtio_close(pktio_entry_t *pktio_entry ODP_UNUSED)
{
	return 0;
}

static int virtio_recv(pktio_entry_t *pktio_entry ODP_UNUSED, int index ODP_UNUSED,
		       odp_packet_t pkts[] ODP_UNUSED, int len ODP_UNUSED)
{
	return 0;
}

static int virtio_send(pktio_entry_t *pktio_entry ODP_UNUSED, int index ODP_UNUSED,
		       const odp_packet_t pkt_tbl[] ODP_UNUSED, int len ODP_UNUSED)
{
	return 0;
}

const pktio_if_ops_t virtio_pktio_ops = {
	.name = "virio-net",
	.print = NULL,
	.init_global = virtio_init_global,
	.init_local = NULL,
	.term = NULL,
	.open = virtio_open,
	.close = virtio_close,
	.start = NULL,
	.stop = NULL,
	.stats = NULL,
	.stats_reset = NULL,
	.recv = virtio_recv,
	.send = virtio_send,
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
