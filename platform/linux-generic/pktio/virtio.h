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

#ifndef _VIRTIO_ETHDEV_H_
#define _VIRTIO_ETHDEV_H_

#include <stdint.h>

#include "virtio_pci.h"

#ifndef ETHER_ADDR_LEN
#define ETHER_ADDR_LEN 6
#endif

#define SPEED_10	10
#define SPEED_100	100
#define SPEED_1000	1000
#define SPEED_10G	10000

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define VIRTIO_MAX_RX_QUEUES 128
#define VIRTIO_MAX_TX_QUEUES 128
#define VIRTIO_MAX_MAC_ADDRS 64
#define VIRTIO_MIN_RX_BUFSIZE 64
#define VIRTIO_MAX_RX_PKTLEN  9728

/* Features desired/implemented by this driver. */
#define VIRTIO_PMD_GUEST_FEATURES		\
	(1u << VIRTIO_NET_F_MAC		  |	\
	 1u << VIRTIO_NET_F_STATUS	  |	\
	 1u << VIRTIO_NET_F_MQ		  |	\
	 1u << VIRTIO_NET_F_CTRL_MAC_ADDR |	\
	 1u << VIRTIO_NET_F_CTRL_VQ	  |	\
	 1u << VIRTIO_NET_F_CTRL_RX	  |	\
	 1u << VIRTIO_NET_F_CTRL_VLAN	  |	\
	 1u << VIRTIO_NET_F_MRG_RXBUF	  |	\
	 1ULL << VIRTIO_F_VERSION_1)

/*
* The VIRTIO_NET_F_GUEST_TSO[46] features permit the host to send us
* frames larger than 1514 bytes. We do not yet support software LRO
* via tcp_lro_rx().
*/
#define VTNET_LRO_FEATURES (VIRTIO_NET_F_GUEST_TSO4 | \
			    VIRTIO_NET_F_GUEST_TSO6 | VIRTIO_NET_F_GUEST_ECN)


#endif /* _VIRTIO_ETHDEV_H_ */