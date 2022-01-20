// SPDX-License-Identifier: MIT License
/*
 * Copyright (C) 2020 Advanced Micro Devices, Inc. - All Rights Reserved
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
#include <time.h>
#include <cpuid.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <pci/pci.h>
#include <pci/types.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "libhsmp.h"
#include "smn.h"
#include "nbio.h"

#ifdef DEBUG_HSMP
#define pr_debug(...)   printf("[libhsmp] " __VA_ARGS__)
#else
#define pr_debug(...)   ((void)0)
#endif

#ifdef DEBUG_HSMP_PCI
#define pr_debug_pci(...)	printf("[libhsmp] " __VA_ARGS__)
#else
#define pr_debug_pci(...)	((void)0)
#endif

#define HSMP_MSG_REG	0x3B10534
#define HSMP_STATUS_REG	0x3B10980
#define HSMP_DATA_REG	0x3B109E0
#define HSMP_TIMEOUT	500

/*
 * Message types
 *
 * All implementations are required to support HSMP_TEST, HSMP_GET_SMU_VER,
 * and HSMP_GET_PROTO_VER. All other messages are implementation dependent.
 */
enum hsmp_msg_t {
	HSMP_TEST				=  1,
	HSMP_GET_SMU_VER			=  2,
	HSMP_GET_PROTO_VER			=  3,
	HSMP_GET_SOCKET_POWER			=  4,
	HSMP_SET_SOCKET_POWER_LIMIT		=  5,
	HSMP_GET_SOCKET_POWER_LIMIT		=  6,
	HSMP_GET_SOCKET_POWER_LIMIT_MAX		=  7,
	HSMP_SET_BOOST_LIMIT			=  8,
	HSMP_SET_BOOST_LIMIT_SOCKET		=  9,
	HSMP_GET_BOOST_LIMIT			= 10,
	HSMP_GET_PROC_HOT			= 11,
	HSMP_SET_XGMI_LINK_WIDTH		= 12,
	HSMP_SET_DF_PSTATE			= 13,
	HSMP_AUTO_DF_PSTATE			= 14,
	HSMP_GET_FCLK_MCLK			= 15,
	HSMP_GET_CCLK_THROTTLE_LIMIT		= 16,
	HSMP_GET_C0_PERCENT			= 17,
	HSMP_SET_NBIO_DPM_LEVEL			= 18,
	HSMP_GET_DDR_BANDWIDTH			= 20,
};

/*
 * Taken from the PPR, the intf_support table describes the
 * highest HSMP interface supported for each HSMP interface
 * version. This is used to validate interface support in
 * the library.
 *
 * NOTE: This should be updated when support for new HSMP
 * interface versions is added.
 */
#define SMN_INTF_SUPPORTED	3

static enum hsmp_msg_t intf_support[] = {
	0,
	HSMP_GET_C0_PERCENT,		/* Interface Version 1 */
	HSMP_SET_NBIO_DPM_LEVEL,	/* Interface Version 2 */
	HSMP_GET_DDR_BANDWIDTH,		/* Interface Version 3 */
};

struct hsmp_message {
	enum hsmp_msg_t	msg_num;	/* Message number */
	uint16_t	num_args;	/* Number of arguments in message */
	uint16_t	response_sz;	/* Number of expected response words */
	uint32_t	args[8];	/* Argument(s) */
	uint32_t	response[8];	/* Response word(s) */
};

union smu_fw_ver {
	struct smu_fw_version ver;
	uint32_t raw_u32;
};

struct cpu_dev {
	int	valid;
	int	socket_id;
	int	apicid;
};

#define MAX_SOCKETS	2
#define MAX_NBIOS	8
#define MAX_CPUS	256

static struct {
	struct cpu_dev		cpus[MAX_CPUS];
	union smu_fw_ver	smu_firmware;		/* SMU firmware version code */
	unsigned int		smu_intf_ver;		/* HSMP implementation level */
	unsigned int		supported_intf;		/* SMU interface version supported */
	unsigned int		x86_family;		/* Family number */
	int			initialized;
	int			lock_fd;
	int			hsmp_disabled;
} hsmp_data;

/* HSMP Status codes used internally */
#define HSMP_STATUS_NOT_READY   0x00
#define HSMP_STATUS_OK          0x01

