/* SPDX-License-Identifier: MIT License */
/*
 * Copyright (C) 2021 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Lewis Carroll <lewis.carroll@amd.com>
 *
 * Helper utilities for discovering NBIOs on the PCI bus and for reading
 * registers in SMN space.
 */

#ifndef _NBIO_DISCOVERY_
#define _NBIO_DISCOVERY_

#include <pci/types.h>
#include <stdbool.h>

#define MAX_SOCKETS	2
#define MAX_NBIOS	8

/* Returns 1 if the CPU family is 17h */
bool is_fam17h(void);

/*
 * Return the PCI device pointer for the lowest numbered IOHC in the
 * specified socket. If the specified socket does not exist, the returned
 * pointer will be NULL.
 */
struct pci_dev *socket_id_to_dev(int socket_id);

/*
 * Read a register in SMN space through PCI config space indirect access.
 * Returns zero for success, negative error code for failure.
 */
int smn_pci_read(struct pci_dev *root, u32 reg_addr, u32 *reg_data);

/*
 * Takes a PCI-e bus number and returns the index into the NBIOs array
 * matching the host NBIO device. Returns -ENODEV if the bus is not found.
 */
int bus_to_nbio(u8 bus);

/*
 * First function that should be called to discover IOHC devices in the
 * system and the PCI busses hosted by each one. Upon success, this will
 * return 0 and num_sockets will be set to the discovered number of sockets
 * in the system. If there is an error, num_sockets is unchanged and a
 * negative error code is returned. Providing a NULL pointer for num_sockets
 * is allowed. This function must be called as root.
 */
int setup_nbios(int *num_sockets);

/* Clean-up the PCI library data used for setting up NBIOs. */
void cleanup_nbios(void);

#endif
