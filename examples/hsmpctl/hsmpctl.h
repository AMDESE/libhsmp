/* SPDX-License-Identifier: MIT License */
/*
 * Copyright (C) 2021 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Nathan Fontenot <nathan.fontenot@amd.com>
 *
 * AMD Host System Management Port command
 */

enum hsmp_msg_t {
	HSMPCTL_GET_VERSION = 1,
	HSMPCTL_SOCKET_POWER,
	HSMPCTL_SOCKET_POWER_LIMIT,
	HSMPCTL_SET_SOCKET_POWER_LIMIT,
	HSMPCTL_SOCKET_POWER_MAX,
	HSMPCTL_SET_CPU_BOOST_LIMIT,
	HSMPCTL_SET_SOCKET_BOOST_LIMIT,
	HSMPCTL_SET_SYSTEM_BOOST_LIMIT,
	HSMPCTL_CPU_BOOST_LIMIT,
	HSMPCTL_PROC_HOT,
	HSMPCTL_XGMI_WIDTH,
	HSMPCTL_XGMI_AUTO,
	HSMPCTL_DF_PSTATE,
	HSMPCTL_FABRIC_CLOCKS,
	HSMPCTL_CORE_CLOCK_MAX,
	HSMPCTL_C0_RESIDENCY,
	HSMPCTL_NBIO_PSTATE,
	HSMPCTL_NBIO_PSTATE_ALL,
	HSMPCTL_NBIO_NEXT_BUS,
	HSMPCTL_DDR_BW,
	HSMPCTLD_START,
	HSMPCTLD_EXIT,
};

struct hsmp_msg {
	enum hsmp_msg_t	msg_id;
	int		err;
	int		errnum;
	int		num_args;
	int		num_responses;
	int		args[8];
	int		response[8];
};

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define HSMPCTL_FIFO "/tmp/hsmpctl"
