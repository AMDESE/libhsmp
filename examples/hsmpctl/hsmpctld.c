// SPDX-License-Identifier: MIT License
/*
 * Copyright (C) 2021 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Nathan Fontenot <nathan.fontenot@amd.com>
 *
 * AMD Host System Management Port command daemon
 */

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "../../libhsmp.h"
#include "hsmpctl.h"

static int valid_num_args(struct hsmp_msg *msg, int expected_args)
{
	if (msg->num_args != expected_args) {
		msg->err = -1;
		msg->errnum = EINVAL;
		return 0;
	}

	return 1;
}

static void hsmpctld_get_version(struct hsmp_msg *msg)
{
	struct smu_fw_version smu_fw;
	int version;
	int err;

	err = hsmp_smu_fw_version(&smu_fw);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
		return;
	}

	err = hsmp_interface_version(&version);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
		return;
	}

	msg->num_responses = 4;
	msg->response[0] = smu_fw.major;
	msg->response[1] = smu_fw.minor;
	msg->response[2] = smu_fw.debug;
	msg->response[3] = version;
}

static void hsmpctld_socket_power(struct hsmp_msg *msg)
{
	int socket;
	u32 power;
	int err;

	if (!valid_num_args(msg, 1))
		return;

	socket = msg->args[0];

	err = hsmp_socket_power(socket, &power);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
		return;
	}

	msg->num_responses = 1;
	msg->response[0] = power;
}

static void hsmpctld_socket_power_limit(struct hsmp_msg *msg)
{
	int socket;
	u32 power;
	int err;

	if (!valid_num_args(msg, 1))
		return;

	socket = msg->args[0];

	err = hsmp_socket_power_limit(socket, &power);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
		return;
	}

	msg->num_responses = 1;
	msg->response[0] = power;
}

static void hsmpctld_set_socket_power_limit(struct hsmp_msg *msg)
{
	int socket;
	u32 power;
	int err;

	if (!valid_num_args(msg, 2))
		return;

	socket = msg->args[0];
	power = msg->args[1];

	err = hsmp_set_socket_power_limit(socket, power);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}
}

static void hsmpctld_socket_power_max(struct hsmp_msg *msg)
{
	int socket;
	u32 power;
	int err;

	if (!valid_num_args(msg, 1))
		return;

	socket = msg->args[0];

	err = hsmp_socket_max_power_limit(socket, &power);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}

	msg->num_responses = 1;
	msg->response[0] = power;
}

static void hsmpctld_set_cpu_boost_limit(struct hsmp_msg *msg)
{
	int boost_limit;
	int cpu;
	int err;

	if (!valid_num_args(msg, 2))
		return;

	cpu = msg->args[0];
	boost_limit = msg->args[1];

	err = hsmp_set_cpu_boost_limit(cpu, boost_limit);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}
}

static void hsmpctld_set_socket_boost_limit(struct hsmp_msg *msg)
{
	int boost_limit;
	int socket;
	int err;

	if (!valid_num_args(msg, 2))
		return;

	socket = msg->args[0];
	boost_limit = msg->args[1];

	err = hsmp_set_socket_boost_limit(socket, boost_limit);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}
}

static void hsmpctld_set_system_boost_limit(struct hsmp_msg *msg)
{
	int boost_limit;
	int err;

	if (!valid_num_args(msg, 1))
		return;

	boost_limit = msg->args[0];

	err = hsmp_set_system_boost_limit(boost_limit);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}
}

static void hsmpctld_cpu_boost_limit(struct hsmp_msg *msg)
{
	u32 boost_limit;
	int cpu;
	int err;

	if (!valid_num_args(msg, 1))
		return;

	cpu = msg->args[0];

	err = hsmp_cpu_boost_limit(cpu, &boost_limit);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}

	msg->num_responses = 1;
	msg->response[0] = boost_limit;
}

static void hsmpctld_proc_hot(struct hsmp_msg *msg)
{
	int proc_hot;
	int socket;
	int err;

	if (!valid_num_args(msg, 1))
		return;

	socket = msg->args[0];

	err = hsmp_proc_hot_status(socket, &proc_hot);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}

	msg->num_responses = 1;
	msg->response[0] = proc_hot;
}

static void hsmpctld_xgmi_width(struct hsmp_msg *msg)
{
	enum hsmp_xgmi_width min, max;
	int err;

	if (!valid_num_args(msg, 2))
		return;

	min = msg->args[0];
	max = msg->args[1];

	err = hsmp_set_xgmi_width(min, max);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}
}

static void hsmpctld_xgmi_auto(struct hsmp_msg *msg)
{
	int err;

	err = hsmp_set_xgmi_auto();
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}
}

static void hsmpctld_df_pstate(struct hsmp_msg *msg)
{
	enum hsmp_df_pstate pstate;
	int socket;
	int err;

	if (!valid_num_args(msg, 1))
		return;

	socket = msg->args[0];
	pstate = msg->args[1];

	err = hsmp_set_data_fabric_pstate(socket, pstate);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}
}

static void hsmpctld_fabric_clocks(struct hsmp_msg *msg)
{
	int df_clock, mem_clock;
	int socket;
	int err;

	socket = msg->args[0];

	err = hsmp_fabric_clocks(socket, &df_clock, &mem_clock);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
		return;
	}

	msg->num_responses = 2;
	msg->response[0] = df_clock;
	msg->response[1] = mem_clock;
}

static void hsmpctld_core_clock_max(struct hsmp_msg *msg)
{
	u32 frequency;
	int socket;
	int err;

	if (!valid_num_args(msg, 1))
		return;

	socket = msg->args[0];

	err = hsmp_core_clock_max_frequency(socket, &frequency);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}

	msg->num_responses = 1;
	msg->response[0] = frequency;
}

