// SPDX-License-Identifier: MIT License
/*
 * Copyright (C) 2021 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Lewis Carroll <lewis.carroll@amd.com>
 *
 * Example utility for configuring xGMI Dynamic Link Width Management (DLWM)
 * and data fabric P-state settings. This example code is provided as-is
 * without any support or expectation of suitability for a specific use case.
 */

#define _GNU_SOURCE	/* Needed for basename function */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>

#include <libhsmp.h>

#include "nbio_discovery.h"

static const char version[] = "1.1";
static bool do_set_fabric_pstate, do_set_link_width, do_get_status, do_defaults;
static int min_width, max_width, fabric_pstate;
static const char *me;

static void show_usage(void)
{
	printf("\nUsage: %1$s [option]\n\n", me);
	printf("This utility configures power management for the EPYC Data Fabric. For a 2P\n"
	       "system, you can set the limits for xGMI Dynamic Link width Management (DLWM)\n"
	       "and for both 1P and 2P systems you can set the fabric P-state to a fixed value\n"
	       "or return it to normal operation. Note wider link widths and lower fabric\n"
	       "P-state values consume more power. Setting limits other than the defaults\n"
	       "will increase idle power consumption.\n\n"
	       "Options:\n"
	       "-d  --defaults        Equivalent to --min-link-width auto --max-link-width auto\n"
	       "                      --fabric-pstate auto\n"
	       "-f  --fabric-pstate   Set data fabric P-state:\n"
	       "                      0 - fixed fabric P-State P0\n"
	       "                      1 - fixed fabric P-State P1\n"
	       "                      2 - fixed fabric P-State P2\n"
	       "                      3 - fixed fabric P-State equivalent to PROC_HOT asserted\n"
	       "                      auto - autonomous fabric P-state selection\n"
	       "-m  --min-link-width  Set minimum xGMI link width (2P system only):\n"
	       "-x  --max-link-width  Set maximum xGMI link width (2P system only):\n"
	       "                      2 - x2\n"
	       "                      8 - x8\n"
	       "                      16 - x16\n"
	       "                      auto - set min or max limit to the platform default\n"
	       "-g  --get-status      Get the current link width and fabric clocks\n"
	       "                      (not the configured min/max or P-state)\n"
	       "-v  --version         Display program version and exit\n"
	       "-h  --help            Display program usage and exit\n\n"
	       "Link width limit manipulation is only possible for 2P systems. These options\n"
	       "are ignored on a 1P system. Setting the same value for min and max link width\n"
	       "will disable DLWM and set a fixed link width. Since both min and max link width\n"
	       "must be set at the same time in hardware, if one of min/max link width is not\n"
	       "specified, the platform default will be used (same as if the value auto had\n"
	       "been specified).\n\n"
	       "Examples:\n"
	       "amd-df-util --min-link-width 8 --fabric-pstate 0\n"
	       "Enable DLWM and allow x8 and x16 link widths only (disable x2 link width), set\n"
	       "fixed data fabric P-state P0\n\n"
	       "amd-df-util --min-link-width 8 --max-link-width 8\n"
	       "Disable DLWM and set the link width to x8\n\n"
	       "amd-df-util --defaults\n"
	       "Enable DLWM and allow all supported link widths (normal operation)\n"
	       "Set automatic fabric P-state selection\n\n");
}

/*
 * Evaluates optarg for a base 10 number or "auto". The return value and
 * handling of the supplied arguments depend on what was found:
 * "auto"         - return value  = 0, is_auto = 1, val unchanged
 * Base 10 number - return value  = 0, is_auto = 0, val set to the found number
 * Anything else  - return value != 0, is_auto = 0, val unchanged
 */
static int get_val_or_auto(int *val, int *is_auto)
{
	int rv;

	if (!strncasecmp(optarg, "auto", 4)) {  /* Returns 0 for match */
		*is_auto = 1;
		return 0;
	}
	*is_auto = 0;

	errno = 0;
	rv = (int)strtol(optarg, NULL, 10);
	if (errno)
		return errno;

	*val = rv;
	return 0;
}

static struct option long_opts[] = {
	{"defaults",             no_argument, 0, 'd'},
	{"fabric-pstate",  required_argument, 0, 'f'},
	{"get-status",           no_argument, 0, 'g'},
	{"help",                 no_argument, 0, 'h'},
	{"min-link-width", required_argument, 0, 'm'},
	{"version",              no_argument, 0, 'v'},
	{"max-link-width", required_argument, 0, 'x'},
	{0, 0, 0, 0}
};

