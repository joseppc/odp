/* Copyright (c) 2016, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#ifndef _ISHMPHY_INTERNAL_H
#define _ISHMPHY_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <odp/drv/shm.h>

typedef odpdrv_phys_addr_t phys_addr_t; /* Physical address definition. */
#define PHYS_ADDR_INVALID ((phys_addr_t)-1)

void *_odp_ishmphy_book_va(uintptr_t len, intptr_t align);
int   _odp_ishmphy_unbook_va(void);
void *_odp_ishmphy_map(int fd, void *start, uint64_t size, int flags);
int   _odp_ishmphy_unmap(void *start, uint64_t len, int flags);
phys_addr_t _odp_ishmphy_getphy(const void *addr);

#ifdef __cplusplus
}
#endif

#endif
