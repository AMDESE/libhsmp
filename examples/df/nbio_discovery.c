// SPDX-License-Identifier: MIT License
/*
 * Copyright (C) 2021 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Lewis Carroll <lewis.carroll@amd.com>
 *
 * Helper utilities for discovering NBIOs on the PCI bus and for reading
 * registers in SMN space.
 */

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <cpuid.h>
#include <pci/pci.h>

#include "nbio_discovery.h"

/* Uncomment to enable debugging PCI bus probing and NBIO discovery */
// #define DEBUG

/* Uncomment to enable low-level PCI library access debugging */
// #define DEBUG_PCI

#ifdef DEBUG
#define pr_debug(...)   printf("[NBIO Discovery] " __VA_ARGS__)
#else
#define pr_debug(...)   ((void)0)
#endif

#ifdef DEBUG_PCI
#define pr_debug_pci(...)	printf("[PCI Access] " __VA_ARGS__)
#else
#define pr_debug_pci(...)	((void)0)
#endif

/* Array of DevID 0x1480 devices */
static struct nbio_dev {
	struct pci_dev *dev;		/* Pointer to PCI-e device in the socket */
	u8		id;		/* NBIO tile number within the socket */
	u8		bus_base;	/* Lowest hosted PCI-e bus number */
	u8		bus_limit;	/* Highest hosted PCI-e bus number + 1 */
} nbios[MAX_NBIOS];

static struct pci_access *pacc;	/* For PCIlib */

bool is_fam17h(void)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned int family, model;

	__cpuid(1, eax, ebx, ecx, edx);
	family = (eax >> 8) & 0xf;
	model = (eax >> 4) & 0xf;

	if (family == 0xf)
		family += (eax >> 20) & 0xff;

	if (family >= 6)
		model += ((eax >> 16) & 0xf) << 4;

	return (family = 0x17);
}

struct pci_dev *socket_id_to_dev(int socket_id)
{
	int idx;

	if (socket_id < 0 || socket_id >= MAX_SOCKETS)
		return NULL;

	idx = socket_id * 4;  /* Four IOHC devices per socket */
	return nbios[idx].dev;
}

/*
 * Indirect access to register in SMN address space. Write the address of
 * the register to be accessed into the index register (which lives in the
 * PCI configuration space for an IOHC device), than read the data register
 * to get the value of the SMN register.
 */
#define SMN_INDEX_REG	0x60  /* Offsets in PCI config space */
#define SMN_DATA_REG	0x64

int smn_pci_read(struct pci_dev *root, u32 reg_addr, u32 *reg_data)
{
	if (!root || !reg_data)
		return -EINVAL;

	pr_debug_pci("pci_write_long dev 0x%p, addr 0x%08X, data 0x%08X\n",
		     root, SMN_INDEX_REG, reg_addr);
	pci_write_long(root, SMN_INDEX_REG, reg_addr);

	*reg_data = pci_read_long(root, SMN_DATA_REG);
	pr_debug_pci("pci_read_long  dev 0x%p, addr 0x%08X, data 0x%08X\n",
		     root, SMN_DATA_REG, *reg_data);

	return 0;
}

int bus_to_nbio(u8 bus)
{
	int idx;

	for (idx = 0; idx < MAX_NBIOS; idx++) {
		if (bus >= nbios[idx].bus_base &&
		    bus <= nbios[idx].bus_limit)
			return idx;
	}

	return -ENODEV;
}

#define PCI_VENDOR_ID_AMD		0x1022
#define F17F19_IOHC_DEVID		0x1480
#define SMN_IOHCMISC0_NB_BUS_NUM_CNTL	0x13B10044  /* Address in SMN space */
#define SMN_IOHCMISC_OFFSET		0x00100000  /* Offset for MISC[1..3] */

int setup_nbios(int *num_sockets)
{
	struct pci_dev *dev;
	int num_nbios, i, err;
	u8 base;

	/* Setup pcilib */
	pacc = pci_alloc();
	if (!pacc) {
		pr_debug("Failed to allocate PCI access structures\n");
		return -ENODEV;
	}

	/* Set up the NBIO bus base/limit table to prepare for sorting */
	for (i = 0; i < MAX_NBIOS; i++)
		nbios[i].bus_base = 0xFF;

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
			goto setup_error;
		}

		nbios[num_nbios].dev = dev;
		nbios[num_nbios].bus_base = base;
		num_nbios++;
	}

	/* Four IOHC devices per socket so verify we found four or eight */
	if (num_nbios == 0 || (num_nbios % (MAX_NBIOS / 2))) {
		pr_debug("Expected %d or %d IOHC devices, found %d\n",
			 MAX_NBIOS / 2, MAX_NBIOS, num_nbios);
		goto setup_error;
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
	for (i = 0; i < num_nbios; i++) {
		if (i < num_nbios - 1)
			nbios[i].bus_limit = nbios[i + 1].bus_base - 1;
		else
			nbios[i].bus_limit = 0xFF;
	}

	/* Finally get IOHC ID for each bus base */
	for (i = 0; i < num_nbios; i++) {
		int idx;
		u32 addr, val;

		addr = SMN_IOHCMISC0_NB_BUS_NUM_CNTL + (i & 0x3) * SMN_IOHCMISC_OFFSET;
		err = smn_pci_read(nbios[i].dev, addr, &val);
		if (err) {
			pr_debug("Error %d accessing socket %d IOHCMISC%d\n",
				 err, i >> 2, i & 0x3);
			goto setup_error;
		}

		pr_debug("Socket %d IOHC%d smu_read addr 0x%08X = 0x%08X\n",
			 i >> 2, i & 0x3, addr, val);
		base = val & 0xFF;

		/* Look up this bus base in our array */
		idx = bus_to_nbio(base);
		if (idx == -1) {
			pr_debug("Unable to map bus 0x%02X to an IOHC device\n", base);
			goto setup_error;
		}

		nbios[idx].id = i & 0x3;  /* NBIO ID number is per socket */
	}

	/* Dump the final table */
#ifdef DEBUG
	for (i = 0; i < MAX_NBIOS; i++) {
		pr_debug("IDX %d: Bus range 0x%02X - 0x%02X --> Socket %d IOHC %d\n",
			 i, nbios[i].bus_base, nbios[i].bus_limit,
			 i >> 2, nbios[i].id);
	}
#endif

	if (num_sockets)
		*num_sockets = num_nbios / 4;

	return 0;

setup_error:
	cleanup_nbios();

	return -ENODEV;
}

void cleanup_nbios(void)
{
	pci_cleanup(pacc);
}
