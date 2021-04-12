// SPDX-License-Identifier: MIT License
/*
 * Copyright (C) 2021 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Lewis Carroll <lewis.carroll@amd.com>
 *
 * Example utility for setting Power Management level for PCI-e logic
 * This example code is provided as-is without any support or expectation of
 * suitability for a specific use case.
 */

#define _GNU_SOURCE  /* Needed for basename function */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>

#include <libhsmp.h>

static const char version[] = "1.3";
static const char *me;

static void show_usage(void)
{
	printf("\nUsage: %1$s [option]\n\n", me);
	printf("This utility disables Dynamic Power Management (DPM) for all PCI-e root\n"
	       "complexes in the system and locks the logic into the highest performance\n"
	       "operational mode.\n\n"
	       "Options:\n"
	       "-v  --version	 Display program version and exit\n"
	       "-h  --help	Display program usage and exit\n");
}

int main(int argc, char **argv)
{
	u8 bus_num;
	int err;
	int idx = 0;
	enum hsmp_nbio_pstate pstate = HSMP_NBIO_PSTATE_P0;

	me = basename(argv[0]);

	if (argc > 1) {
		/* First argument is -v: print version */
		if (strcmp(argv[1], "-v") == 0 ||
		    strcmp(argv[1], "--version") == 0) {
			printf("%s version %s\n", me, version);
			return 0;
		}

		/* First argument is -h / --help: print usage */
		if (strcmp(argv[1], "-h") == 0 ||
		    strcmp(argv[1], "--help") == 0) {
			show_usage();
			return 0;
		}

		printf("Unrecognized option %s\n", argv[1]);
		show_usage();
		return -EINVAL;
	}

	/*
	 * Loop through the base busses, one for each NBIO block.
	 * Call the HSMP function to set the NBIO block to max performance.
	 *
	 * The first call to this function will init the library. The iteration
	 * ends when the return value of hsmp_next_bus is 0. A return value of
	 * < 0 indicates an error.
	 */
	errno = 0;
	do {
		idx = hsmp_next_bus(idx, &bus_num);
		if (idx < 0)
			break;
		printf("Calling hsmp_set_nbio_pstate, P-state %d, base bus 0x%02X...",
		       (int)pstate, bus_num);
		err = hsmp_set_nbio_pstate(bus_num, pstate);
		printf("%s\n", hsmp_strerror(err, errno));
	} while (idx > 0 && err == 0);

	switch (errno) {
	case 0:
		break;
	case EPERM:
		printf("libhsmp applications must be run as root\n");
		break;
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
		printf("The HSMP message to set NBIO LCLK DPM levels is "
		       "not supported on this system\n");
		break;
	case ETIMEDOUT:
		printf("HSMP message send timeout\n");
		break;
	case EBADMSG:
		printf("HSMP message send failure\n");
		break;
	case EINVAL:
		/* Should not happen in this example */
		printf("Invalid parameter\n");
		break;
	default:
		printf("Unknown failure, errno = %d\n", errno);
	}

	return -errno;
}

