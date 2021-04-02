// SPDX-License-Identifier: MIT License
/*
 * Copyright (C) 2021 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Nathan Fontenot <nathan.fontenot@amd.com>
 *
 * AMD Host System Management Port command
 */

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "hsmpctl.h"
#include "libhsmp.h"

#define pr_error(...)	fprintf(stderr, "ERROR: " __VA_ARGS__)

#define hsmpctl_version		"0.9"

int chosen_cpu = -1;
int chosen_socket = -1;
int chosen_bus = -1;
int all_system;
int help_opt;
int list_opt;

int system_sockets;
int system_cpus;
int cpu_family;

enum hsmpctl_perms {
	USER	= 1,	/* All users have access */
	ROOT	= 2,	/* Command needs root privileges */
	FUNC	= 3,	/* Command may require root privileges */
};

struct hsmp_cmd {
	const char *cmd_name;
	int (*handler)(int argc, const char **argv);
	void (*help)(void);
	enum hsmpctl_perms perms;
};

struct hsmp_cmd *cmd;

static int parse_value(const char *type, const char *str, int *value)
{
	char *end;
	int val;

	errno = 0;
	val = strtol(str, &end, 0);

	if ((errno == EINVAL && (val == LONG_MAX || val == LONG_MIN)) ||
	    (errno && val == 0)) {
		pr_error("Invalid %s specified, \"%s\"\n", type, str);
		return -1;
	}

	if (end == str) {
		pr_error("No %s found\n", type);
		return -1;
	}

	*value = val;
	return 0;
}

/* verify hsmpctld is running */
static int daemon_is_active(void)
{
	char buf[1024];
	int found;
	char *cmd;
	FILE *fp;

	fp = popen("ps -U root", "r");
	if (!fp)
		return -1;

	found = 0;

	while (fgets(buf, sizeof(buf), fp)) {
		cmd = strstr(buf, "hsmpctld");
		if (cmd) {
			found = 1;
			break;
		}
	}

	pclose(fp);
	return found;
}

static int write_msg(struct hsmp_msg *msg)
{
	ssize_t cnt;
	int fd;

	fd = open(HSMPCTL_FIFO, O_WRONLY);
	if (!fd) {
		pr_error("Could not open pipe to daemon\n%s",
			 strerror(errno));
		return -1;
	}

	cnt = write(fd, msg, sizeof(*msg));
	close(fd);

	if (cnt != sizeof(*msg)) {
		pr_error("Failed to write to daemon\n%s",
			 strerror(errno));
		return -1;
	}

	return 0;
}

static int read_msg(struct hsmp_msg *msg)
{
	ssize_t cnt;
	int fd;

	fd = open(HSMPCTL_FIFO, O_RDONLY);
	if (!fd) {
		pr_error("Could not open pipe to daemon\n%s",
			 strerror(errno));
		return -1;
	}

	cnt = read(fd, msg, sizeof(*msg));
	close(fd);

	if (cnt != sizeof(*msg)) {
		pr_error("Failed to read from daemon\n%s",
			 strerror(errno));
		return -1;
	}

	return 0;
}

static int send_msg(struct hsmp_msg *msg, int expected_responses)
{
	if (write_msg(msg))
		return -1;

	if (read_msg(msg))
		return -1;

	if (msg->err) {
		switch (msg->errnum) {
		case ENOMSG:
		case EBADMSG:
			pr_error("The %s command is not supported.\n", cmd->cmd_name);
			break;
		case ENOTSUP:
			pr_error("HSMP is not supported on this system or has "
				 "been disabled at the BIOS level\n");
			break;
		case ETIMEDOUT:
			pr_error("The hsmpctl command timed out waiting for a "
				 "response from HSMP\n");
			break;
		case EAGAIN:
			pr_error("An error occurred during libhsmp initialization, "
				 "re-trying the command may succeed.\n");
			break;
		case EINVAL:
			pr_error("An invalid parameter was specified\n");
			break;
		default:
			pr_error("An unexpected error occurred;\n%s\n",
				 strerror(msg->errnum));
			break;
		}

		return -1;
	}

	if (expected_responses && msg->num_responses != expected_responses) {
		pr_error("Incorrect responses, returned %d expected %d\n",
			 msg->num_responses, expected_responses);
		return -1;
	}

	return 0;
}

static int get_socket(void)
{
	if (chosen_socket == -1) {
		/* default to socket 0 on single socket systems */
		if (system_sockets == 1)
			return 0;

		pr_error("No socket specified.\n");
	} else {
		if (chosen_socket < system_sockets)
			return chosen_socket;

		pr_error("Invalid socket %d specified\n", chosen_socket);
	}

	return -1;
}

