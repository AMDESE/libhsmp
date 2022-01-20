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
#include <string.h>
#include <dirent.h>
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

static struct nbio_dev *nbios;
static int num_nbios;
static int nbios_per_socket;

struct nbio_dev *get_nbio(int idx)
{
	if (idx >= 0 && idx < num_nbios)
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

	if (socket_id < 0 || socket_id >= num_sockets)
		return NULL;

	idx = socket_id * nbios_per_socket;
	return &nbios[idx];
}

/*
 * Takes a PCI-e bus number and returns the index into the NBIOs array
 * matching the host NBIO device. Returns -1 if the bus is not found.
 */
struct nbio_dev *bus_to_nbio(u8 bus)
{
	int idx;

	for (idx = 0; idx < num_nbios; idx++) {
		if (bus >= nbios[idx].bus_base && bus <= nbios[idx].bus_limit)
			return &nbios[idx];
	}

	return NULL;
}

static int get_socket_count(void)
{
	struct dirent *dp;
	DIR *d;

	d = opendir("/sys/devices/system/node");
	if (!d)
		return errno;

	dp = readdir(d);
	while (dp) {
		if (!strncmp(dp->d_name, "node", 4))
			num_sockets++;

		dp = readdir(d);
	}

	closedir(d);
	return 0;
}

static int find_nbio_devs(void)
{
	struct pci_access *pacc;
	struct nbio_dev *nbio;
	struct pci_dev *dev;

	/* Setup pcilib */
	pacc = pci_alloc();
	if (!pacc) {
		pr_debug("Failed to allocate PCI access structures\n");
		return -1;
	}

	/*
	 * First, find all IOHC devices (root complex)
	 *
	 * We do this in a two step process first counting the number of
	 * IOHC devices, then a second pass to fill in the IOHC device
	 * structs.
	 */
	pci_init(pacc);
	pci_scan_bus(pacc);

	num_nbios = 0;
	for (dev = pacc->devices; dev; dev = dev->next) {
		pci_fill_info(dev, PCI_FILL_IDENT);

		if (dev->vendor_id != PCI_VENDOR_ID_AMD ||
		    dev->device_id != F17F19_IOHC_DEVID)
			continue;

		num_nbios++;
	}

	nbios = calloc(sizeof(*nbios), num_nbios);
	if (!nbios) {
		pr_debug("Failed to allocate nbio device array\n");
		pci_cleanup(pacc);
		return -ENOMEM;
	}

	memset(nbios, 0, sizeof(*nbios) * num_nbios);

	nbio = nbios;
	for (dev = pacc->devices; dev; dev = dev->next) {
		pci_fill_info(dev, PCI_FILL_IDENT);

		if (dev->vendor_id != PCI_VENDOR_ID_AMD ||
		    dev->device_id != F17F19_IOHC_DEVID)
			continue;

		pr_debug("Found IOHC dev on bus 0x%02X\n", dev->bus);
		nbio->dev = dev;
		nbio->bus_base = dev->bus;
		nbio++;
	}

	pci_cleanup(pacc);
	return 0;
}

int setup_nbios(void)
{
	uint8_t base;
	int i, err;

	err = get_socket_count();
	if (err)
		return err;

	err = find_nbio_devs();
	if (err)
		return err;

	nbios_per_socket = num_nbios / num_sockets;

	/* Sort the table by bus_base */
	for (i = 0; i < num_nbios - 1; i++) {
		int j;

		for (j = i + 1; j < num_nbios; j++) {
			if (nbios[j].bus_base < nbios[i].bus_base) {
				struct nbio_dev tmp_nbio = nbios[i];

				nbios[i] = nbios[j];
				nbios[j] = tmp_nbio;
			}
		}
	}

	/* Calculate bus limits - we can safely assume no overlapping ranges */
	for (i = 0; i < num_nbios; i++)
		nbios[i].bus_limit = nbios[i + 1].bus_base - 1;

	nbios[i - 1].bus_limit = 0xFF;

	/* Finally get IOHC ID for each bus base */
	for (i = 0; i < num_nbios; i++) {
		struct nbio_dev *nbio;
		uint32_t addr, val;
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

		nbio->id = i & nbios_per_socket;
		nbio->socket = i / nbios_per_socket;
		nbio->index = i;
	}

	/* Dump the final table */
#ifdef DEBUG_HSMP
	for (i = 0; i < num_nbios; i++) {
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

void cleanup_nbios(void)
{
	if (nbios) {
		free(nbios);
		nbios = NULL;
	}
}
