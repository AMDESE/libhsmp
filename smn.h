/* SPDX-License-Identifier: MIT License */
/*
 * Copyright (C) 2021 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Nathan Fontenot <nathan.fontenot@amd.com>
 */

#ifndef _SMU_H_
#define _SMU_H_ 1

#include <unistd.h>
#include <stdint.h>
#include <pci/pci.h>
#include <pci/types.h>

#define SMN_IOHCMISC0_NB_BUS_NUM_CNTL	0x13B10044	/* Address in SMN space */
#define SMN_IOHCMISC_OFFSET		0x00100000	/* Offset for MISC[1..3] */

int smn_read(struct pci_dev *root, u32 reg_addr, u32 *reg_data);

int hsmp_read(struct pci_dev *root, u32 reg_addr, u32 *reg_data);
int hsmp_write(struct pci_dev *root, u32 reg_addr, u32 reg_data);

#endif
