/* SPDX-License-Identifier: MIT License */
/*
 * Copyright (C) 2020 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Nathan Fontenot <nathan.fontenot@amd.com>
 * Author: Lewis Carroll <lewis.carroll@amd.com>
 *
 * AMD Host System Management Port library
 */

#ifndef _NBIO_H_
#define _NBIO_H_ 1

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "libhsmp.h"

#define PCI_VENDOR_ID_AMD	0x1022
#define F17F19_IOHC_DEVID	0x1480

int num_sockets;

struct nbio_dev {
	struct pci_dev	*dev;	   /* Pointer to PCI-e device in the socket */
	uint8_t		id;	   /* NBIO tile number within the socket */
	uint8_t		bus_base;  /* Lowest hosted PCI-e bus number */
	uint8_t		bus_limit; /* Highest hosted PCI-e bus number + 1 */
	int		index;
	int		socket;
};

struct nbio_dev *get_nbio(int idx);
struct nbio_dev *socket_id_to_nbio(int socket_id);
struct nbio_dev *bus_to_nbio(uint8_t bus_num);
void clear_nbio_table(void);
void cleanup_nbios(void);
int setup_nbios(void);

#endif
