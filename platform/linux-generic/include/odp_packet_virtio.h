#pragma once

#include <pktio/virtio.h>
typedef uint8_t mac_t[ETHER_ADDR_LEN];

typedef struct {
	struct virtio_hw hw;
	char name[32]; /**< Unique identifier name */
	mac_t* mac_addrs;
	void **rx_queues; /**< Array of pointers to RX queues. */
	void **tx_queues; /**< Array of pointers to TX queues. */
	uint16_t nb_rx_queues; /**< Number of RX queues. */
	uint16_t nb_tx_queues; /**< Number of TX queues. */
	uint16_t mtu;                   /**< Maximum Transmission Unit. */
	uint32_t min_rx_buf_size;
	uint32_t dev_flags; /**< Capabilities */
	uint32_t port_id;
} pkt_virtio_t;