static inline void process_args(int argc, char **argv)
{
	char opt;
	int fam17h;

	/* No arguments: print usage */
	if (argc == 1) {
		show_usage();
		exit(0);
	}

	/* Set platform defaults */
	fam17h = is_fam17h();
	fabric_pstate = HSMP_DF_PSTATE_AUTO;
	min_width = fam17h ? 8 : 2;
	max_width = 16;

	while ((opt = getopt_long_only(argc, argv, "df:ghm:vx:", long_opts, NULL)) != -1) {
		int err, is_auto;

		switch (opt) {
		case 'd':
			do_defaults = true;
			break;
		case 'f':
			do_set_fabric_pstate = true;
			err = get_val_or_auto(&fabric_pstate, &is_auto);
			if (!err && is_auto)
				break;

			if (!err && fabric_pstate >= HSMP_DF_PSTATE_0 &&
			    fabric_pstate <= HSMP_DF_PSTATE_3)
				break;

			printf("Invalid value %s specified for fabric P-state.\n"
			       "Allowed values: %d - %d or auto\n",
			       optarg, HSMP_DF_PSTATE_0, HSMP_DF_PSTATE_3);
			exit(-EINVAL);
		case 'g':
			do_get_status = true;
			break;
		case 'h':
			show_usage();
			exit(0);
		case 'm':
			do_set_link_width = true;
			err = get_val_or_auto(&min_width, &is_auto);
			if (!err && is_auto)
				break;

			if (!err &&
			    ((!fam17h && min_width == 2) || min_width == 8 || min_width == 16))
				break;

			printf("Invalid value %s specified for min-link-width. Allowed values: ",
			       optarg);
			if (!fam17h)
				printf("2, ");
			printf("8 and 16\n");
			exit(-EINVAL);
		case 'v':
			printf("%s version %s\n\n", me, version);
			exit(0);
		case 'x':
			do_set_link_width = true;
			err = get_val_or_auto(&max_width, &is_auto);
			if (!err && is_auto)
				break;

			if (!err &&
			    ((!fam17h && max_width == 2) || max_width == 8 || max_width == 16))
				break;

			printf("Invalid value %s specified for max-link-width. Allowed values: ",
			       optarg);
			if (!fam17h)
				printf("2, ");
			printf("8 and 16\n");
			exit(-EINVAL);
			break;
		default:
			show_usage();
			exit(-EINVAL);
		}
	}
}

#define SMN_XGMI2_G0_PCS_LINK_STATUS1  0x12EF0050
#define XGMI_LINK_WIDTH_X2             (1 << 1)
#define XGMI_LINK_WIDTH_X8             (1 << 2)
#define XGMI_LINK_WIDTH_X16            (1 << 5)
static inline int f17f19_get_xgmi2_width(void)
{
	int err;
	uint32_t val;
	struct pci_dev *root;

	root = socket_id_to_dev(0);
	if (!root)
		return -1;

	err = smn_pci_read(root, SMN_XGMI2_G0_PCS_LINK_STATUS1, &val);
	if (err) {
		printf("Error %d reading xGMI2 G0 PCS link status register\n", err);
		return err;
	}

#ifdef DEBUG
	printf("XGMI2_G0_PCS_LINK_STATUS1 raw val: 0x%08X\n", val);
#endif

	val >>= 16;
	val  &= 0x3F;

	if (val & XGMI_LINK_WIDTH_X16)
		return 16;
	if (val & XGMI_LINK_WIDTH_X8)
		return 8;
	if (val & XGMI_LINK_WIDTH_X2)
		return 2;

	printf("Unable to determine xGMI2 link width, status = 0x%02X\n", val);
	return -1;
}

#define SMN_XGMI2_G0_PCS_CONTEXT5      0x12EF0114
#define SMN_FCH_PLL_CTRL0              0x02D02330
#define REF_CLK_100MHZ                 0x00
#define REF_CLK_133MHZ                 0x55
static inline int f17f19_get_xgmi2_speed(void)
{
	int err;
	uint32_t freqcnt, refclksel;
	struct pci_dev *root;

	root = socket_id_to_dev(0);
	if (!root)
		return -1;

	/* Get phy clock multiplier */
	err = smn_pci_read(root, SMN_XGMI2_G0_PCS_CONTEXT5, &freqcnt);
	if (err) {
		printf("Error %d reading xGMI2 G0 PCS context register\n", err);
		return err;
	}

#ifdef DEBUG
	printf("XGMI2_G0_PCS_CONTEXT5 raw val: 0x%08X\n", freqcnt);
#endif

	/* Determine reference clock - 100 MHz or 133 MHz */
	err = smn_pci_read(root, SMN_FCH_PLL_CTRL0, &refclksel);
	if (err) {
		printf("Error %d reading reference clock select\n", err);
		return err;
	}

#ifdef DEBUG
	printf("FCH_PLL_CTRL0 raw val: 0x%08X\n", refclksel);
#endif

	freqcnt  >>= 3;
	freqcnt   &= 0xFE;
	refclksel &= 0xFF;

	/* Calculate xGMI2 transfer speed in mega transfers per second MTS */
	if (refclksel == REF_CLK_100MHZ)
		return freqcnt * 100;
	if (refclksel == REF_CLK_133MHZ)
		return freqcnt * 133;

	printf("Unable to determine reference clock, refclksel = 0x%02X)\n", refclksel);
	return -1;
}