static int get_cpu(void)
{
	if (chosen_cpu == -1) {
		pr_error("No cpu specified\n");
		return -1;
	}

	if (chosen_cpu < 0 || chosen_cpu >= system_cpus) {
		pr_error("Invalid cpu %d specified\n", chosen_cpu);
		return -1;
	}

	return chosen_cpu;
}

static int get_next_bus(int *index, u8 *bus_num)
{
	struct hsmp_msg msg;
	int err;

	memset(&msg, 0, sizeof(msg));
	msg.msg_id = HSMPCTL_NBIO_NEXT_BUS;
	msg.num_args = 1;
	msg.args[0] = *index;

	err = send_msg(&msg, 2);
	if (err)
		return -1;

	*index = msg.response[0];
	*bus_num = msg.response[1];

	return 0;
}

static int get_bus(u8 *bus_num)
{
	u8 next_bus;
	int errno;
	int index;
	int err;

	if (chosen_bus == -1) {
		pr_error("No bus specified");
		return 0;
	}

	index = 0;
	do {
		err = get_next_bus(&index, &next_bus);
		if (err)
			break;

		if (index <= 0)
			break;

		if (next_bus == chosen_bus) {
			*bus_num = next_bus;
			return 1;
		}
	} while (index <= 0);

	/* Valid bus not found */
	pr_error("Invalid bus %d specified\n", chosen_bus);
	return 0;
}

static void help_version(void)
{
	printf("Usage: hsmpctl version\n\n"
	       "Display the SMU firmware and HSMP Interface version\n");
}

static int cmd_version(int argc, const char **argv)
{
	struct hsmp_msg msg;
	int err;

	memset(&msg, 0, sizeof(msg));

	msg.msg_id = HSMPCTL_GET_VERSION;
	err = send_msg(&msg, 4);
	if (err)
		return err;

	printf("SMU FW Version: %d:%d:%d\n", msg.response[0], msg.response[1],
	       msg.response[2]);
	printf("HSMP Interface Version: %d\n", msg.response[3]);

	return 0;
}

static void help_socket_power(void)
{
	printf("Usage: hsmpctl [options] socket_power\n\n"
	       "Display the average socket power consumption in mW.\n\n"
	       "Options:\n"
	       "    -s <socket>     - Display the power consumption for the\n"
	       "                      specified <socket>.\n"
	       "    [-a | --all]    - Display the power consumption for all sockets.\n");
}

static int get_socket_power(int socket)
{
	struct hsmp_msg msg;
	int err;

	memset(&msg, 0, sizeof(msg));

	msg.msg_id = HSMPCTL_SOCKET_POWER;
	msg.num_args = 1;
	msg.args[0] = socket;

	err = send_msg(&msg, 1);
	if (err)
		return err;

	printf("Socket %d: %d mW\n", socket, msg.response[0]);
	return 0;
}

static int cmd_socket_power(int argc, const char **argv)
{
	int socket;
	int i, err;

	if (all_system) {
		for (i = 0; i < system_sockets; i++) {
			err = get_socket_power(i);
			if (err)
				break;
		}
	} else {
		socket = get_socket();
		if (socket == -1) {
			help_socket_power();
			return -1;
		}

		err =  get_socket_power(socket);
	}

	return err;
}

static void help_socket_power_limit(void)
{
	printf("Usage: hsmpctl [options] socket_power_limit <power_limit>\n\n"
	       "Displays the socket power limit (in mW) if no <power_limit> is\n"
	       "specified, otherwise set the specified <power_limit>, must be\n"
	       "root to set the power limit\n\n"
	       "Options:\n"
	       "    -s <socket>     - Display or set the power limit for the\n"
	       "                      specified <socket>.\n"
	       "    [-a | --all]    - Display or set the power limit for all sockets.\n");
}

static int get_socket_power_limit(int socket)
{
	struct hsmp_msg msg;
	int err;

	memset(&msg, 0, sizeof(msg));

	msg.msg_id = HSMPCTL_SOCKET_POWER_LIMIT;
	msg.num_args = 1;
	msg.args[0] = socket;

	err = send_msg(&msg, 1);
	if (err)
		return err;

	printf("Socket %d power limit: %d mW\n", socket, msg.response[0]);
	return 0;
}

static int set_socket_power_limit(int socket, int power_limit)
{
	struct hsmp_msg msg;

	memset(&msg, 0, sizeof(msg));

	msg.msg_id = HSMPCTL_SET_SOCKET_POWER_LIMIT;
	msg.num_args = 2;
	msg.args[0] = socket;
	msg.args[1] = power_limit;

	return send_msg(&msg, 0);
}

