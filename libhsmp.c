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

static struct smu_port {
	u32 index_reg;  /* PCI-e index register for SMU access */
	u32 data_reg;   /* PCI-e data register for SMU access */
} smu, hsmp;

static struct {
	u32 mbox_msg_id;    /* SMU register for HSMP message ID */
	u32 mbox_status;    /* SMU register for HSMP status word */
	u32 mbox_data;      /* SMU base for message argument(s) */
	u32 mbox_timeout;   /* Timeout in MS to consider the SMU hung */
} hsmp_access;

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

struct hsmp_message {
	enum hsmp_msg_t	msg_num;	/* Message number */
	u16		num_args;	/* Number of arguments in message */
	u16		response_sz;	/* Number of expected response words */
	u32		args[8];	/* Argument(s) */
	u32		response[8];	/* Response word(s) */
};

union smu_fw_ver {
	struct smu_fw_version ver;
	u32 raw_u32;
};

struct nbio_dev {
	struct pci_dev *dev;		/* Pointer to PCI-e device in the socket */
	u8		id;		/* NBIO tile number within the socket */
	u8		bus_base;	/* Lowest hosted PCI-e bus number */
	u8		bus_limit;	/* Highest hosted PCI-e bus number + 1 */
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
	struct pci_access	*pacc;			/* PCIlib */
	struct nbio_dev		nbios[MAX_NBIOS];	/* Array of DevID 0x1480 devices */
	struct cpu_dev		cpus[MAX_CPUS];
	union smu_fw_ver	smu_firmware;		/* SMU firmware version code */
	unsigned int		hsmp_proto_ver;		/* HSMP implementation level */
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
	enum hsmp_msg_t max_supported_id;

	switch (hsmp_data.hsmp_proto_ver) {
	case 1:
		max_supported_id = HSMP_GET_C0_PERCENT;
		break;

	case 2:
		max_supported_id = HSMP_SET_NBIO_DPM_LEVEL;
		break;

	case 3:
		max_supported_id = HSMP_GET_DDR_BANDWIDTH;
		break;

	default:
		return false;
	}

	return msg_id <= max_supported_id;
}

#define PCI_VENDOR_ID_AMD		0x1022
#define F17F19_IOHC_DEVID		0x1480
#define SMN_IOHCMISC0_NB_BUS_NUM_CNTL	0x13B10044  /* Address in SMN space */
#define SMN_IOHCMISC_OFFSET		0x00100000  /* Offset for MISC[1..3] */

/*
 * SMU access functions
 * Returns 0 on success, negative error code on failure. The return status
 * is for the SMU access, not the result of the intended SMU or HSMP operation.
 *
 * SMU PCI config space access method
 * There are two access apertures defined in the PCI-e config space for the
 * North Bridge, one for general purpose SMU register reads/writes and a second
 * aperture specific for HSMP messages and responses. For both reads and writes,
 * step one is to write the register to be accessed to the appropriate aperture
 * index register. Step two is to read or write the appropriate aperture data
 * register.
 */
static int smu_pci_write(struct pci_dev *root, u32 reg_addr,
			 u32 reg_data, struct smu_port *port)
{
	pr_debug_pci("pci_write_long dev 0x%p, addr 0x%08X, data 0x%08X\n",
		     root, port->index_reg, reg_addr);
	pci_write_long(root, port->index_reg, reg_addr);

	pr_debug_pci("pci_write_long dev 0x%p, addr 0x%08X, data 0x%08X\n",
		     root, port->index_reg, reg_addr);
	pci_write_long(root, port->data_reg, reg_data);

	return 0;
}

static int smu_pci_read(struct pci_dev *root, u32 reg_addr,
			u32 *reg_data, struct smu_port *port)
{
	pr_debug_pci("pci_write_long dev 0x%p, addr 0x%08X, data 0x%08X\n",
		     root, port->index_reg, reg_addr);
	pci_write_long(root, port->index_reg, reg_addr);

	*reg_data = pci_read_long(root, port->data_reg);
	pr_debug_pci("pci_read_long  dev 0x%p, addr 0x%08X, data 0x%08X\n",
		     root, port->data_reg, *reg_data);

	return 0;
}

/*
 * Return the PCI device pointer for the IOHC dev hosting the lowest numbered
 * PCI bus in the specified socket (0 or 1). If socket_id 1 is passed on a 1P
 * system, NULL will be returned.
 */
static struct pci_dev *socket_id_to_dev(int socket_id)
{
	int idx;

	if (socket_id < 0 || socket_id >= MAX_SOCKETS)
		return NULL;

	idx = socket_id * 4;
	return hsmp_data.nbios[idx].dev;
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
 * Send a message to the SMU access port via PCI-e config space registers.
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
	u32 mbox_status;

