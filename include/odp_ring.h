/* Copyright (c) 2014, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

/*
 * Derived from FreeBSD's bufring.c
 *
 **************************************************************************
 *
 * Copyright (c) 2007,2008 Kip Macy kmacy@freebsd.org
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. The name of Kip Macy nor the names of other
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ***************************************************************************/

/**
 * ODP Ring
 *
 * The Ring Manager is a fixed-size queue, implemented as a table of
 * pointers. Head and tail pointers are modified atomically, allowing
 * concurrent access to it. It has the following features:
 *
 * - FIFO (First In First Out)
 * - Maximum size is fixed; the pointers are stored in a table.
 * - Lockless implementation.
 * - Multi- or single-consumer dequeue.
 * - Multi- or single-producer enqueue.
 * - Bulk dequeue.
 * - Bulk enqueue.
 *
 * Note: the ring implementation is not preemptable. A lcore must not
 * be interrupted by another task that uses the same ring.
 *
 */

#ifndef ODP_RING_H_
#define ODP_RING_H_

#ifdef __cplusplus
extern "C" {
#endif


#include <odp_std_types.h>
#include <odp_hints.h>
#include <odp_atomic.h>
#include <errno.h>

enum odp_ring_queue_behavior {
	ODP_RING_QUEUE_FIXED = 0, /* Enq/Deq a fixed number
				of items from a ring */
	ODP_RING_QUEUE_VARIABLE   /* Enq/Deq as many items
				a possible from ring */
};


#define ODP_RING_NAMESIZE 32 /* The maximum length of a ring name. */

/**
 * An ODP ring structure.
 *
 * The producer and the consumer have a head and a tail index. The particularity
 * of these index is that they are not between 0 and size(ring). These indexes
 * are between 0 and 2^32, and we mask their value when we access the ring[]
 * field. Thanks to this assumption, we can do subtractions between 2 index
 * values in a modulo-32bit base: that's why the overflow of the indexes is not
 * a problem.
 */
typedef struct {
	char name[ODP_RING_NAMESIZE];    /* Name of the ring. */
	int flags;                       /* Flags supplied at creation. */

	struct prod {
		uint32_t watermark;      /* Maximum items */
		uint32_t sp_enqueue;     /* True, if single producer. */
		uint32_t size;           /* Size of ring. */
		uint32_t mask;           /* Mask (size-1) of ring. */
		uint32_t head;		/* Producer head. */
		uint32_t tail;		/* Producer tail. */
	} prod ODP_ALIGNED_CACHE;

	struct cons {
		uint32_t sc_dequeue;     /* True, if single consumer. */
		uint32_t size;           /* Size of the ring. */
		uint32_t mask;           /* Mask (size-1) of ring. */
		uint32_t head;		/* Consumer head. */
		uint32_t tail;		/* Consumer tail. */
	} cons ODP_ALIGNED_CACHE;

	void *ring[0] ODP_ALIGNED_CACHE;/* Memory space of ring starts here. */
} odp_ring_t;


#define RING_F_SP_ENQ 0x0001 /* The default enqueue is "single-producer". */
#define RING_F_SC_DEQ 0x0002 /* The default dequeue is "single-consumer". */
#define ODP_RING_QUOT_EXCEED (1 << 31)  /* Quota exceed for burst ops */
#define ODP_RING_SZ_MASK  (unsigned)(0x0fffffff) /* Ring size mask */


/**
 * Create a new ring named *name* in memory.
 *
 * This function uses odp_shm_reserve() to allocate memory. Its size is
 * set to *count*, which must be a power of two. Water marking is
 * disabled by default. Note that the real usable ring size is count-1
 * instead of count.
 *
 * @param name
 *   The name of the ring.
 * @param count
 *   The size of the ring (must be a power of 2).
 * @param socket_id (dummy, not included : todo)
 * @param flags
 *   An OR of the following:
 *    - RING_F_SP_ENQ: If this flag is set, the default behavior when
 *      using ``odp_ring_enqueue()`` or ``odp_ring_enqueue_bulk()``
 *      is "single-producer". Otherwise, it is "multi-producers".
 *    - RING_F_SC_DEQ: If this flag is set, the default behavior when
 *      using ``odp_ring_dequeue()`` or ``odp_ring_dequeue_bulk()``
 *      is "single-consumer". Otherwise, it is "multi-consumers".
 * @return
 *   On success, the pointer to the new allocated ring. NULL on error with
 *    odp_errno set appropriately. Possible errno values include:
 *    - EINVAL - count provided is not a power of 2
 *    - ENOSPC - the maximum number of memzones has already been allocated
 *    - EEXIST - a memzone with the same name already exists
 *    - ENOMEM - no appropriate memory area found in which to create memzone
 */
odp_ring_t *odp_ring_create(const char *name, unsigned count,
						unsigned flags);


/**
 * Change the high water mark.
 *
 * If *count* is 0, water marking is disabled. Otherwise, it is set to the
 * *count* value. The *count* value must be greater than 0 and less
 * than the ring size.
 *
 * This function can be called at any time (not necessarily at
 * initialization).
 *
 * @param r  Pointer to the ring structure.
 * @param count New water mark value.
 * @return 0: Success; water mark changed.
 *		-EINVAL: Invalid water mark value.
 */
int odp_ring_set_water_mark(odp_ring_t *r, unsigned count);

/**
 * Dump the status of the ring to the console.
 *
 * @param r A pointer to the ring structure.
 */
void odp_ring_dump(const odp_ring_t *r);

/**
 * Enqueue several objects on the ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the ring from the obj_table.
 * @param behavior
 *   ODP_RING_QUEUE_FIXED:    Enqueue a fixed number of items from a ring
 *   ODP_RING_QUEUE_VARIABLE: Enqueue as many items a possible from ring
 * @return
 *   Depend on the behavior value
 *   if behavior = ODP_RING_QUEUE_FIXED
 *   - 0: Success; objects enqueue.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue, no object is enqueued.
 *   if behavior = ODP_RING_QUEUE_VARIABLE
 *   - n: Actual number of objects enqueued.
 */
int __odp_ring_mp_do_enqueue(odp_ring_t *r, void * const *obj_table,
			 unsigned n, enum odp_ring_queue_behavior behavior);

/**
 * Dequeue several objects from a ring (multi-consumers safe). When
 * the request objects are more than the available objects, only dequeue the
 * actual number of objects
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * @param behavior
 *   ODP_RING_QUEUE_FIXED:    Dequeue a fixed number of items from a ring
 *   ODP_RING_QUEUE_VARIABLE: Dequeue as many items a possible from ring
 * @return
 *   Depend on the behavior value
 *   if behavior = ODP_RING_QUEUE_FIXED
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue; no object is
 *     dequeued.
 *   if behavior = ODP_RING_QUEUE_VARIABLE
 *   - n: Actual number of objects dequeued.
 */

int __odp_ring_mc_do_dequeue(odp_ring_t *r, void **obj_table,
			 unsigned n, enum odp_ring_queue_behavior behavior);

/**
 * Enqueue several objects on the ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the ring from the obj_table.
 * @return
 *   - 0: Success; objects enqueue.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue, no object is enqueued.
 */
int odp_ring_mp_enqueue_bulk(odp_ring_t *r, void * const *obj_table,
				unsigned n);

/**
 * Dequeue several objects from a ring (multi-consumers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * @return
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue; no object is
 *     dequeued.
 */
int odp_ring_mc_dequeue_bulk(odp_ring_t *r, void **obj_table, unsigned n);

/**
 * Test if a ring is full.
 *
 * @param r
 *   A pointer to the ring structure.
 * @return
 *   - 1: The ring is full.
 *   - 0: The ring is not full.
 */
int odp_ring_full(const odp_ring_t *r);

/**
 * Test if a ring is empty.
 *
 * @param r
 *   A pointer to the ring structure.
 * @return
 *   - 1: The ring is empty.
 *   - 0: The ring is not empty.
 */
int odp_ring_empty(const odp_ring_t *r);

/**
 * Return the number of entries in a ring.
 *
 * @param r
 *   A pointer to the ring structure.
 * @return
 *   The number of entries in the ring.
 */
unsigned odp_ring_count(const odp_ring_t *r);

/**
 * Return the number of free entries in a ring.
 *
 * @param r
 *   A pointer to the ring structure.
 * @return
 *   The number of free entries in the ring.
 */
unsigned odp_ring_free_count(const odp_ring_t *r);

/**
 * search ring by name
 * @param name	ring name to search
 * @return	pointer to ring otherwise NULL
 */
odp_ring_t *odp_ring_lookup(const char *name);

/*todo: dump the status of all rings on the console */
void odp_ring_list_dump(void);

#ifdef __cplusplus
}
#endif

#endif