static int cmd_socket_power_limit(int argc, const char **argv)
{
	int socket;
	int i, err;

	if (!all_system) {
		socket = get_socket();
		if (socket == -1) {
			help_socket_power_limit();
			return -1;
		}
	}

	if (argc == 1) {
		/* If no socket power limit is specified, report the current
		 * power limit for the specified socket.
		 */
		if (all_system) {
			for (i = 0; i < system_sockets; i++) {
				err = get_socket_power_limit(i);
				if (err)
					return err;
			}
		} else {
			return get_socket_power_limit(socket);
		}
	} else {
		int power_limit;

		if (argc < 2) {
			pr_error("No power limit specified\n");
			help_socket_power_limit();
			return -1;
		}

		err = parse_value("power limit", argv[1], &power_limit);
		if (err) {
			help_socket_power_limit();
			return -1;
		}

		/* Setting socket power limit requires root access */
		if (geteuid() != 0) {
			pr_error("%s\n", strerror(EPERM));
			return -1;
		}

		if (all_system) {
			for (i = 0; i < system_sockets; i++) {
				err = set_socket_power_limit(i, power_limit);
				if (err)
					return err;
			}
		} else {
			return set_socket_power_limit(socket, power_limit);
		}
	}

	return 0;
}

static void help_socket_max_power(void)
{
	printf("Usage: hsmpctl [options] socket_max_power\n\n"
	       "Display the maximum power consumption limit that can be set\n\n"
	       "Options:\n"
	       "    -s <socket>     - Display the maximum power consumption limit\n"
	       "                      for the specified <socket>\n"
	       "    [-a | --all]    - Display the maximum power consumption limit\n"
	       "                      for all sockets.\n");
}

static int get_socket_max_power(int socket)
{
	struct hsmp_msg msg;
	int err;

	memset(&msg, 0, sizeof(msg));

	msg.msg_id = HSMPCTL_SOCKET_POWER_MAX;
	msg.num_args = 1;
	msg.args[0] = socket;

	err = send_msg(&msg, 1);
	if (err)
		return err;

	printf("Socket %d max power limit: %d mW\n", socket, msg.response[0]);
	return 0;
}

static int cmd_socket_max_power(int argc, const char **argv)
{
	int socket;
	int i, err;

	if (all_system) {
		for (i = 0; i < system_sockets; i++) {
			err = get_socket_max_power(i);
			if (err)
				return err;
		}
	} else {
		socket = get_socket();
		if (socket == -1) {
			help_socket_max_power();
			return -1;
		}

		err = get_socket_max_power(socket);
	}

	return err;
}

static void help_boost_limit(void)
{
	printf("Usage: hsmpctl [options] cpu_boost_limit <boost_limit>\n\n"
	       "Display the CPU boost limit (in MHz) or set the boost limit if\n"
	       "a <boost_limit> is specified, must be root to set the\n"
	       "boost limit\n\n"
	       "Options:\n"
	       "    -c <cpu>        - Display or set the boost limit for the\n"
	       "                      specified <cpu>\n"
	       "    -s <socket>     - Set the boost limit for all CPUs in the\n"
	       "                      specified <socket>\n"
	       "    [-a | --all]    - Display or set the boost limit for all CPUs\n");
}

static int get_cpu_boost_limit(int cpu)
{
	struct hsmp_msg msg;
	int err;

	memset(&msg, 0, sizeof(msg));

	msg.msg_id = HSMPCTL_CPU_BOOST_LIMIT;
	msg.num_args = 1;
	msg.args[0] = cpu;

	err = send_msg(&msg, 1);
	if (err)
		return err;

	printf("CPU %d boost limit: %d mW\n", cpu, msg.response[0]);
	return 0;
}

static int show_cpu_boost_limit(void)
{
	int i, err;
	int cpu;

	if (all_system) {
		for (i = 0; i < system_cpus; i++) {
			err = get_cpu_boost_limit(i);
			if (err)
				return err;
		}
	} else {
		cpu = get_cpu();
		if (cpu == -1) {
			help_boost_limit();
			return -1;
		}

		err = get_cpu_boost_limit(cpu);
	}

	return err;
}

