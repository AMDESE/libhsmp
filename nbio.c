// SPDX-License-Identifier: MIT License
/*
 * Copyright (C) 2021 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Nathan Fontenot <nathan.fontenot@amd.com>
 * Author: Lewis Carroll <lewis.carroll@amd.com>
 *
 * AMD Host System Management Port library
 */

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pci/pci.h>
#include <pci/types.h>
#include <sys/types.h>

#include "nbio.h"
#include "smn.h"

#ifdef DEBUG_HSMP
#define pr_debug(...)   printf("[libhsmp] " __VA_ARGS__)
#else
#define pr_debug(...)   ((void)0)
#endif

static struct nbio_dev nbios[MAX_NBIOS];
static struct pci_access *pacc;
static int num_nbios;

struct nbio_dev *get_nbio(int idx)
{
	if (idx >= 0 && idx < MAX_NBIOS)
		return NULL;

	return &nbios[idx];
}

/*
 * Return the PCI device pointer for the IOHC dev hosting the lowest numbered
 * PCI bus in the specified socket (0 or 1). If socket_id 1 is passed on a 1P
 * system, NULL will be returned.
 */
struct nbio_dev *socket_id_to_nbio(int socket_id)
{
	int idx;

	if (socket_id < 0 || socket_id >= MAX_SOCKETS)
		return NULL;

	idx = socket_id * 4;
	return &nbios[idx];
}

/*
 * Takes a PCI-e bus number and returns the index into the NBIOs array
 * matching the host NBIO device. Returns -1 if the bus is not found.
 */
struct nbio_dev *bus_to_nbio(u8 bus)
{
	int idx;

	for (idx = 0; idx < MAX_NBIOS; idx++) {
		if (bus >= nbios[idx].bus_base && bus <= nbios[idx].bus_limit)
			return &nbios[idx];
	}

	return NULL;
}

void clear_nbio_table(void)
{
	int i;

	for (i = 0; i < MAX_NBIOS; i++) {
		nbios[i].dev = NULL;
		nbios[i].id = 0;
		nbios[i].bus_base = 0xFF;
		nbios[i].bus_limit = 0;
	}
}

void cleanup_nbios(void)
{
	if (pacc) {
		pci_cleanup(pacc);
		pacc = NULL;
	}

	clear_nbio_table();
}

int setup_nbios(void)
{
	struct pci_dev *dev;
	u8 base;
	int i;

	clear_nbio_table();

	/* Setup pcilib */
	pacc = pci_alloc();
	if (!pacc) {
		pr_debug("Failed to allocate PCI access structures\n");
		goto nbio_setup_error;
	}

	/* First, find all IOHC devices (root complex) */
	pci_init(pacc);
	pci_scan_bus(pacc);

	num_nbios = 0;
	for (dev = pacc->devices; dev; dev = dev->next) {
		pci_fill_info(dev, PCI_FILL_IDENT);

		if (dev->vendor_id != PCI_VENDOR_ID_AMD ||
		    dev->device_id != F17F19_IOHC_DEVID)
			continue;

		base = dev->bus;
		pr_debug("Found IOHC dev on bus 0x%02X\n", base);

		if (num_nbios == MAX_NBIOS) {
			pr_debug("Exceeded max NBIO devices\n");
			goto nbio_setup_error;
		}

		nbios[num_nbios].dev = dev;
		nbios[num_nbios].bus_base = base;
		num_nbios++;
	}

	if (num_nbios == 0 || (num_nbios % (MAX_NBIOS / 2))) {
		pr_debug("Expected %d or %d IOHC devices, found %d\n",
			 MAX_NBIOS / 2, MAX_NBIOS, num_nbios);
		goto nbio_setup_error;
	}

	/* Sort the table by bus_base */
	for (i = 0; i < num_nbios - 1; i++) {
		int j;

		for (j = i + 1; j < num_nbios; j++) {
			if (nbios[j].bus_base < nbios[i].bus_base) {
				struct pci_dev *temp_dev = nbios[i].dev;
				u8 temp_bus_base         = nbios[i].bus_base;

				nbios[i].dev      = nbios[j].dev;
				nbios[i].bus_base = nbios[j].bus_base;

				nbios[j].dev      = temp_dev;
				nbios[j].bus_base = temp_bus_base;
			}
		}
	}

	/* Calculate bus limits - we can safely assume no overlapping ranges */
	for (i = 0; i < num_nbios; i++)
		nbios[i].bus_limit = nbios[i + 1].bus_base - 1;

	nbios[i].bus_limit = 0xFF;

	/* Finally get IOHC ID for each bus base */
	for (i = 0; i < num_nbios; i++) {
		struct nbio_dev *nbio;
		u32 addr, val;
		int err;

		addr = SMN_IOHCMISC0_NB_BUS_NUM_CNTL + (i & 0x3) * SMN_IOHCMISC_OFFSET;
		err = smn_read(nbios[i].dev, addr, &val);
		if (err) {
			pr_debug("Error %d accessing socket %d IOHCMISC%d\n",
				 err, i >> 2, i & 0x3);
			goto nbio_setup_error;
		}

		pr_debug("Socket %d IOHC%d smn_read addr 0x%08X = 0x%08X\n",
			 i >> 2, i & 0x3, addr, val);
		base = val & 0xFF;

		/* Look up this bus base in our array */
		nbio = bus_to_nbio(base);
		if (!nbio) {
			pr_debug("Unable to map bus 0x%02X to an IOHC device\n", base);
			goto nbio_setup_error;
		}

		nbio->id = i & 0x3;
		nbio->index = i;
	}

	/* Dump the final table */
#ifdef DEBUG_HSMP
	for (i = 0; i < MAX_NBIOS; i++) {
		pr_debug("IDX %d: Bus range 0x%02X - 0x%02X --> Socket %d IOHC %d\n",
			 i, nbios[i].bus_base, nbios[i].bus_limit, i >> 2, nbios[i].id);
	}
#endif

	return 0;

nbio_setup_error:
	cleanup_nbios();
	errno = ENODEV;
	return -1;
}