/*
 * Wrapper for str_error when using the libhsmp interfaces.
 * This allows us to return a valid error string for hardware
 * HSMP defined error codes or a generic errno value.
 *
 * Valid HSMP_ERR_ values are defined in libshmp.h
 */
char *hsmp_strerror(int err, int errno_val)
{
	char *err_string;

	if (err == -1)
		return strerror(errno_val);

	switch (err) {
	case 0:
		err_string =  "Success";
		break;
	case HSMP_ERR_INVALID_MSG_ID:
		err_string =  "Invalid HSMP message ID";
		break;
	case HSMP_ERR_INVALID_ARG:
		err_string =  "Invalid HSMP argument";
		break;
	default:
		err_string = "Unknown error";
		break;
	}

	return err_string;
}

/* Determine if the specified HSMP message ID is supported
 * based on the HSMP interface version.
 *
 * NOTE: This routine assumes hsmp_init() has been called
 * successfully.
 */
static bool msg_id_supported(enum hsmp_msg_t msg_id)
{
	return msg_id <= intf_support[hsmp_data.supported_intf];
}

#define HSMP_LOCK_FILE	"/var/lock/hsmp"

static int hsmp_lock(void)
{
	int err;

	hsmp_data.lock_fd = open(HSMP_LOCK_FILE, O_CREAT, S_IROTH | S_IWOTH);
	if (hsmp_data.lock_fd == -1)
		return -1;

	err = flock(hsmp_data.lock_fd, LOCK_EX);
	if (err) {
		/* Should we re-try? */
		close(hsmp_data.lock_fd);
		return -1;
	}

	return 0;
}

static void hsmp_unlock(void)
{
	flock(hsmp_data.lock_fd, LOCK_UN);
	close(hsmp_data.lock_fd);
}

/*
 * Send a message to the SMN access port via PCI-e config space registers.
 * The caller is expected to zero out any unused arguments. If a response
 * is expected, the number of response words should be greater than 0.
 * Returns 0 for success and populates the requested number of arguments
 * in the passed struct. Returns a negative error code for failure.
 */
static int _hsmp_send_message(struct pci_dev *root_dev, struct hsmp_message *msg)
{
	struct timespec one_ms = { 0, 1000 * 1000 };
	unsigned int arg_num = 0;
	int err, timeout;
	uint32_t mbox_status;

	/* Zero the status register */
	mbox_status = HSMP_STATUS_NOT_READY;
	err = hsmp_write(root_dev, HSMP_STATUS_REG, mbox_status);
	if (err) {
		pr_debug("Error %d clearing HSMP mailbox status register\n", err);
		return err;
	}

	/* Write any message arguments */
	for (arg_num = 0; arg_num < msg->num_args; arg_num++) {
		err = hsmp_write(root_dev, HSMP_DATA_REG + (arg_num << 2),
				 msg->args[arg_num]);
		if (err) {
			pr_debug("Error %d writing HSMP message argument %d\n",
				 err, arg_num);
			return err;
		}
	}

	/* Write the message ID which starts the operation */
	err = hsmp_write(root_dev, HSMP_MSG_REG, msg->msg_num);
	if (err) {
		pr_debug("Error %d writing HSMP message ID %u\n", err, msg->msg_num);
		return err;
	}

	timeout = HSMP_TIMEOUT;

	/*
	 * Assume it takes at least one SMN FW cycle (1 MS) to complete
	 * the operation. Some operations might complete in two, some in
	 * more. So first thing we do is yield the CPU.
	 */
retry:
	nanosleep(&one_ms, NULL);
	err = hsmp_read(root_dev, HSMP_STATUS_REG, &mbox_status);
	if (err) {
		pr_debug("HSMP message ID %u - error %d reading mailbox status\n",
			 err, msg->msg_num);
		return err;
	}

	/* SMN has not responded to the message yet. */
	if (mbox_status == HSMP_STATUS_NOT_READY) {
		if (--timeout == 0) {
			pr_debug("SMN timeout for message ID %u\n", msg->msg_num);
			errno = ETIMEDOUT;
			return -1;
		}

		goto retry;
	}

	/*
	 * Some platforms may not support every HSMP interface covered
	 * by the HSMP interface version specified. For these instances
	 * a status code of HSMP_ERR_INVALID_MSG_ID is returned. If we
	 * see this status and the msg_id is considered supported by the
	 * interface version we return EBADMSG.
	 */
	if (mbox_status == HSMP_ERR_INVALID_MSG_ID &&
	    msg_id_supported(msg->msg_num)) {
		errno = EBADMSG;
		return -1;
	}

	if (mbox_status != HSMP_STATUS_OK)
		return mbox_status;

	/* SMN has responded OK. Read response data */
	for (arg_num = 0; arg_num < msg->response_sz; arg_num++) {
		err = hsmp_read(root_dev, HSMP_DATA_REG + (arg_num << 2),
				&msg->response[arg_num]);
		if (err) {
			pr_debug("Error %d reading HSMP response %u for message ID %u\n",
				 err, arg_num, msg->msg_num);
			return err;
		}
	}

	return 0;
}