	/* Zero the status register */
	mbox_status = HSMP_STATUS_NOT_READY;
	err = smu_pci_write(root_dev, hsmp_access.mbox_status, mbox_status, &hsmp);
	if (err) {
		pr_debug("Error %d clearing HSMP mailbox status register\n", err);
		return err;
	}

	/* Write any message arguments */
	for (arg_num = 0; arg_num < msg->num_args; arg_num++) {
		err = smu_pci_write(root_dev, hsmp_access.mbox_data + (arg_num << 2),
				    msg->args[arg_num], &hsmp);
		if (err) {
			pr_debug("Error %d writing HSMP message argument %d\n",
				 err, arg_num);
			return err;
		}
	}

	/* Write the message ID which starts the operation */
	err = smu_pci_write(root_dev, hsmp_access.mbox_msg_id, msg->msg_num, &hsmp);
	if (err) {
		pr_debug("Error %d writing HSMP message ID %u\n", err, msg->msg_num);
		return err;
	}

	timeout = hsmp_access.mbox_timeout;

	/*
	 * Assume it takes at least one SMU FW cycle (1 MS) to complete
	 * the operation. Some operations might complete in two, some in
	 * more. So first thing we do is yield the CPU.
	 */
retry:
	nanosleep(&one_ms, NULL);
	err = smu_pci_read(root_dev, hsmp_access.mbox_status, &mbox_status, &hsmp);
	if (err) {
		pr_debug("HSMP message ID %u - error %d reading mailbox status\n",
			 err, msg->msg_num);
		return err;
	}

	/*
	 * SMU has not responded to the message yet.
	 *
	 * If we time out waiting for SMU to respond indicates HSMP is not
	 * enabled in BIOS. Unfortunately there is no error return if HSMP
	 * is disabled, SMU simply does not respond.
	 *
	 * Mark HSMP as disabled, this results in any future calls returning
	 * ENOTSUP. There is no way to enable HSMP without rebooting.
	 */
	if (mbox_status == HSMP_STATUS_NOT_READY) {
		if (--timeout == 0) {
			pr_debug("SMU timeout for message ID %u, HSMP is not enabled\n",
				 msg->msg_num);
			hsmp_data.hsmp_disabled = 1;
			errno = ENOTSUP;
			return -1;
		}

		goto retry;
	}

	/*
	 * Some platforms may not support every HSMP interface covered
	 * by the HSMP interface version specified. For these instances
	 * a status code of HSMP_ERR_INVALID_MSG_ID is returned. If we
	 * see this status and the msg_id is considered supported by the
	 * interface version we should return ENOTSUP.
	 */
	if (mbox_status == HSMP_ERR_INVALID_MSG_ID &&
	    msg_id_supported(msg->msg_num)) {
		errno = ENOTSUP;
		return -1;
	}

	if (mbox_status != HSMP_STATUS_OK)
		return mbox_status;