static int cmd_boost_limit(int argc, const char **argv)
{
	struct hsmp_msg msg;
	int boost_limit;
	int cpu, socket;
	int err;

	if (argc == 1) {
		/* Report CPU boost limit */
		return show_cpu_boost_limit();
	}

	/* Setting boost limit requires root access */
	if (geteuid() != 0) {
		pr_error("%s\n", strerror(EPERM));
		return -1;
	}

	err = parse_value("boost limit", argv[1], &boost_limit);
	if (err) {
		return -1;
		help_boost_limit();
	}

	memset(&msg, 0, sizeof(msg));

	if (chosen_cpu != -1) {
		cpu = get_cpu();
		if (cpu == -1) {
			help_boost_limit();
			return -1;
		}

		msg.msg_id = HSMPCTL_SET_CPU_BOOST_LIMIT;
		msg.num_args = 2;
		msg.args[0] = chosen_cpu;
		msg.args[1] = boost_limit;
	} else if (chosen_socket != -1) {
		socket = get_socket();
		if (socket == -1) {
			help_boost_limit();
			return -1;
		}

		msg.msg_id = HSMPCTL_SET_SOCKET_BOOST_LIMIT;
		msg.num_args = 2;
		msg.args[0] = socket;
		msg.args[1] = boost_limit;
	} else if (all_system) {
		msg.msg_id = HSMPCTL_SET_SYSTEM_BOOST_LIMIT;
		msg.num_args = 1;
		msg.args[0] = boost_limit;
	} else {
		pr_error("No cpu, socket, or entire system specified\n");
		help_boost_limit();
		return -1;
	}

	return send_msg(&msg, 0);
}

static void help_proc_hot(void)
{
	printf("Usage: hsmpctl [options] proc_hot\n\n"
	       "Display the PROC HOT status.\n\n"
	       "Options:\n"
	       "    -s <socket>     - Display PROC HOT for the specified <socket>\n"
	       "    [-a | --all]    - Display PROC HOT status for all sockets\n");
}

static int get_proc_hot(int socket)
{
	struct hsmp_msg msg;
	int err;

	memset(&msg, 0, sizeof(msg));
	msg.msg_id = HSMPCTL_PROC_HOT;
	msg.num_args = 1;
	msg.args[0] = socket;

	err = send_msg(&msg, 1);
	if (err)
		return err;

	printf("Socket %d PROC HOT %s asserted\n", socket,
	       msg.response[0] ? "" : "not");
	return 0;
}

static int cmd_proc_hot(int argc, const char **argv)
{
	int socket;
	int i, err;

	if (all_system) {
		for (i = 0; i < system_sockets; i++) {
			err = get_proc_hot(i);
			if (err)
				break;
		}
	} else {
		socket = get_socket();
		if (socket == -1) {
			help_proc_hot();
			return -1;
		}

		err = get_proc_hot(socket);
	}

	return err;
}

static void help_xgmi_width(void)
{
	printf("Usage: hsmpctl xgmi_width [auto | <min> <max>]\n\n"
	       "Set the xGMI link width control to Dynamic Link Width Management.\n"
	       "if \'auto\' is scpecified, or set the xGMI link width control to\n"
	       "the specified <min> and <max> values. To set a fixed link width,\n"
	       "specifiy width values such that <min> = <max>. Must be run as root\n\n");
	printf("Valid xGMI link widths:\n"
	       "    x2              - 2 lanes\n"
	       "    x8              - 8 lanes\n"
	       "    x16             - 16 lanes\n");
}

static enum hsmp_xgmi_width parse_xgmi_width(const char *arg)
{
	if (!strcmp(arg, "x2"))
		return HSMP_XGMI_WIDTH_X2;

	if (!strcmp(arg, "x8"))
		return HSMP_XGMI_WIDTH_X8;

	if (!strcmp(arg, "x16"))
		return HSMP_XGMI_WIDTH_X16;

	pr_error("Invalid xGMI width \"%s\" specified\n", arg);
	help_xgmi_width();
	return -1;
}

static int cmd_xgmi_width(int argc, const char **argv)
{
	enum hsmp_xgmi_width min, max;
	struct hsmp_msg msg;

	if (argc < 2) {
		pr_error("No xGMI width setting specified\n");
		return -1;
	}

	if (argc == 2) {
		/* If only one width provided it should be 'auto' */
		if (strcmp(argv[1], "auto")) {
			pr_error("The provided width \'%s\' is not valid\n", argv[1]);
			help_xgmi_width();
			return -1;
		}

		memset(&msg, 0, sizeof(msg));
		msg.msg_id = HSMPCTL_XGMI_AUTO;
		msg.num_args = 0;
	} else {
		/* There should be a min and max width provided. */
		min = parse_xgmi_width(argv[1]);
		if (min != -1)
			max = parse_xgmi_width(argv[2]);

		if (min == -1 || max == -1)
			return -1;

		memset(&msg, 0, sizeof(msg));
		msg.msg_id = HSMPCTL_XGMI_WIDTH;
		msg.num_args = 2;
		msg.args[0] = min;
		msg.args[1] = max;
	}

	return send_msg(&msg, 0);
}