static int hsmp_send_message(int socket_id, struct hsmp_message *msg)
{
	struct nbio_dev *nbio;
	int err;
#ifdef DEBUG_HSMP
	unsigned int arg_num = 0;
#endif

	nbio = socket_id_to_nbio(socket_id);
	if (!nbio) {
		errno = EINVAL;
		return -1;
	}

	err = hsmp_lock();
	if (err)
		return -1;

#ifdef DEBUG_HSMP
	pr_debug("Sending message ID %d to socket %d\n", msg->msg_num, socket_id);
	while (msg->num_args && arg_num < msg->num_args) {
		pr_debug("    arg[%d] 0x%08X\n", arg_num, msg->args[arg_num]);
			 arg_num++;
	}
#endif

	err = _hsmp_send_message(nbio->dev, msg);
	hsmp_unlock();

	return err;
}

static int cpu_apicid(int cpu)
{
	if (cpu > MAX_CPUS) {
		errno = EINVAL;
		return -1;
	}

	if (!hsmp_data.cpus[cpu].valid) {
		errno = EINVAL;
		return -1;
	}

	return hsmp_data.cpus[cpu].apicid;
}

static int cpu_socket_id(int cpu)
{
	if (cpu > MAX_CPUS) {
		errno = EINVAL;
		return -1;
	}

	if (!hsmp_data.cpus[cpu].valid) {
		errno = EINVAL;
		return -1;
	}

	return hsmp_data.cpus[cpu].socket_id;
}

/*
 * Probe HSMP mailboxes to verify HSMP is enabled, if successful retrieve
 * the SMU fw version and HSMP interface version.
 *
 * Returns -1 with errno set to ENOTSUP if the test message fails, or
 * errno set to EAGAIN if retrieving the SMU fw version or interface
 * version fails.
 */