static void hsmpctld_c0_residency(struct hsmp_msg *msg)
{
	u32 residency;
	int socket;
	int err;

	if (!valid_num_args(msg, 1))
		return;

	socket = msg->args[0];

	err = hsmp_c0_residency(socket, &residency);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}

	msg->num_responses = 1;
	msg->response[0] = residency;
}

static void hsmpctld_nbio_pstate(struct hsmp_msg *msg)
{
	enum hsmp_nbio_pstate pstate;
	u8 bus_num;
	int err;

	if (!valid_num_args(msg, 2))
		return;

	pstate = msg->args[0];
	bus_num = msg->args[1];

	err = hsmp_set_nbio_pstate(bus_num, pstate);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}
}

static void hsmpctld_nbio_pstate_all(struct hsmp_msg *msg)
{
	enum hsmp_nbio_pstate pstate;
	u8 bus_num;
	int index;
	int err;

	if (!valid_num_args(msg, 1))
		return;

	pstate = msg->args[0];

	index = 0;
	index = hsmp_next_bus(index, &bus_num);
	while (index > 0) {
		err = hsmp_set_nbio_pstate(bus_num, pstate);
		if (err)
			break;

		index = hsmp_next_bus(index, &bus_num);
	}

	if (index < 0 || err) {
		msg->err = -1;
		msg->errnum = errno;
	}
}

static void hsmpctld_nbio_next_bus(struct hsmp_msg *msg)
{
	u8 bus_num;
	int index;

	if (!valid_num_args(msg, 1))
		return;

	index = msg->args[0];

	errno = 0;
	index = hsmp_next_bus(index, &bus_num);
	if (index < 0) {
		msg->err = -1;
		msg->errnum = errno;
	} else {
		msg->num_responses = 2;
		msg->response[0] = index;
		msg->response[1] = bus_num;
	}
}

static void hsmpctld_ddr_bw(struct hsmp_msg *msg)
{
	u32 max, utilized, percent;
	int socket;
	int err;

	socket = msg->args[0];

	err = hsmp_ddr_bandwidths(socket, &max, &utilized, &percent);
	if (err) {
		msg->err = err;
		msg->errnum = errno;
	}

	msg->num_responses = 3;
	msg->response[0] = max;
	msg->response[1] = utilized;
	msg->response[2] = percent;
}

struct hsmpctld_cmd {
	enum hsmp_msg_t	msg_id;
	void (*cmd)(struct hsmp_msg *msg);
};

struct hsmpctld_cmd hsmpctld_handlers[] = {
	{HSMPCTL_GET_VERSION,			hsmpctld_get_version},
	{HSMPCTL_SOCKET_POWER,			hsmpctld_socket_power},
	{HSMPCTL_SOCKET_POWER_LIMIT,		hsmpctld_socket_power_limit},
	{HSMPCTL_SET_SOCKET_POWER_LIMIT,	hsmpctld_set_socket_power_limit},
	{HSMPCTL_SOCKET_POWER_MAX,		hsmpctld_socket_power_max},
	{HSMPCTL_SET_CPU_BOOST_LIMIT,		hsmpctld_set_cpu_boost_limit},
	{HSMPCTL_SET_SOCKET_BOOST_LIMIT,	hsmpctld_set_socket_boost_limit},
	{HSMPCTL_SET_SYSTEM_BOOST_LIMIT,	hsmpctld_set_system_boost_limit},
	{HSMPCTL_CPU_BOOST_LIMIT,		hsmpctld_cpu_boost_limit},
	{HSMPCTL_PROC_HOT,			hsmpctld_proc_hot},
	{HSMPCTL_XGMI_WIDTH,			hsmpctld_xgmi_width},
	{HSMPCTL_XGMI_AUTO,			hsmpctld_xgmi_auto},
	{HSMPCTL_DF_PSTATE,			hsmpctld_df_pstate},
	{HSMPCTL_FABRIC_CLOCKS,			hsmpctld_fabric_clocks},
	{HSMPCTL_CORE_CLOCK_MAX,		hsmpctld_core_clock_max},
	{HSMPCTL_C0_RESIDENCY,			hsmpctld_c0_residency},
	{HSMPCTL_NBIO_PSTATE,			hsmpctld_nbio_pstate},
	{HSMPCTL_NBIO_PSTATE_ALL,		hsmpctld_nbio_pstate_all},
	{HSMPCTL_NBIO_NEXT_BUS,			hsmpctld_nbio_next_bus},
	{HSMPCTL_DDR_BW,			hsmpctld_ddr_bw},
};

static void handle_request(struct hsmp_msg *msg)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(hsmpctld_handlers); i++) {
		if (hsmpctld_handlers[i].msg_id == msg->msg_id)
			return hsmpctld_handlers[i].cmd(msg);
	}

	/* No valid message found */
	msg->err = -1;
	msg->errnum = EINVAL;
}

int main(int argc, const char **argv)
{
	struct hsmp_msg msg;
	int fd;

	/* Child process/daemon */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	umask(0);
	mkfifo(HSMPCTL_FIFO, 0666);

	while (1) {
		fd = open(HSMPCTL_FIFO, O_RDONLY);
		if (fd < 0)
			continue;

		read(fd, &msg, sizeof(msg));
		close(fd);

		if (msg.msg_id == HSMPCTLD_EXIT)
			break;

		handle_request(&msg);

		fd = open(HSMPCTL_FIFO, O_WRONLY);
		write(fd, &msg, sizeof(msg));
		close(fd);

		sleep(1);
	}

	unlink(HSMPCTL_FIFO);
	return 0;
}