	/* SMU has responded OK. Read response data */
	for (arg_num = 0; arg_num < msg->response_sz; arg_num++) {
		err = smu_pci_read(root_dev, hsmp_access.mbox_data + (arg_num << 2),
				   &msg->response[arg_num], &hsmp);
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
	struct pci_dev *root_dev;
	int err;
#ifdef DEBUG_HSMP
	unsigned int arg_num = 0;
#endif

	root_dev = socket_id_to_dev(socket_id);
	if (!root_dev) {
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

	err = _hsmp_send_message(root_dev, msg);
	hsmp_unlock();

	return err;
}

/* Read a register in SMN address space */
static int smu_read(struct pci_dev *root, u32 addr, u32 *val)
{
	return smu_pci_read(root, addr, val, &smu);
}

/*
 * Takes a PCI-e bus number and returns the index into the NBIOs array
 * matching the host NBIO device. Returns -1 if the bus is not found.
 */
static int bus_to_nbio(u8 bus)
{
	int idx;

	for (idx = 0; idx < MAX_NBIOS; idx++) {
		if (bus >= hsmp_data.nbios[idx].bus_base &&
		    bus <= hsmp_data.nbios[idx].bus_limit)
			return idx;
	}

	return -1;
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
 * Check if this CPU supports HSMP (based on vendor, family, and model). If so,
 * attempt a test message and if successful, retrieve the protocol version
 * and SMU firmware version. Check if the protocol version is supported.
 * Returns 0 for success
 * Returns -ENODEV for unsupported protocol version, unsupported CPU,
 * or if probe or test message fails.
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
		if (!socket_id_to_dev(socket_id))
			break;

		msg.msg_num = HSMP_TEST;
		msg.num_args = 1;
		msg.args[0] = 1;
		msg.response_sz = 1;

		err = hsmp_send_message(socket_id, &msg);
		if (err) {
			pr_debug("HSMP Test failed for socket %d\n", socket_id);
			errno = ENOTSUP;
			return -1;
		}

		if (msg.response[0] != msg.args[0] + 1) {
			pr_debug("HSMP test failed for socket %d, expected %x, received %x\n",
				 socket_id, msg.args[0] + 1, msg.response[0]);
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
				errno = ENOTSUP;
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
				errno = ENOTSUP;
				return -1;
			}

			hsmp_data.hsmp_proto_ver = msg.response[0];
			pr_debug("Interface Version: %d\n", hsmp_data.hsmp_proto_ver);

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
	errno = ENOTSUP;
	return -1;
}

static void hsmp_clear_nbio_table(void)
{
	int i;

	for (i = 0; i < MAX_NBIOS; i++) {
		hsmp_data.nbios[i].dev = NULL;
		hsmp_data.nbios[i].id = 0;
		hsmp_data.nbios[i].bus_base = 0xFF;
		hsmp_data.nbios[i].bus_limit = 0;
	}
}

static void hsmp_cleanup_nbios(void)
{
	if (hsmp_data.pacc) {
		pci_cleanup(hsmp_data.pacc);
		hsmp_data.pacc = NULL;
	}

	hsmp_clear_nbio_table();
}

static int hsmp_setup_nbios(void)
{
	struct pci_dev *dev;
	int num_nbios;
	u8 base;
	int i;

	hsmp_clear_nbio_table();

	/* Setup pcilib */
	hsmp_data.pacc = pci_alloc();
	if (!hsmp_data.pacc) {
		pr_debug("Failed to allocate PCI access structures\n");
		goto nbio_setup_error;
	}

	/* First, find all IOHC devices (root complex) */
	pci_init(hsmp_data.pacc);
	pci_scan_bus(hsmp_data.pacc);

	num_nbios = 0;
	for (dev = hsmp_data.pacc->devices; dev; dev = dev->next) {
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

		hsmp_data.nbios[num_nbios].dev = dev;
		hsmp_data.nbios[num_nbios].bus_base = base;
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
			if (hsmp_data.nbios[j].bus_base < hsmp_data.nbios[i].bus_base) {
				struct pci_dev *temp_dev = hsmp_data.nbios[i].dev;
				u8 temp_bus_base         = hsmp_data.nbios[i].bus_base;

				hsmp_data.nbios[i].dev      = hsmp_data.nbios[j].dev;
				hsmp_data.nbios[i].bus_base = hsmp_data.nbios[j].bus_base;

				hsmp_data.nbios[j].dev      = temp_dev;
				hsmp_data.nbios[j].bus_base = temp_bus_base;
			}
		}
	}

	/* Calculate bus limits - we can safely assume no overlapping ranges */
	for (i = 0; i < num_nbios; i++) {
		if (i < num_nbios - 1)
			hsmp_data.nbios[i].bus_limit = hsmp_data.nbios[i + 1].bus_base - 1;
		else
			hsmp_data.nbios[i].bus_limit = 0xFF;
	}

	/* Finally get IOHC ID for each bus base */
	for (i = 0; i < num_nbios; i++) {
		int err, idx;
		u32 addr, val;

		addr = SMN_IOHCMISC0_NB_BUS_NUM_CNTL + (i & 0x3) * SMN_IOHCMISC_OFFSET;
		err = smu_read(hsmp_data.nbios[i].dev, addr, &val);
		if (err) {
			pr_debug("Error %d accessing socket %d IOHCMISC%d\n",
				 err, i >> 2, i & 0x3);
			goto nbio_setup_error;
		}

		pr_debug("Socket %d IOHC%d smu_read addr 0x%08X = 0x%08X\n",
			 i >> 2, i & 0x3, addr, val);
		base = val & 0xFF;

		/* Look up this bus base in our array */
		idx = bus_to_nbio(base);
		if (idx == -1) {
			pr_debug("Unable to map bus 0x%02X to an IOHC device\n", base);
			goto nbio_setup_error;
		}

		hsmp_data.nbios[idx].id = i & 0x3;
	}

	/* Dump the final table */
#ifdef DEBUG_HSMP
	for (i = 0; i < MAX_NBIOS; i++) {
		pr_debug("IDX %d: Bus range 0x%02X - 0x%02X --> Socket %d IOHC %d\n",
			 i, hsmp_data.nbios[i].bus_base, hsmp_data.nbios[i].bus_limit,
			 i >> 2, hsmp_data.nbios[i].id);
	}
#endif

	return 0;

nbio_setup_error:
	hsmp_cleanup_nbios();
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

	/* Offsets in PCIe config space for 0x1480 DevID (IOHC) */
	smu.index_reg  = 0x60;
	smu.data_reg   = 0x64;
	hsmp.index_reg = 0xC4;
	hsmp.data_reg  = 0xC8;

	/* Offsets in SMN address space */
	hsmp_access.mbox_msg_id  = 0x3B10534;
	hsmp_access.mbox_status  = 0x3B10980;
	hsmp_access.mbox_data    = 0x3B109E0;

	hsmp_access.mbox_timeout = 500;

	err = hsmp_setup_nbios();
	if (err)
		return -1;

	err = hsmp_get_cpu_data();
	if (!err)
		err = hsmp_probe();

	if (err) {
		hsmp_cleanup_nbios();
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
		errno = ENOTSUP;
		return -1;
	}

	return 0;
}

static void hsmp_fini(void)
{
	hsmp_cleanup_nbios();
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

	*version = hsmp_data.hsmp_proto_ver;
	return 0;
}

int hsmp_socket_power(int socket_id, u32 *power_mw)
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

int hsmp_set_socket_power_limit(int socket_id, u32 power_limit)
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

int hsmp_socket_power_limit(int socket_id, u32 *power_limit)
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

int hsmp_socket_max_power_limit(int socket_id, u32 *max_power)
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

int hsmp_set_cpu_boost_limit(int cpu, u32 boost_limit)
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

static int _set_socket_boost_limit(int socket_id, u32 boost_limit)
{
	struct hsmp_message msg = { 0 };

	msg.msg_num = HSMP_SET_BOOST_LIMIT_SOCKET;
	msg.num_args = 1;
	msg.args[0] = boost_limit;

	return hsmp_send_message(socket_id, &msg);
}

int hsmp_set_socket_boost_limit(int socket_id, u32 boost_limit)
{
	int err;

	err = hsmp_enter(HSMP_SET_BOOST_LIMIT_SOCKET);
	if (err)
		return err;

	return _set_socket_boost_limit(socket_id, boost_limit);
}

int hsmp_set_system_boost_limit(u32 boost_limit)
{
	int socket_id;
	int err;

	err = hsmp_enter(HSMP_SET_BOOST_LIMIT_SOCKET);
	if (err)
		return -1;

	for (socket_id = 0; socket_id < MAX_SOCKETS; socket_id++) {
		if (!socket_id_to_dev(socket_id))
			break;

		err = _set_socket_boost_limit(socket_id, boost_limit);
		if (err)
			break;
	}

	return err;
}

int hsmp_cpu_boost_limit(int cpu, u32 *boost_limit)
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
		if (!socket_id_to_dev(socket_id))
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

int hsmp_core_clock_max_frequency(int socket_id, u32 *max_freq)
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

int hsmp_c0_residency(int socket_id, u32 *residency)
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
	int idx, socket_id;
	int err;