static int hsmp_probe(void)
{
	struct hsmp_message msg = { 0 };
	int socket_found;
	int socket_id;
	int err;

	/*
	 * Check each socket to be safe. The test message takes one argument and
	 * returns the value of that argument + 1. The protocol version and SMU
	 * version messages take no arguments and return one.
	 */
	socket_found = 0;
	for (socket_id = 0; socket_id < MAX_SOCKETS; socket_id++) {
		if (!socket_id_to_nbio(socket_id))
			break;

		msg.msg_num = HSMP_TEST;
		msg.num_args = 1;
		msg.args[0] = 1;
		msg.response_sz = 1;

		/*
		 * Send a test message to verify HSMP enablement. If this call
		 * fails the issue is due to HSMP not being enabled in BIOS.
		 *
		 * Set the disabled flag to short-circuit future library calls
		 * from going through library initialization.
		 */
		err = hsmp_send_message(socket_id, &msg);
		if (err) {
			pr_debug("HSMP Test failed for socket %d\n", socket_id);
			hsmp_data.hsmp_disabled = 1;
			errno = ENOTSUP;
			return -1;
		}

		if (msg.response[0] != msg.args[0] + 1) {
			pr_debug("HSMP test failed for socket %d, expected %x, received %x\n",
				 socket_id, msg.args[0] + 1, msg.response[0]);
			hsmp_data.hsmp_disabled = 1;
			errno = ENOTSUP;
			return -1;
		}

		if (!socket_found) {
			/* Retrieve SMU FW Version */
			msg.msg_num = HSMP_GET_SMU_VER;
			msg.num_args = 0;
			msg.args[0] = 0xDEADBEEF;
			msg.response_sz = 1;

			err = hsmp_send_message(socket_id, &msg);
			if (err) {
				pr_debug("Could not retrieve SMU FW Version\n");
				errno = EAGAIN;
				return -1;
			}

			hsmp_data.smu_firmware.raw_u32 = msg.response[0];

			pr_debug("SMU FW Version %u:%u:%u\n",
				 hsmp_data.smu_firmware.ver.major,
				 hsmp_data.smu_firmware.ver.minor,
				 hsmp_data.smu_firmware.ver.debug);

			/* Retrieve HSMP Interface Version */
			msg.msg_num = HSMP_GET_PROTO_VER;
			msg.num_args = 0;
			msg.args[0] = 0xDEADBEEF;
			msg.response_sz = 1;

			err = hsmp_send_message(socket_id, &msg);
			if (err) {
				pr_debug("Could not retrieve HSMP Interface Version\n");
				errno = EAGAIN;
				return -1;
			}

			hsmp_data.smu_intf_ver = msg.response[0];
			pr_debug("Interface Version: %d\n", hsmp_data.smu_intf_ver);

			/*
			 * If the SMU interface version is lower than what libhsmp
			 * supports, reduce the supported interface version to match
			 * the SMU interface version.
			 */
			if (hsmp_data.smu_intf_ver < hsmp_data.supported_intf)
				hsmp_data.supported_intf = hsmp_data.smu_intf_ver;

			socket_found = 1;
		}
	}

	return 0;
}

static int get_system_info(void)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned int family, model;
	int id = 0;
	char vendstr[3][8] __attribute__ ((unused)) = {"unknown", "Intel", "AMD"};

	__cpuid(0, eax, ebx, ecx, edx);
	if (ebx == 0x756e6547 && ecx == 0x6c65746e && edx == 0x49656e69)
		id = 1;
	else if (ebx == 0x68747541 && ecx == 0x444d4163 && edx == 0x69746e65)
		id = 2;

	__cpuid(1, eax, ebx, ecx, edx);
	family = (eax >> 8) & 0xf;
	model = (eax >> 4) & 0xf;

	if (family == 0xf)
		family += (eax >> 20) & 0xff;

	if (family >= 6)
		model += ((eax >> 16) & 0xf) << 4;

	hsmp_data.x86_family = family;

	/*
	 * Family 0x17 = Zen/Zen2, models 30h - 3Fh
	 * Family 0x19 = Zen3
	 */
	if (id == 2 && (
#ifdef HSMP_FAMILY_0x17
		(family == 0x17 && model >= 0x30 && model <= 0x3F) ||
#endif
		(family >= 0x19))) {
		pr_debug("Detected %s CPU family %xh model %xh\n",
			 vendstr[id], family, model);

		if (family == 0x17)
			fprintf(stderr,
				"WARNING: libhsmp not supported on %s CPU Family 0x17 CPUs\n",
				vendstr[id]);
		return 0;
	}

	pr_debug("libhsmp not supported on %s CPU family %xh model %xh\n",
		 vendstr[id], family, model);
	hsmp_data.hsmp_disabled = 1;
	errno = ENOTSUP;
	return -1;
}

static int read_id(char *line)
{
	char *c = line;

	while (*c++ != ':') {}
	c++;

	return strtol(c, NULL, 10);
}