static void help_df_pstate(void)
{
	printf("Usage: [options] hsmpctl df_pstate <pstate>\n\n"
	       "Set the data fabric P-state to the specified <pstate>, must\n"
	       "be run as root\n\n"
	       "Options:\n"
	       "    -s <socket>     - Set data fabric <pstate> for the specified socket\n"
	       "    [-a | --all]    - Set data fabric <pstate> for all sockets\n\n");
	printf("Valid P-states:\n"
	       "    auto            - Enable automatic p_state selection.\n"
	       "    0               - Highest P-state.\n"
	       "    1                 .\n"
	       "    2                 .\n"
	       "    3               - Lowest P-state.\n");
}

static int set_df_pstate(int socket, enum hsmp_df_pstate pstate)
{
	struct hsmp_msg msg;

	memset(&msg, 0, sizeof(msg));
	msg.msg_id = HSMPCTL_DF_PSTATE;
	msg.num_args = 1;
	msg.args[0] = socket;
	msg.args[1] = pstate;

	return send_msg(&msg, 0);
}

static int cmd_df_pstate(int argc, const char **argv)
{
	enum hsmp_df_pstate pstate;
	int socket;
	int err;

	if (argc < 2) {
		pr_error("No data fabric P-state specified\n");
		help_df_pstate();
		return -1;
	}

	if (!strcmp(argv[1], "auto")) {
		pstate = HSMP_DF_PSTATE_AUTO;
	} else if (!strcmp(argv[1], "0")) {
		pstate = HSMP_DF_PSTATE_0;
	} else if (!strcmp(argv[1], "1")) {
		pstate = HSMP_DF_PSTATE_1;
	} else if (!strcmp(argv[1], "2")) {
		pstate = HSMP_DF_PSTATE_2;
	} else if (!strcmp(argv[1], "3")) {
		pstate = HSMP_DF_PSTATE_3;
	} else {
		pr_error("Invalid data fabric P-state \"%s\" specified\n", argv[1]);
		help_df_pstate();
		return -1;
	}

	if (all_system) {
		for (socket = 0; socket < system_sockets; socket++) {
			err = set_df_pstate(socket, pstate);
			if (err)
				break;
		}
	} else {
		socket = get_socket();
		if (socket == -1) {
			help_df_pstate();
			return -1;
		}

		err = set_df_pstate(socket, pstate);
	}

	return err;
}

static void help_fabric_clocks(void)
{
	printf("Usage: hsmpctl [options] fabric_clocks\n\n"
	       "Display the Data Fabric and Memory clocks (in MHz).\n\n"
	       "Options:\n"
	       "    -s <socket>     - Display clocks for the specified <socket>\n"
	       "    [-a | --all]    - Display clocks for all sockets\n");
}

static int get_fabric_clocks(int socket)
{
	struct hsmp_msg msg;
	int err;

	memset(&msg, 0, sizeof(msg));
	msg.msg_id = HSMPCTL_FABRIC_CLOCKS;
	msg.args[0] = socket;

	err = send_msg(&msg, 2);
	if (err)
		return err;

	printf("Socket %d data fabric clock: %d MHz\n", socket, msg.response[0]);
	printf("Socket %d memory clock: %d MHz\n", socket, msg.response[1]);
	return 0;
}

static int cmd_fabric_clocks(int argc, const char **argv)
{
	int socket;
	int err;

	if (all_system) {
		for (socket = 0; socket < system_sockets; socket++) {
			err = get_fabric_clocks(socket);
			if (err)
				break;
		}
	} else {
		socket = get_socket();
		if (socket == -1) {
			help_fabric_clocks();
			return -1;
		}

		err = get_fabric_clocks(socket);
	}

	return err;
}

static void help_core_clock_max(void)
{
	printf("Usage: hsmpctl [options] core_clock_max\n\n"
	       "Display the maximum core clock (in MHz).\n\n"
	       "Options:\n"
	       "    -s <socket>     - Display clock for the specified <socket>\n"
	       "    [-a | --all]    - Display clock for all sockets\n");
}

static int get_core_clock_max(int socket)
{
	struct hsmp_msg msg;
	int err;

	memset(&msg, 0, sizeof(msg));
	msg.msg_id = HSMPCTL_CORE_CLOCK_MAX;
	msg.num_args = 1;
	msg.args[0] = socket;

	err = send_msg(&msg, 1);
	if (err)
		return err;

	printf("Socket %d core clock max frequency: %d MHz\n", socket,
	       msg.response[0]);
	return 0;
}