	err = hsmp_enter(HSMP_SET_NBIO_DPM_LEVEL);
	if (err)
		return -1;

	idx = bus_to_nbio(bus_num);
	if (idx == -1) {
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

	nbio = &hsmp_data.nbios[idx];
	socket_id = (idx >= (MAX_NBIOS / 2)) ? 1 : 0;

	msg.msg_num = HSMP_SET_NBIO_DPM_LEVEL;
	msg.num_args = 1;
	msg.args[0] = (nbio->id << 16) | (dpm_max << 8) | dpm_min;

	return hsmp_send_message(socket_id, &msg);
}

int hsmp_next_bus(int idx, u8 *bus_num)
{
	int err;

	err = hsmp_enter(0);
	if (err)
		return -1;

	if (!bus_num)
		return -1;

	if (idx < 0 || idx >= MAX_NBIOS)
		return -1;

	if (!hsmp_data.nbios[idx].dev)
		return -1;

	*bus_num = hsmp_data.nbios[idx].bus_base;
	return idx + 1;
}

int hsmp_ddr_bandwidths(int socket_id, u32 *max_bw,
			u32 *utilized_bw, u32 *utilized_pct)
{
	struct hsmp_message msg = { 0 };
	u32 result;
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

int hsmp_ddr_max_bandwidth(int socket_id, u32 *max_bw)
{
	return hsmp_ddr_bandwidths(socket_id, max_bw, NULL, NULL);
}

int hsmp_ddr_utilized_bandwidth(int socket_id, u32 *utilized_bw)
{
	return hsmp_ddr_bandwidths(socket_id, NULL, utilized_bw, NULL);
}

int hsmp_ddr_utilized_percent(int socket_id, u32 *utilized_pct)
{
	return hsmp_ddr_bandwidths(socket_id, NULL, NULL, utilized_pct);
}