static int hsmp_get_cpu_data(void)
{
	FILE *fp;
	char *line;
	size_t len;
	int read;
	int err;

	fp = fopen("/proc/cpuinfo", "r");
	if (!fp) {
		pr_debug("Failed to open \"/proc/cpuinfo\"\n");
		return -1;
	}

	line = NULL;
	err = 0;
	while ((read = getline(&line, &len, fp)) != -1) {
		if (!strncmp(line, "processor", 9)) {
			int cpu_id;

			cpu_id = read_id(line);
			if (cpu_id >= MAX_CPUS)
				break;

			/* goto 'physical id' */
			while ((read = getline(&line, &len, fp)) != -1) {
				if (!strncmp(line, "physical id", 11)) {
					hsmp_data.cpus[cpu_id].socket_id = read_id(line);
					break;
				}
			}

			if (read == -1) {
				err = -1;
				break;
			}

			/* goto 'apicid' */
			while ((read = getline(&line, &len, fp)) != -1) {
				if (!strncmp(line, "apicid", 6)) {
					hsmp_data.cpus[cpu_id].apicid = read_id(line);
					break;
				}
			}

			if (read == -1) {
				err = -1;
				break;
			}

			hsmp_data.cpus[cpu_id].valid = 1;
		}
	}

	free(line);
	fclose(fp);

	if (err) {
		pr_debug("Failed to parse \"/proc/cpuinfo\" for CPU socket id and apicid\n");
		errno = EINVAL;
	}

	return err;
}

static int hsmp_init(void)
{
	int err;

	err = get_system_info();
	if (err)
		return -1;

	/* Set supported HSMP interface version */
	hsmp_data.supported_intf = SMN_INTF_SUPPORTED;

	err = setup_nbios();
	if (err)
		return -1;

	err = hsmp_get_cpu_data();
	if (!err)
		err = hsmp_probe();

	if (err) {
		cleanup_nbios();
		return err;
	}

	hsmp_data.initialized = 1;
	return 0;
}

static int hsmp_enter(enum hsmp_msg_t msg_id)
{
	int err = 0;

	if (geteuid() != 0) {
		pr_debug("libhsmp requires root access!\n");
		errno = EPERM;
		return -1;
	}

	if (hsmp_data.hsmp_disabled) {
		errno = ENOTSUP;
		return -1;
	}

	if (!hsmp_data.initialized) {
		err = hsmp_init();
		if (err)
			return err;
	}

	if (!msg_id_supported(msg_id)) {
		errno = ENOMSG;
		return -1;
	}

	return 0;
}

static void hsmp_fini(void)
{
	cleanup_nbios();
}

void __attribute__ ((destructor)) hsmp_fini(void);

int hsmp_smu_fw_version(struct smu_fw_version *smu_fw)
{
	int err;

	err = hsmp_enter(HSMP_GET_SMU_VER);
	if (err)
		return -1;

	if (!smu_fw) {
		errno = EINVAL;
		return -1;
	}

	smu_fw->major = hsmp_data.smu_firmware.ver.major;
	smu_fw->minor = hsmp_data.smu_firmware.ver.minor;
	smu_fw->debug = hsmp_data.smu_firmware.ver.debug;

	return 0;
}

int hsmp_interface_version(int *version)
{
	int err;

	err = hsmp_enter(HSMP_GET_PROTO_VER);
	if (err)
		return -1;

	if (!version) {
		errno = EINVAL;
		return -1;
	}

	*version = hsmp_data.smu_intf_ver;
	return 0;
}

int hsmp_socket_power(int socket_id, uint32_t *power_mw)
{
	struct hsmp_message msg = { 0 };
	int err;

	err = hsmp_enter(HSMP_GET_SOCKET_POWER);
	if (err)
		return -1;

	if (!power_mw) {
		errno = EINVAL;
		return -1;
	}

	msg.msg_num = HSMP_GET_SOCKET_POWER;
	msg.response_sz = 1;

	err = hsmp_send_message(socket_id, &msg);
	if (err)
		return err;

	*power_mw = msg.response[0];
	return 0;
}

int hsmp_set_socket_power_limit(int socket_id, uint32_t power_limit)
{
	struct hsmp_message msg = { 0 };
	int err;

	err = hsmp_enter(HSMP_SET_SOCKET_POWER_LIMIT);
	if (err)
		return -1;

	msg.msg_num = HSMP_SET_SOCKET_POWER_LIMIT;
	msg.num_args = 1;
	msg.args[0] = power_limit;

	return hsmp_send_message(socket_id, &msg);
}

int hsmp_socket_power_limit(int socket_id, uint32_t *power_limit)
{
	struct hsmp_message msg = { 0 };
	int err;

	err = hsmp_enter(HSMP_GET_SOCKET_POWER_LIMIT);
	if (err)
		return -1;

	if (!power_limit) {
		errno = EINVAL;
		return -1;
	}

	msg.msg_num = HSMP_GET_SOCKET_POWER_LIMIT;
	msg.response_sz = 1;

	err = hsmp_send_message(socket_id, &msg);
	if (err)
		return err;

	*power_limit = msg.response[0];
	return 0;
}