static int cmd_core_clock_max(int argc, const char **argv)
{
	int socket;
	int err;

	if (all_system) {
		for (socket = 0; socket < system_sockets; socket++) {
			err = get_core_clock_max(socket);
			if (err)
				break;
		}
	} else {
		socket = get_socket();
		if (socket == -1) {
			help_core_clock_max();
			return -1;
		}

		err = get_core_clock_max(socket);
	}

	return err;
}

static void help_c0_residency(void)
{
	printf("Usage: hsmpctl [options] c0_residency\n\n"
	       "Display C0 Residency as an integer between 0 - 100, where 100 specifies\n"
	       "that all enabled cpus in the socket are running in C0.\n\n"
	       "Options:\n"
	       "    -s <socket>     - Display C0 Residency for the specified <socket>\n"
	       "    [-a | --all]    - Display C0 Residency for all sockets\n");
}

static int get_c0_residency(int socket)
{
	struct hsmp_msg msg;
	int err;

	memset(&msg, 0, sizeof(msg));
	msg.msg_id = HSMPCTL_C0_RESIDENCY;
	msg.num_args = 1;
	msg.args[0] = socket;

	err = send_msg(&msg, 1);
	if (err)
		return err;

	printf("Socket %d C0 Residency: %d\n", socket, msg.response[0]);
	return 0;
}

static int cmd_c0_residency(int argc, const char **argv)
{
	int socket;
	int err;

	if (all_system) {
		for (socket = 0; socket < system_sockets; socket++) {
			err = get_c0_residency(socket);
			if (err)
				break;
		}
	} else {
		socket = get_socket();
		if (socket == -1) {
			help_c0_residency();
			return -1;
		}

		err = get_c0_residency(socket);
	}

	return err;
}

static void help_nbio_pstate(void)
{
	printf("Usage: hsmpctl [options] nbio_pstate <pstate>\n\n"
	       "Set the NBIO P-state to the specified <pstate>, must be run as root.\n\n"
	       "Options:\n"
	       "    -b <bus_num>    - Set <pstate> for the specified <bus_num>\n"
	       "    [-a | --all]    - Set <pstate> for all busses\n\n"
	       "Valid P-states:\n"
	       "    auto            - Enable automatic P-state selection\n"
	       "    0               - Highest NBIO P-state\n");
}

static int cmd_nbio_pstate(int argc, const char **argv)
{
	enum hsmp_nbio_pstate pstate;
	struct hsmp_msg msg;
	int valid_bus;
	u8 bus_num;

	if (argc < 2) {
		pr_error("No NBIO P-state specified\n");
		help_nbio_pstate();
		return -1;
	}

	if (!strcmp(argv[1], "auto")) {
		pstate = HSMP_NBIO_PSTATE_AUTO;
	} else if (!strcmp(argv[1], "0")) {
		pstate = HSMP_NBIO_PSTATE_P0;
	} else {
		pr_error("Invalid NBIO P-state \"%s\" specified\n", argv[1]);
		help_nbio_pstate();
		return -1;
	}

	memset(&msg, 0, sizeof(msg));
	msg.args[0] = pstate;

	if (all_system) {
		msg.msg_id = HSMPCTL_NBIO_PSTATE_ALL;
		msg.num_args = 1;
	} else {
		valid_bus = get_bus(&bus_num);
		if (!valid_bus) {
			help_nbio_pstate();
			return -1;
		}

		msg.msg_id = HSMPCTL_NBIO_PSTATE;
		msg.num_args = 2;
		msg.args[1] = bus_num;
	}

	return send_msg(&msg, 0);
}

static void help_ddr_bw(void)
{
	printf("Usage: hsmpctl [options] ddr_bw\n\n"
	       "Display DDR theoretical maximum bandwidth (in GB/s), the utilized\n"
	       "bandwidth (in GB/s), and the bandwidth as a percentage of the\n"
	       "theoretical maximum.\n\n"
	       "Options:\n"
	       "    -s <socket>     - Display bandwidth for the specified <socket>\n"
	       "    [-a | --all]    - Display bandwidth for all sockets\n");
}

static int get_ddr_bw(int socket)
{
	struct hsmp_msg msg;
	int err;

	memset(&msg, 0, sizeof(msg));
	msg.msg_id = HSMPCTL_DDR_BW;
	msg.args[0] = socket;

	err = send_msg(&msg, 3);
	if (err)
		return err;

	printf("Socket %d DDR max bandwidth: %d GB/s\n", socket, msg.response[0]);
	printf("Socket %d DDR utilized bandwidth: %d GB/s (%d%%)\n", socket,
	       msg.response[1], msg.response[2]);
	return 0;
}