static void print_error(void)
{
	switch (errno) {
	case ENOTSUP:
		printf("HSMP is not supported on this processor / model "
		       "or is disabled in system firmware\n");
		break;
	case EAGAIN:
		printf("HSMP initialization failed for an unknown reason "
		       "but may succeed on a subsequent attempt\n");
		break;
	case ENODEV:
		printf("libhsmp initialization failed - possible problem "
		       "accessing the PCI subsystem\n");
		break;
	case ENOMSG:
		printf("The HSMP message to set xGMI dynamic link width limits "
		       "is not supported on this system\n");
		break;
	case EINVAL:
		printf("Invalid message parameters\n");
		break;
	case ETIMEDOUT:
		printf("HSMP message send timeout\n");
		break;
	case EBADMSG:
		printf("HSMP message send failure\n");
		break;
	default:
		printf("Unknown failure, errno = %d\n", errno);
	}
}

/* No error checking in this function! Returns x16 width for errors */
static enum hsmp_xgmi_width xgmi_width_to_arg(int width)
{
	if (width == 2)
		return HSMP_XGMI_WIDTH_X2;

	if (width == 8)
		return HSMP_XGMI_WIDTH_X8;

	return HSMP_XGMI_WIDTH_X16;
}

int main(int argc, char **argv)
{
	int rv, num_sockets;
	bool is_2p;
	int err = 0;

	/*
	 * Root permission required since reading an SMN register is required
	 * for NBIO tile to PCI bus topology discovery, and reading an SMN
	 * register is done via indirect access through PCI config space, and
	 * root permission is required to write the index register for that
	 * indirect access.
	 */
	rv = geteuid();
	if (rv) {
		printf("libhsmp applications must be run as root\n");
		exit(-EPERM);
	}

	me = basename(argv[0]);

	process_args(argc, argv);

	rv = setup_nbios(&num_sockets);
	if (rv < 0)
		exit(-rv);

	is_2p = (num_sockets == 2);

	if (do_set_link_width && max_width < min_width) {
		printf("Min link width %d must be less than max link width %d\n",
		       min_width, max_width);
		err = -EINVAL;
		goto cleanup;
	}

	if (!is_2p && do_set_link_width) {
		printf("Ignoring set link width command on 1P system\n");
		do_set_link_width = 0;
	}

	if (do_defaults) {
		if (do_set_fabric_pstate || do_set_link_width) {
			printf("Can't specify both defaults and fabric P-state "
			       "and/or link width options\n");
			err = -EINVAL;
			goto cleanup;
		}

		do_set_fabric_pstate = true;
		fabric_pstate        = HSMP_DF_PSTATE_AUTO;

		if (is_2p)
			do_set_link_width = true;
	}

	if (!do_get_status && !do_set_link_width && !do_set_fabric_pstate) {
		printf("Nothing to do...\n");
		goto cleanup;
	}

	if (do_get_status) {
		int socket;

		for (socket = 0; socket < num_sockets; socket++) {
			int fclk = 0, mclk = 0;

			printf("Calling hsmp_fabric_clocks for socket %d...", socket);
			rv = hsmp_fabric_clocks(socket, &fclk, &mclk);
			printf("%s\n", hsmp_strerror(rv, errno));
			if (rv) {
				err = rv;
				print_error();
			} else {
				printf("  Fabric clock = %u MHz, memory speed = %u MTS\n",
				       fclk, mclk * 2);
			}
		}

		if (is_2p) {
			int curr_width = f17f19_get_xgmi2_width();
			int curr_speed = f17f19_get_xgmi2_speed();

			if (curr_width > 0 && curr_speed > 0)
				printf("xGMI2 link width x%d, speed %d MTS\n",
				       curr_width, curr_speed);
		}
	}

	if (do_set_link_width) {
		enum hsmp_xgmi_width min = xgmi_width_to_arg(min_width);
		enum hsmp_xgmi_width max = xgmi_width_to_arg(max_width);

		printf("Calling hsmp_set_xgmi_width, min = %d, max = %d...", min, max);
		rv = hsmp_set_xgmi_width(min, max);
		printf("%s\n", hsmp_strerror(rv, errno));
		if (rv) {
			err = rv;
			print_error();
		}
	}

	if (do_set_fabric_pstate) {
		int socket;

		for (socket = 0; socket < num_sockets; socket++) {
			printf("Calling hsmp_set_data fabric_pstate for socket %d, ", socket);
			if (fabric_pstate == HSMP_DF_PSTATE_AUTO)
				printf("auto P-state...");
			else
				printf("P-state P%d...", fabric_pstate);
			rv = hsmp_set_data_fabric_pstate(socket, fabric_pstate);
			printf("%s\n", hsmp_strerror(rv, errno));
			if (rv) {
				err = rv;
				print_error();
			}
		}
	}

cleanup:
	cleanup_nbios();

	return err;
}