int hsmp_socket_max_power_limit(int socket_id, uint32_t *max_power)
{
	struct hsmp_message msg = { 0 };
	int err;

	err = hsmp_enter(HSMP_GET_SOCKET_POWER_LIMIT_MAX);
	if (err)
		return -1;

	if (!max_power) {
		errno = EINVAL;
		return -1;
	}

	msg.msg_num = HSMP_GET_SOCKET_POWER_LIMIT_MAX;
	msg.response_sz = 1;

	err = hsmp_send_message(socket_id, &msg);
	if (err)
		return err;

	*max_power = msg.response[0];
	return 0;
}

int hsmp_set_cpu_boost_limit(int cpu, uint32_t boost_limit)
{
	struct hsmp_message msg = { 0 };
	int socket_id;
	int apicid;
	int err;

	err = hsmp_enter(HSMP_SET_BOOST_LIMIT);
	if (err)
		return err;

	apicid = cpu_apicid(cpu);
	if (apicid < 0) {
		errno = EINVAL;
		return -1;
	}

	socket_id = cpu_socket_id(cpu);
	if (socket_id < 0) {
		errno = EINVAL;
		return -1;
	}

	msg.msg_num = HSMP_SET_BOOST_LIMIT;
	msg.num_args = 1;
	msg.args[0] = apicid << 16 | boost_limit;

	return hsmp_send_message(socket_id, &msg);
}

static int _set_socket_boost_limit(int socket_id, uint32_t boost_limit)
{
	struct hsmp_message msg = { 0 };

	msg.msg_num = HSMP_SET_BOOST_LIMIT_SOCKET;
	msg.num_args = 1;
	msg.args[0] = boost_limit;

	return hsmp_send_message(socket_id, &msg);
}

int hsmp_set_socket_boost_limit(int socket_id, uint32_t boost_limit)
{
	int err;

	err = hsmp_enter(HSMP_SET_BOOST_LIMIT_SOCKET);
	if (err)
		return err;

	return _set_socket_boost_limit(socket_id, boost_limit);
}

int hsmp_set_system_boost_limit(uint32_t boost_limit)
{
	int socket_id;
	int err;

	err = hsmp_enter(HSMP_SET_BOOST_LIMIT_SOCKET);
	if (err)
		return -1;

	for (socket_id = 0; socket_id < MAX_SOCKETS; socket_id++) {
		if (!socket_id_to_nbio(socket_id))
			break;

		err = _set_socket_boost_limit(socket_id, boost_limit);
		if (err)
			break;
	}

	return err;
}

int hsmp_cpu_boost_limit(int cpu, uint32_t *boost_limit)
{
	struct hsmp_message msg = { 0 };
	int socket_id;
	int apicid;
	int err;

	err = hsmp_enter(HSMP_GET_BOOST_LIMIT);
	if (err)
		return -1;

	if (!boost_limit) {
		errno = EINVAL;
		return -1;
	}

	apicid = cpu_apicid(cpu);
	if (apicid < 0) {
		errno = EINVAL;
		return -1;
	}

	socket_id = cpu_socket_id(cpu);
	if (socket_id < 0) {
		errno = EINVAL;
		return -1;
	}

	msg.msg_num = HSMP_GET_BOOST_LIMIT;
	msg.num_args = 1;
	msg.response_sz = 1;
	msg.args[0] = apicid;

	err = hsmp_send_message(socket_id, &msg);
	if (err)
		return err;

	*boost_limit = msg.response[0];
	return err;
}

int hsmp_proc_hot_status(int socket_id, int *status)
{
	struct hsmp_message msg = { 0 };
	int err;

	err = hsmp_enter(HSMP_GET_PROC_HOT);
	if (err)
		return -1;

	if (!status) {
		errno = EINVAL;
		return -1;
	}

	msg.msg_num = HSMP_GET_PROC_HOT;
	msg.response_sz = 1;

	err = hsmp_send_message(socket_id, &msg);
	if (err)
		return err;

	*status = msg.response[0];
	return 0;
}