static int cmd_ddr_bw(int argc, const char **argv)
{
	int socket;
	int err;

	if (all_system) {
		for (socket = 0; socket < system_sockets; socket++) {
			err = get_ddr_bw(socket);
			if (err)
				break;
		}
	} else {
		socket = get_socket();
		if (socket == -1) {
			help_ddr_bw();
			return -1;
		}

		err = get_ddr_bw(socket);
	}

	return err;
}

static void help_stop_daemon(void)
{
	printf("Usage: hsmpctl stop\n\n"
	       "Stop the hsmpctld daemon, must be run as root.\n");
}

static int stop_daemon(int argc, const char **argv)
{
	struct hsmp_msg msg;

	if (!daemon_is_active())
		return 0;

	memset(&msg, 0, sizeof(msg));
	msg.msg_id = HSMPCTLD_EXIT;

	return write_msg(&msg);
}

#define HSMPCTLD_CMD "/usr/local/sbin/hsmpctld"

static void help_start_daemon(void)
{
	printf("Usage: hsmpctl start\n\n"
	       "Start the hsmpctld daemon, must be run as root.\n");
}

int start_daemon(int argc, const char **argv)
{
	const char *cmd;
	pid_t pid;
	int err;

	cmd = HSMPCTLD_CMD;

	if (daemon_is_active()) {
		printf("hsmpctld is already active\n");
		return 0;
	}

	pid = fork();
	if (pid < 0) {
		pr_error("failed to start hsmpctld daemon\n");
		return -1;
	} else if (pid > 0) {
		/* parent */
		return 0;
	}

	err = execlp(cmd, cmd,  NULL);
	if (err)
		pr_error("%s\n", strerror(errno));

	return err;
}

#define LSCPU_SOCKETS	"Socket(s):"
#define LSCPU_CORES	"Core(s) per socket:"
#define CPU_FAMILY	"CPU family:"

void get_system_info(void)
{
	char buf[1024];
	FILE *fp;

	system_sockets = -1;
	system_cpus = -1;

	fp = popen("lscpu", "r");
	if (!fp)
		return;

	while (fgets(buf, sizeof(buf), fp)) {
		if (!strncmp(buf, LSCPU_SOCKETS, strlen(LSCPU_SOCKETS)))
			system_sockets = strtol(buf + strlen(LSCPU_SOCKETS), NULL, 0);

		if (!strncmp(buf, LSCPU_CORES, strlen(LSCPU_CORES)))
			system_cpus = strtol(buf + strlen(LSCPU_CORES), NULL, 0);

		if (!strncmp(buf, CPU_FAMILY, strlen(CPU_FAMILY)))
			cpu_family = strtol(buf + strlen(CPU_FAMILY), NULL, 0);
	}

	system_cpus *= system_sockets;

	pclose(fp);
}

static struct hsmp_cmd hsmp_commands[] = {
	{"version",		cmd_version,		help_version,			USER},
	{"socket_power",	cmd_socket_power,	help_socket_power,		USER},
	{"socket_power_limit",	cmd_socket_power_limit,	help_socket_power_limit,	FUNC},
	{"socket_max_power",	cmd_socket_max_power,	help_socket_max_power,		USER},
	{"cpu_boost_limit",	cmd_boost_limit,	help_boost_limit,		FUNC},
	{"proc_hot",		cmd_proc_hot,		help_proc_hot,			USER},
	{"xgmi_width",		cmd_xgmi_width,		help_xgmi_width,		ROOT},
	{"df_pstate",		cmd_df_pstate,		help_df_pstate,			ROOT},
	{"fabric_clocks",	cmd_fabric_clocks,	help_fabric_clocks,		USER},
	{"core_clock_max",	cmd_core_clock_max,	help_core_clock_max,		USER},
	{"c0_residency",	cmd_c0_residency,	help_c0_residency,		USER},
	{"nbio_pstate",		cmd_nbio_pstate,	help_nbio_pstate,		ROOT},
	{"ddr_bw",		cmd_ddr_bw,		help_ddr_bw,			USER},
	{"start",		start_daemon,		help_start_daemon,		ROOT},
	{"stop",		stop_daemon,		help_stop_daemon,		ROOT},
};

static void list_resources(void)
{
	int index;
	int err;
	u8 bus;

	printf("CPUs: 0 - %d\n", system_cpus - 1);
	printf("Sockets: 0 - %d\n", system_sockets - 1);
	printf("Buses: ");

	index = 0;
	do {
		err = get_next_bus(&index, &bus);
		if (err)
			break;

		if (index <= 0)
			break;

		printf("%d ", bus);
	} while (index <= 0);

	printf("\n");
}

