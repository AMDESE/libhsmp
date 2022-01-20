// SPDX-License-Identifier: MIT License
/*
 * Copyright (C) 2021 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Nathan Fontenot <nathan.fontenot@amd.com>
 *
 * AMD Host System Management Port library
 */

#include <stdio.h>
#include <stdint.h>

#include "smn.h"

#ifdef DEBUG_HSMP_PCI
#define pr_debug_pci(...)       printf("[libhsmp] " __VA_ARGS__)
#else
#define pr_debug_pci(...)       ((void)0)
#endif

struct smn_pci_port {
	uint32_t index_reg;  /* PCI-e index register for SMN access */
	uint32_t data_reg;   /* PCI-e data register for SMN access */
};

static struct smn_pci_port smn_port = {
	.index_reg = 0x60,
	.data_reg  = 0x64,
};

static struct smn_pci_port hsmp_port = {
	.index_reg = 0xC4,
	.data_reg  = 0xC8,
};

#define SMN_READ	0
#define SMN_WRITE	1

/*
 * SMN access functions
 * Returns 0 on success, negative error code on failure. The return status
 * is for the SMN access, not the result of the intended SMN or HSMP operation.
 *
 * SMN PCI config space access method
 * There are two access apertures defined in the PCI-e config space for the
 * North Bridge, one for general purpose SMN register reads/writes and a second
 * aperture specific for HSMP messages and responses. For both reads and writes,
 * step one is to write the register to be accessed to the appropriate aperture
 * index register. Step two is to read or write the appropriate aperture data
 * register.
 */
static int smn_pci_rdwr(struct pci_dev *root, uint32_t reg_addr,
			uint32_t *reg_data, struct smn_pci_port *port, int rdwr)
{
	pr_debug_pci("pci_write_long dev 0x%p, addr 0x%08X, data 0x%08X\n",
		     root, port->index_reg, reg_addr);
	pci_write_long(root, port->index_reg, reg_addr);

	if (rdwr == SMN_READ) {
		*reg_data = pci_read_long(root, port->data_reg);
		pr_debug_pci("pci_read_long  dev 0x%p, addr 0x%08X, data 0x%08X\n",
			     root, port->data_reg, *reg_data);
	} else {
		pr_debug_pci("pci_write_long dev 0x%p, addr 0x%08X, data 0x%08X\n",
			     root, port->index_reg, reg_addr);
		pci_write_long(root, port->data_reg, *reg_data);
	}

	return 0;
}

int smn_read(struct pci_dev *root, uint32_t reg_addr, uint32_t *reg_data)
{
	return smn_pci_rdwr(root, reg_addr, reg_data, &smn_port, SMN_READ);
}

int hsmp_read(struct pci_dev *root, uint32_t reg_addr, uint32_t *reg_data)
{
	return smn_pci_rdwr(root, reg_addr, reg_data, &hsmp_port, SMN_READ);
}

int hsmp_write(struct pci_dev *root, uint32_t reg_addr, uint32_t reg_data)
{
	return smn_pci_rdwr(root, reg_addr, &reg_data, &hsmp_port, SMN_WRITE);
}