int hsmp_set_xgmi_width(enum hsmp_xgmi_width min_width,
			enum hsmp_xgmi_width max_width)
{
	struct hsmp_message msg = { 0 };
	int socket_id;
	u8 min, max;
	int err;

	err = hsmp_enter(HSMP_SET_XGMI_LINK_WIDTH);
	if (err)
		return -1;

	if (hsmp_data.x86_family >= 0x19)
		min = HSMP_XGMI_WIDTH_X2;
	else
		min = HSMP_XGMI_WIDTH_X8;

	if (min_width < min || min_width > HSMP_XGMI_WIDTH_X16 ||
	    max_width < min_width || max_width > HSMP_XGMI_WIDTH_X16) {
		errno = EINVAL;
		return -1;
	}

	min = min_width;
	max = max_width;

	msg.msg_num = HSMP_SET_XGMI_LINK_WIDTH;
	msg.num_args = 1;
	msg.args[0] = (min << 8) | max;

	for (socket_id = 0; socket_id < MAX_SOCKETS; socket_id++) {
		if (!socket_id_to_nbio(socket_id))
			break;

		err = hsmp_send_message(socket_id, &msg);
		if (err)
			break;
	}

	return err;
}

int hsmp_set_xgmi_auto(void)
{
	enum hsmp_xgmi_width min;

	if (hsmp_data.x86_family >= 0x19)
		min = HSMP_XGMI_WIDTH_X2;
	else
		min = HSMP_XGMI_WIDTH_X8;

	return hsmp_set_xgmi_width(min, HSMP_XGMI_WIDTH_X16);
}

int hsmp_set_data_fabric_pstate(int socket_id, enum hsmp_df_pstate pstate)
{
	struct hsmp_message msg = { 0 };
	int err;

	/*
	 * HSMP_AUTO_DF_PSTATE has the higher hsmp_msg_t so we use
	 * that here for support validation. Both HSMP_AUTO_DF_PSTATE
	 * and HSMP_SET_DF_PSTATE are supported in HSMP interface
	 * versions 1 and later.
	 */
	err = hsmp_enter(HSMP_AUTO_DF_PSTATE);
	if (err)
		return err;

	if (pstate < HSMP_DF_PSTATE_0 || pstate > HSMP_DF_PSTATE_AUTO) {
		errno = EINVAL;
		return -1;
	}

	if (pstate == HSMP_DF_PSTATE_AUTO) {
		msg.msg_num = HSMP_AUTO_DF_PSTATE;
	} else {
		msg.msg_num = HSMP_SET_DF_PSTATE;
		msg.num_args = 1;
		msg.args[0] = pstate;
	}

	return hsmp_send_message(socket_id, &msg);
}

int hsmp_fabric_clocks(int socket_id, int *data_fabric_clock, int *mem_clock)
{
	struct hsmp_message msg = { 0 };
	int err;

	err = hsmp_enter(HSMP_GET_FCLK_MCLK);
	if (err)
		return err;

	if (!data_fabric_clock && !mem_clock) {
		errno = EINVAL;
		return -1;
	}

	msg.msg_num = HSMP_GET_FCLK_MCLK;
	msg.response_sz = 2;

	err = hsmp_send_message(socket_id, &msg);
	if (err)
		return err;

	if (data_fabric_clock)
		*data_fabric_clock = msg.response[0];

	if (mem_clock)
		*mem_clock = msg.response[1];

	return 0;
}

int hsmp_data_fabric_clock(int socket_id, int *data_fabric_clock)
{
	return hsmp_fabric_clocks(socket_id, data_fabric_clock, NULL);
}

int hsmp_memory_clock(int socket_id, int *mem_clock)
{
	return hsmp_fabric_clocks(socket_id, NULL, mem_clock);
}

int hsmp_core_clock_max_frequency(int socket_id, uint32_t *max_freq)
{
	struct hsmp_message msg = { 0 };
	int err;

	err = hsmp_enter(HSMP_GET_CCLK_THROTTLE_LIMIT);
	if (err)
		return err;

	if (!max_freq) {
		errno = EINVAL;
		return -1;
	}

	msg.msg_num = HSMP_GET_CCLK_THROTTLE_LIMIT;
	msg.response_sz = 1;

	err = hsmp_send_message(socket_id, &msg);
	if (err)
		return err;

	*max_freq = msg.response[0];
	return 0;
}