static void usage(void)
{
	int i;

	printf("Usage: hsmpctl [options] command [args]\n\n");
	printf("Options: availability depends on command.\n");
	printf("    [-h | --help]             - Display this message.\n");
	printf("    [-s | --socket] <socket>  - Specify socket for command.\n");
	printf("    [-c | --cpu] <cpu>        - Specify cpu for command\n");
	printf("    [-b | --bus] <bus>        - Specify bus for command\n");
	printf("    [-a | --all]              - Perform command for all sockets/cpus.\n");
	printf("    [-l | --list]             - List available CPUs, sockets, and buses\n");
	printf("    [-v]                      - Print hsmpctl command version\n");
	printf("\nUse hsmpctl [-h | --help] <command> for detailed help.\n");

	printf("\nAvailable commands:\n");
	for (i = 0; i < ARRAY_SIZE(hsmp_commands); i++)
		printf("    %s\n", hsmp_commands[i].cmd_name);
}

static void parse_options(int *argc, const char ***argv)
{
	const char *specifier_opt;
	int new_argc = 0;
	int i, err;

	if (*argc < 1)
		return;

	specifier_opt = NULL;

	for (i = 0; i < *argc && ((*argv)[i])[0] == '-'; i++) {
		const char *opt = (*argv)[i];

		/* There are no valid option combinations */
		if (specifier_opt) {
			pr_error("hsmpctl: %s %s : incompatible options\n",
				 opt, specifier_opt);
			usage();
			exit(-1);
		}

		specifier_opt = opt;

		if (!strcmp(opt, "-h") || !strcmp(opt, "--help")) {
			help_opt = 1;
			new_argc++;
		} else if (!strcmp(opt, "-l") || !strcmp(opt, "--list")) {
			list_opt = 1;
		} else if (!strcmp(opt, "-c") || !strcmp(opt, "--cpu")) {
			if (*argc < 2) {
				usage();
				exit(-1);
			}

			opt = (*argv)[++i];
			err = parse_value("cpu", opt, &chosen_cpu);
			if (err)
				exit(-1);

			/* Cut out -c X */
			new_argc += 2;
		} else if (!strcmp(opt, "-s") || !strcmp(opt, "--socket")) {
			if (*argc < 2) {
				usage();
				exit(-1);
			}

			opt = (*argv)[++i];
			err = parse_value("socket", opt, &chosen_socket);
			if (err)
				exit(-1);

			/* Cut out -s X */
			new_argc += 2;
		} else if (!strcmp(opt, "-b") || !strcmp(opt, "--bus")) {
			if (*argc < 2) {
				usage();
				exit(-1);
			}

			opt = (*argv)[++i];
			err = parse_value("bus", opt, &chosen_bus);
			if (err)
				exit(-1);

			/* Cut out -b X */
			new_argc += 2;
		} else if (!strcmp(opt, "-a") || !strcmp(opt, "--all")) {
			all_system = 1;
			new_argc += 1;
		} else if (!strcmp(opt, "-v")) {
			printf("hsmpctl version %s\n", hsmpctl_version);
			exit(0);
		} else {
			pr_error("invalid option %s specified\n", opt);
			usage();
			exit(-1);
		}
	}

	*argc -= new_argc;
	*argv += new_argc;
}

int main(int argc, const char *argv[])
{
	int i;

	get_system_info();

	if (cpu_family == 0x17)
		printf("WARNING: hsmpctl not supported on AMD Family 0x17 CPUs\n");

	/* Skip past argc/argv command name */
	argc--;
	argv += 1;

	parse_options(&argc, &argv);

	if (argc < 1) {
		usage();
		exit(-1);
	}

	if (help_opt) {
		cmd->help();
		return 0;
	}

	if (list_opt) {
		list_resources();
		return 0;
	}

	cmd = NULL;
	for (i = 0; i < ARRAY_SIZE(hsmp_commands); i++) {
		if (!strcmp(hsmp_commands[i].cmd_name, argv[0])) {
			cmd = &hsmp_commands[i];
			break;
		}
	}

	if (!cmd) {
		printf("Command %s not found\n", argv[0]);
		usage();
		exit(-1);
	}

	/*
	 * No need to do a daemon check when starting or stopping hsmpctld,
	 * this is handeld in the start/stop routines.
	 */
	if (strcmp(cmd->cmd_name, "start") && strcmp(cmd->cmd_name, "stop")) {
		if (!daemon_is_active()) {
			pr_error("The hsmpctld daemon must be started prior to using the "
				 "hsmpctl command.\n\n");
			help_start_daemon();
			exit(-1);
		}
	}

	if (cmd->perms == ROOT) {
		if (geteuid() != 0) {
			pr_error("Root permissions required\n");
			return -1;
		}
	}

	return cmd->handler(argc, argv);
}