int hsmp_c0_residency(int socket_id, uint32_t *residency)
{
	struct hsmp_message msg = { 0 };
	int err;

	err = hsmp_enter(HSMP_GET_C0_PERCENT);
	if (err)
		return -1;

	if (!residency) {
		errno = EINVAL;
		return -1;
	}

	msg.msg_num = HSMP_GET_C0_PERCENT;
	msg.response_sz = 1;

	err = hsmp_send_message(socket_id, &msg);
	if (err)
		return err;

	*residency = msg.response[0];
	return 0;
}

int hsmp_set_nbio_pstate(u8 bus_num, enum hsmp_nbio_pstate pstate)
{
	struct hsmp_message msg = { 0 };
	struct nbio_dev *nbio;
	u8 dpm_min, dpm_max;
	int socket_id;
	int err;

	err = hsmp_enter(HSMP_SET_NBIO_DPM_LEVEL);
	if (err)
		return -1;

	nbio = bus_to_nbio(bus_num);
	if (!nbio) {
		errno = EINVAL;
		return -1;
	}

	switch (pstate) {
	case HSMP_NBIO_PSTATE_AUTO:
		dpm_min = 0;
		dpm_max = 2;
		break;
	case HSMP_NBIO_PSTATE_P0:
		dpm_min = 2;
		dpm_max = 2;
		break;
	default:
		errno = EINVAL;
		return -1;
	}

	socket_id = (nbio->index >= (MAX_NBIOS / 2)) ? 1 : 0;

	msg.msg_num = HSMP_SET_NBIO_DPM_LEVEL;
	msg.num_args = 1;
	msg.args[0] = (nbio->id << 16) | (dpm_max << 8) | dpm_min;

	return hsmp_send_message(socket_id, &msg);
}

int hsmp_next_bus(int idx, u8 *bus_num)
{
	struct nbio_dev *nbio;
	int err;

	/* Perform library initialization and HSMP test message */
	err = hsmp_enter(0);
	if (err)
		return err;

	if (!bus_num) {
		errno = EINVAL;
		return -1;
	}

	nbio = get_nbio(idx);
	if (!nbio || !nbio->dev) {  /* No IOHC at this array index */
		errno = ENODEV;
		return -1;
	}

	*bus_num = nbio->bus_base;

	/*
	 * Test if the next iteration would succeed. Return idx if so,
	 * return 0 if not.
	 */
	nbio = get_nbio(idx + 1);
	if (!nbio || !nbio->dev)
		return 0;

	return idx + 1;
}

int hsmp_ddr_bandwidths(int socket_id, uint32_t *max_bw,
			uint32_t *utilized_bw, uint32_t *utilized_pct)
{
	struct hsmp_message msg = { 0 };
	uint32_t result;
	int err;

	err = hsmp_enter(HSMP_GET_DDR_BANDWIDTH);
	if (err)
		return -1;

	if (!max_bw && !utilized_bw && !utilized_pct) {
		errno = EINVAL;
		return -1;
	}

	msg.msg_num = HSMP_GET_DDR_BANDWIDTH;
	msg.response_sz = 1;

	err = hsmp_send_message(socket_id, &msg);
	if (err)
		return err;

	result = msg.response[0];

	if (max_bw)
		*max_bw = result >> 20;

	if (utilized_bw)
		*utilized_bw = (result >> 8) & 0xFFFFF;

	if (utilized_pct)
		*utilized_pct = result & 0xFF;

	return 0;
}

int hsmp_ddr_max_bandwidth(int socket_id, uint32_t *max_bw)
{
	return hsmp_ddr_bandwidths(socket_id, max_bw, NULL, NULL);
}

int hsmp_ddr_utilized_bandwidth(int socket_id, uint32_t *utilized_bw)
{
	return hsmp_ddr_bandwidths(socket_id, NULL, utilized_bw, NULL);
}

int hsmp_ddr_utilized_percent(int socket_id, uint32_t *utilized_pct)
{
	return hsmp_ddr_bandwidths(socket_id, NULL, NULL, utilized_pct);
}

