// SPDX-License-Identifier: MIT License
/*
 * Copyright (C) 2020 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Nathan Fontenot <nathan.fontenot@amd.com>
 *
 * AMD Host System Management Port library test module
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>

#ifndef BUILD_STATIC
#include <cpuid.h>
#endif

#include <sys/types.h>

#ifndef BUILD_STATIC
#include "libhsmp.h"
#endif

#ifdef BUILD_STATIC
#include "smn.c"
#include "nbio.c"
#include "libhsmp.c"
#endif

struct smu_fw_version smu_fw;
int interface_version;
int hsmp_disabled;
int test_passed;
int unsupported_interface;

int total_tests;
int passed_tests;
int failed_tests;
int ebadmsg_tests;

int verbose = 0;
int privileged_user = 0;

#define TEST_INDENT	"    "
#define TEST_BUF_SZ	1024
char test_buf[TEST_BUF_SZ];
char *test_buffer = &test_buf[0];
int buf_offset;

unsigned int cpu_family;
unsigned int cpu_model;

struct hsmp_testcase {
	char *desc;
	void (*func)(void);
};

int pr_test_start(const char *fmt, ...)
{
	va_list vargs;

	memset(test_buffer, 0, TEST_BUF_SZ);
	buf_offset = 0;

	buf_offset = sprintf(test_buffer, TEST_INDENT);

	va_start(vargs, fmt);
	buf_offset += vsprintf(test_buffer + buf_offset, fmt, vargs);
	va_end(vargs);

	return buf_offset;
}

int pr_test_note(const char *fmt, ...)
{
	va_list vargs;
	int len = 0;

	va_start(vargs, fmt);
	len = printf("%s- ", TEST_INDENT);
	len += vprintf(fmt, vargs);
	va_end(vargs);

	return len;
}

void pr_pass(void)
{
	total_tests++;
	passed_tests++;
	test_passed = 1;

	sprintf(test_buffer + buf_offset, "=> PASSED\n");
	printf("%s", test_buffer);
}

void pr_fail(int rc)
{
	total_tests++;
	failed_tests++;
	test_passed = 0;

	sprintf(test_buffer + buf_offset, "=> FAILED\n");
	printf("%s", test_buffer);

	if (rc)
		pr_test_note("Received unexpected error: %s\n", hsmp_strerror(rc, errno));
}

void pr_ebadmsg(void)
{
	total_tests++;
	ebadmsg_tests++;
	test_passed = 0;

	sprintf(test_buffer + buf_offset, "=> UNKNOWN\n");
	printf("%s", test_buffer);

	pr_test_note("Received EBADMSG, interface may not be supported by SMU.\n");
}

#define einval_error(_r, _e)	((_r) == -1 && (_e) == EINVAL)
#define eperm_error(_r, _e)	(!privileged_user && (_r) == -1 && (_e) == EPERM)
#define enotsup_error(_r, _e)	(privileged_user && (_r) == -1 && (_e) == ENOTSUP)
#define enomsg_error(_r, _e)	(privileged_user && unsupported_interface && \
				 (_r) == -1 && (_e) == ENOMSG)
#define ebadmsg_error(_r, _e)	(privileged_user && (_r) == -1 && (_e) == EBADMSG)

/*
 * The following routines for evaluating return codes from a test
 * caseare based on what is expected from a libhsmp call, the current
 * status of HSMP enablement, and if a particular interface is supported
 * in the current HSMP interface version.
 *
 * Any libhsmp call made by a non-root user should always return
 * errno == EPERM.
 *
 * For a priviliged user, the return code should be errno == ENOTSUP
 * if HSMP is disabled in BIOS (hsmp_disabled), the current call is
 * not supported for the interface version (unsupported_interface),
 * or this is running on a family 0x17 CPU and Family 0x17 support
 * is not enabled.
 */

void eval_for_failure(int rc)
{
	if (rc == 0) {
		/* expected failure */
		pr_fail(rc);
		pr_test_note("Expected test failure but received rc == 0\n");
		return;
	}

	if (privileged_user) {
		if (enotsup_error(rc, errno) &&
		    (hsmp_disabled || unsupported_interface || cpu_family < 0x19)) {
			pr_pass();
			pr_test_note("received expected ENOTSUP return code\n");
			return;
		} else if (einval_error(rc, errno)) {
			pr_pass();
			pr_test_note("received expected EINVAL return code\n");
			return;
		} else if (enomsg_error(rc, errno)) {
			pr_pass();
			pr_test_note("received expected ENOMSG return code\n");
			return;
		} else if (ebadmsg_error(rc, errno)) {
			pr_ebadmsg();
			return;
		}
	} else if (eperm_error(rc, errno)) {
		pr_pass();
		pr_test_note("received expected EPERM return code\n");
		return;
	}

	pr_fail(rc);
}

void eval_for_pass_results(int rc, int expected, int result)
{
	if (rc == 0) {
		if (expected != result)
			pr_fail(rc);
		else
			pr_pass();
		return;
	}

	if (privileged_user) {
		if (enotsup_error(rc, errno) &&
		    (hsmp_disabled || unsupported_interface || cpu_family < 0x19)) {
			pr_pass();
			pr_test_note("received expected ENOTSUP return code\n");
			return;
		} else if (enomsg_error(rc, errno)) {
			pr_pass();
			pr_test_note("received expected ENOMSG return code\n");
			return;
		} else if (ebadmsg_error(rc, errno)) {
			pr_ebadmsg();
			return;
		}
	} else if (eperm_error(rc, errno)) {
		pr_pass();
		pr_test_note("received expected EPERM return code\n");
		return;
	}

	pr_fail(rc);
}

void eval_for_pass(int rc)
{
	return eval_for_pass_results(rc, 0, 0);
}

/*
 * Attempt to SMU FW version to test for HSMP enablement. The results
 * of this are not logged as part of any tests.
 */
void test_hsmp_enablement(void)
{
	struct smu_fw_version fw;
	int rc;

	if (!privileged_user) {
		printf("Unable to determine SMU firmware HSMP enablement\n");
		return;
	}

	rc = hsmp_smu_fw_version(&fw);
	if (enotsup_error(rc, errno)) {
		printf("HSMP is not enabled in SMU firmware\n");
		hsmp_disabled = 1;
	}
}

void test_smu_fw_version(void)
{
	int rc;

	printf("Testing hsmp_smu_fw_version()...\n");

	pr_test_start("Testing with NULL SMU fw version pointer ");
	rc = hsmp_smu_fw_version(NULL);
	eval_for_failure(rc);

	pr_test_start("Testing with valid SMU fw version pointer ");
	rc = hsmp_smu_fw_version(&smu_fw);
	eval_for_pass(rc);

	if (test_passed && privileged_user &&
	    !(enotsup_error(rc, errno) && hsmp_disabled))
		pr_test_note("** SMU fw version %d.%d.%d\n", smu_fw.major,
			     smu_fw.minor, smu_fw.debug);
}

void test_interface_version(void)
{
	int rc;

	printf("Testing hsmp_interface_version()...\n");

	pr_test_start("Testing with NULL interface version pointer ");
	rc = hsmp_interface_version(NULL);
	eval_for_failure(rc);

	pr_test_start("Testing with valid interface version pointer ");
	rc = hsmp_interface_version(&interface_version);
	eval_for_pass(rc);

	if (test_passed && privileged_user &&
	    !(enotsup_error(rc, errno) && hsmp_disabled))
		pr_test_note("** HSMP Interface Version %d\n", interface_version);
}

void test_hsmp_ddr(void)
{
	u32 bw, u_bw, pct_bw;
	int rc;

	if (interface_version < 3)
		unsupported_interface = 1;

	printf("Testing %s hsmp_ddr_max_bandwidth()...\n",
	       unsupported_interface ? "unsupported" : "");

	pr_test_start("Testing with NULL max bandwidth pointer ");
	rc = hsmp_ddr_max_bandwidth(0, NULL);
	eval_for_failure(rc);

	pr_test_start("Testing with invalid socket_id ");
	rc = hsmp_ddr_max_bandwidth(-1, &bw);
	eval_for_failure(rc);

	pr_test_start("Testing with valid bandwidth pointer ");
	rc = hsmp_ddr_max_bandwidth(0, &bw);
	eval_for_pass(rc);

	if (test_passed && privileged_user &&
	    !(hsmp_disabled || unsupported_interface))
		pr_test_note("max bandwidth is %d\n", bw);

	printf("Testing %s hsmp_ddr_utilized_bandwidth()...\n",
	       unsupported_interface ? "unsupported" : "");

	pr_test_start("Testing with NULL utilized bandwidth pointer ");
	rc = hsmp_ddr_utilized_bandwidth(0, NULL);
	eval_for_failure(rc);

	pr_test_start("Testing with invalid socket_id ");
	rc = hsmp_ddr_utilized_bandwidth(-1, &u_bw);
	eval_for_failure(rc);

	pr_test_start("Testing with valid utilized bandwidth pointer ");
	rc = hsmp_ddr_utilized_bandwidth(0, &u_bw);
	eval_for_pass(rc);

	if (test_passed && privileged_user &&
	    !(hsmp_disabled || unsupported_interface))
		pr_test_note("utilized bandwidth is %d\n", u_bw);

	printf("Testing %s hsmp_ddr_utilized_percent()...\n",
	       unsupported_interface ? "unsupported" : "");

	pr_test_start("Testing with NULL utilized percent pointer ");
	rc = hsmp_ddr_utilized_percent(0, NULL);
	eval_for_failure(rc);

	pr_test_start("Testing with invalid socket_id ");
	rc = hsmp_ddr_utilized_percent(-1, &pct_bw);
	eval_for_failure(rc);

	pr_test_start("Testing with valid utilized pct bandwidth pointer ");
	rc = hsmp_ddr_utilized_percent(0, &pct_bw);
	eval_for_pass(rc);

	if (test_passed && privileged_user &&
	    !(hsmp_disabled || unsupported_interface))
		pr_test_note("utilized percent is %d\n", pct_bw);

	printf("Testing %s hsmp_ddr_bandwidths()...\n",
	       unsupported_interface ? "unsupported" : "");

	pr_test_start("Testing DDR bandwidths  ");
	rc = hsmp_ddr_bandwidths(0, &bw, &u_bw, &pct_bw);
	eval_for_pass(rc);

	if (test_passed && privileged_user &&
	    !(hsmp_disabled || unsupported_interface))
		pr_test_note("max bw:%d, utilized %d:, percent %d\n",
			     bw, u_bw, pct_bw);

	unsupported_interface = 0;
}

void test_hsmp_boost_limit(void)
{
	u32 set_limit, limit;
	int rc;

	printf("Testing hsmp_set_cpu_boost_limit()...\n");

	/* Per the PPR, setting boost limit causes the specified value to be clipped
	 * so testing with an invalid boost limit will be skipped until we can find
	 * a value considered invalid.
	 *
	 * Note that this applies to each test in this routine that attempts to set
	 * the limit to an invalid value (-1).
	 */
#if 0
	set_limit = -1;
	pr_test_start("Testing cpu boost limit with invalid boost limit -1 ");
	rc = hsmp_set_cpu_boost_limit(0, set_limit);
	eval_for_pass(rc);
#endif

	/* The set_limit value we use may need to be updated based upon
	 * the system we are testing on. Specifying a value greaer than
	 * the max value results in the boost limit being clipped to the max.
	 * This scenario would cause the tests to validate the limit is set
	 * to fail below.
	 *
	 * From the PPR; "Values written are constrained to the supported
	 * frequency range of the processor"
	 */
	set_limit = 0x7d0;
	pr_test_start("Testing with invalid CPU ");
	rc = hsmp_set_cpu_boost_limit(-1, set_limit);
	eval_for_failure(rc);

	pr_test_start("Testing setting CPU 0 boost limit to %x ", set_limit);
	rc = hsmp_set_cpu_boost_limit(0, set_limit);
	eval_for_pass(rc);

	printf("Testing hsmp_cpu_boost_limit()...\n");

	pr_test_start("Testing reading CPU 0 boost limit ");
	rc = hsmp_cpu_boost_limit(0, &limit);
	eval_for_pass_results(rc, limit, set_limit);

	if (privileged_user && !hsmp_disabled) {
		if (test_passed)
			pr_test_note("CPU 0 boost limit %d\n", limit);
		else
			pr_test_note("CPU boost limit returned incorrect value 0x%x instead of 0x%x\n",
				     limit, set_limit);
	}

	pr_test_start("Testing with NULL cpu boost limit pointer ");
	rc = hsmp_cpu_boost_limit(0, NULL);
	eval_for_failure(rc);

	pr_test_start("Testing reading CPU boost limit with invalid CPU ");
	rc = hsmp_cpu_boost_limit(-1, &limit);
	eval_for_failure(rc);

	printf("Testing hsmp_set_socket_boost_limit()...\n");

	pr_test_start("Testing setting socket boost limit with invalid socket id ");
	rc = hsmp_set_socket_boost_limit(-1, set_limit);
	eval_for_failure(rc);

	pr_test_start("Testing setting socket 0 boost limit to 0x%x ", set_limit);
	rc = hsmp_set_socket_boost_limit(0, set_limit);
	eval_for_pass(rc);

	printf("Testing hsmp_set_system_boost_limit()...\n");

	pr_test_start("Testing setting system boost limit a to 0x%x ", set_limit);
	rc = hsmp_set_system_boost_limit(set_limit);
	eval_for_pass(rc);
}

void test_hsmp_xgmi(void)
{
	enum hsmp_xgmi_width xgmi_width;
	int rc;

	printf("Testing hsmp_set_xgmi_width()...\n");

	pr_test_start("Testing hsmp_set_xgmi_auto() ");
	rc = hsmp_set_xgmi_auto();
	eval_for_pass(rc);

	pr_test_start("Testing xgmi width invalid value 5 ");
	rc = hsmp_set_xgmi_width(1, 5);
	eval_for_failure(rc);

	pr_test_start("Testing xgmi width min > max ");
	rc = hsmp_set_xgmi_width(HSMP_XGMI_WIDTH_X16, HSMP_XGMI_WIDTH_X8);
	eval_for_failure(rc);

	xgmi_width = HSMP_XGMI_WIDTH_X16;
	pr_test_start("Testing HSMP_XGMI_WIDTH_X16 (%d) ", xgmi_width);
	rc = hsmp_set_xgmi_width(xgmi_width, xgmi_width);
	eval_for_pass(rc);

	xgmi_width = HSMP_XGMI_WIDTH_X8;
	pr_test_start("Testing HSMP_XGMI_WIDTH_X8 (%d) ", xgmi_width);
	rc = hsmp_set_xgmi_width(xgmi_width, xgmi_width);
	eval_for_pass(rc);

	/* HSMP_XGMI_WIDTH_X2 only valid on Family 19h systems */
	if (cpu_family < 0x19)
		unsupported_interface = 1;

	xgmi_width = HSMP_XGMI_WIDTH_X2;
	pr_test_start("Testing %sHSMP_XGMI_WIDTH_X2 (%d) ",
		      unsupported_interface ? "unsupported " : "", xgmi_width);
	rc = hsmp_set_xgmi_width(xgmi_width, xgmi_width);
	if (unsupported_interface)
		eval_for_failure(rc);
	else
		eval_for_pass(rc);
	unsupported_interface = 0;
}

void test_hsmp_socket_power(void)
{
	u32 power;
	u32 limit;
	int rc;

	printf("Testing hsmp_socket_power()...\n");

	pr_test_start("Testing with NULL power pointer ");
	rc = hsmp_socket_power(0, NULL);
	eval_for_failure(rc);

	pr_test_start("Testing socket power with invalid socket_id ");
	rc = hsmp_socket_power(-1, &power);
	eval_for_failure(rc);

	pr_test_start("Testing socket power with socket id 0 ");
	rc = hsmp_socket_power(0, &power);
	eval_for_pass(rc);

	if (test_passed && privileged_user && !hsmp_disabled)
		pr_test_note("Socket power 0x%x\n", power);

	printf("Testing hsmp_set_socket_power_limit()...\n");
	limit = 120000;

	/*
	 * Per the PPR, attempting to pass an invalid limit value may not be
	 * possible.
	 *
	 * "The value written is clipped to the maximum cTDP range for the
	 * processor. NOTE: there is a limit on the minimum power that the
	 * processor can operate at; no further socket power reduction
	 * occurs if the socket power limit is set below that limit"
	 *
	 * We can add this test back in once we can identify an invalid
	 * power limit to use.
	 */
#if 0
	pr_test_start("Testing socket power limit with invalid limit -1 ");
	rc = hsmp_set_socket_power_limit(0, -1);
	eval_for_pass(rc);
#endif

	pr_test_start("Testing socket power limit with invalid socket id ");
	rc = hsmp_set_socket_power_limit(-1, limit);
	eval_for_failure(rc);

	pr_test_start("Testing set socket power limit to %d for socket 0 ", limit);
	rc = hsmp_set_socket_power_limit(0, limit);
	eval_for_pass(rc);

	printf("Testing hsmp_socket_power_limit()...\n");

	pr_test_start("Testing socket power limit for socket 0 ");
	rc = hsmp_socket_power_limit(0, &power);
	eval_for_pass_results(rc, power, limit);

	if (privileged_user && !hsmp_disabled) {
		if (test_passed)
			pr_test_note("Socket power reported %d\n", power);
		else
			pr_test_note("Socket power returned %d instead of %d\n",
				     power, limit);
	}

	pr_test_start("Testing with NULL socket power limit pointer ");
	rc = hsmp_socket_power_limit(0, NULL);
	eval_for_failure(rc);

	pr_test_start("Testing socket power limit with invalid socket id ");
	rc = hsmp_socket_power_limit(-1, &limit);
	eval_for_failure(rc);

	printf("Testing hsmp_socket_max_power_limit()...\n");

	pr_test_start("Testing with NULL max power limit pointer ");
	rc = hsmp_socket_max_power_limit(0, NULL);
	eval_for_failure(rc);

	pr_test_start("Testing max socket power limit with invalid socket id ");
	rc = hsmp_socket_max_power_limit(-1, &limit);
	eval_for_failure(rc);

	pr_test_start("Testing socket power max limit for socket 0 ");
	rc = hsmp_socket_max_power_limit(0, &limit);
	eval_for_pass(rc);

	if (test_passed && privileged_user && !hsmp_disabled)
		pr_test_note("socket 0 max limit %d\n", limit);
}

void test_proc_hot_status(void)
{
	int proc_hot;
	int rc;

	printf("Testing hsmp_proc_hot_status()...\n");

	pr_test_start("Testing with NULL proc hot pointer ");
	rc = hsmp_proc_hot_status(0, NULL);
	eval_for_failure(rc);

	pr_test_start("Testing proc hot with invalid socket id ");
	rc = hsmp_proc_hot_status(-1, &proc_hot);
	eval_for_failure(rc);

	pr_test_start("Testing proc hot for socket 0 ");
	rc = hsmp_proc_hot_status(0, &proc_hot);
	eval_for_pass(rc);

	if (test_passed && privileged_user && !hsmp_disabled)
		pr_test_note("proc hot = %d\n");
}

void test_df_pstate(void)
{
	enum hsmp_df_pstate df_pstate;
	int rc;

	printf("Testing hsmp_set_data_fabric_pstate()...\n");

	df_pstate = 42;

	pr_test_start("Testing DF pstate with invalid pstate %d ", df_pstate);
	rc = hsmp_set_data_fabric_pstate(0, df_pstate);
	eval_for_failure(rc);

	df_pstate = HSMP_DF_PSTATE_AUTO;

	pr_test_start("Testing DF pstate with invalid scoket_id ");
	rc = hsmp_set_data_fabric_pstate(-1, df_pstate);
	eval_for_failure(rc);

	pr_test_start("Testing DF pstate HSMP_DF_PSTATE_AUTO (%d) ", df_pstate);
	rc = hsmp_set_data_fabric_pstate(0, df_pstate);
	eval_for_pass(rc);

	df_pstate = HSMP_DF_PSTATE_0;
	pr_test_start("Testing DF pstate HSMP_DF_PSTATE_0 (%d) ", df_pstate);
	rc = hsmp_set_data_fabric_pstate(0, df_pstate);
	eval_for_pass(rc);

	df_pstate = HSMP_DF_PSTATE_1;
	pr_test_start("Testing DF pstate HSMP_DF_PSTATE_1 (%d) ", df_pstate);
	rc = hsmp_set_data_fabric_pstate(0, df_pstate);
	eval_for_pass(rc);

	df_pstate = HSMP_DF_PSTATE_2;
	pr_test_start("Testing DF pstate HSMP_DF_PSTATE_2 (%d) ", df_pstate);
	rc = hsmp_set_data_fabric_pstate(0, df_pstate);
	eval_for_pass(rc);

	df_pstate = HSMP_DF_PSTATE_3;
	pr_test_start("Testing DF pstate HSMP_DF_PSTATE_3 (%d) ", df_pstate);
	rc = hsmp_set_data_fabric_pstate(0, df_pstate);
	eval_for_pass(rc);
}

void test_fabric_clocks(void)
{
	int mem_clock, df_clock;
	int rc;

	printf("Testing hsmp_memory_clock()...\n");

	pr_test_start("Testing with NULL memory clock pointer ");
	rc = hsmp_memory_clock(0, NULL);
	eval_for_failure(rc);

	pr_test_start("Testing memory clock with invalid socket id ");
	rc = hsmp_memory_clock(-1, &mem_clock);
	eval_for_failure(rc);

	pr_test_start("Testing memory clock ");
	rc = hsmp_memory_clock(0, &mem_clock);
	eval_for_pass(rc);

	if (test_passed && privileged_user && !hsmp_disabled)
		pr_test_note("memory clock %d\n", mem_clock);

	printf("Testing hsmp_data_fabric_clock()...\n");

	pr_test_start("Testing with NULL data fabric clock pointer ");
	rc = hsmp_data_fabric_clock(0, NULL);
	eval_for_failure(rc);

	pr_test_start("Testing data fabroic clock with invalid socket id ");
	rc = hsmp_data_fabric_clock(-1, &df_clock);
	eval_for_failure(rc);

	pr_test_start("Testing data fabric clock ");
	rc = hsmp_data_fabric_clock(0, &df_clock);
	eval_for_pass(rc);

	if (test_passed && privileged_user && !hsmp_disabled)
		pr_test_note("data fabric clock %d\n", df_clock);

	printf("Testing hsmp_fabric_clocks()...\n");

	pr_test_start("Testing fabric clocks ");
	rc = hsmp_fabric_clocks(0, &df_clock, &mem_clock);
	eval_for_pass(rc);

	if (test_passed && privileged_user && !hsmp_disabled)
		pr_test_note("df clock %d, memory clock %d\n", df_clock, mem_clock);
}

void test_core_clock_max(void)
{
	u32 clock;
	int rc;

	printf("Testing hsmp_core_clock_max_frequency()...\n");

	pr_test_start("Testing with NULL core clock pointer ");
	rc = hsmp_core_clock_max_frequency(0, NULL);
	eval_for_failure(rc);

	pr_test_start("Reading core clock max frequency with invalid socket id ");
	rc = hsmp_core_clock_max_frequency(-1, &clock);
	eval_for_failure(rc);

	pr_test_start("Reading core clock max frequency for socket 0 ");
	rc = hsmp_core_clock_max_frequency(0, &clock);
	eval_for_pass(rc);

	if (test_passed && privileged_user && !hsmp_disabled)
		pr_test_note("max frequency clock is %d\n", clock);
}

void test_c0_residency(void)
{
	u32 residency;
	int rc;

	printf("Testing hsmp_c0_residency()...\n");

	pr_test_start("Testing with NULL residency pointer ");
	rc = hsmp_c0_residency(0, NULL);
	eval_for_failure(rc);

	pr_test_start("Reading C0 residency with invalid socket id ");
	rc = hsmp_c0_residency(-1, &residency);
	eval_for_failure(rc);

	pr_test_start("Reading C0 residency of socket 0 ");
	rc = hsmp_c0_residency(0, &residency);
	eval_for_pass(rc);
	if (test_passed && privileged_user && !hsmp_disabled)
		pr_test_note("C0 residency is %d\n", residency);
}

void test_nbio_pstate(void)
{
	enum hsmp_nbio_pstate pstate;
	int rc;

	if (interface_version < 2)
		unsupported_interface = 1;

	printf("Testing %s hsmp_set_nbio_pstate()...\n",
	       unsupported_interface ? "unsupported" : "");

	pstate = 5;
	pr_test_start("Testing NBIO pstate for socket 0 with invalid pstate %x ", pstate);
	rc = hsmp_set_nbio_pstate(0, pstate);
	eval_for_failure(rc);

	pstate = HSMP_NBIO_PSTATE_AUTO;
	pr_test_start("Testing HSMP_NBIO_PSTATE_AUTO pstate (%d) ", pstate);
	rc = hsmp_set_nbio_pstate(0, pstate);
	eval_for_pass(rc);

	pstate = HSMP_NBIO_PSTATE_P0;
	pr_test_start("Testing HSMP_NBIO_PSTATE_P0 pstate (%d) ", pstate);
	rc = hsmp_set_nbio_pstate(0, pstate);
	eval_for_pass(rc);

	unsupported_interface = 0;
}

void test_hsmp_strerror(void)
{
	char *hsmp_errstring;

	printf("Testing hsmp_errstring()...\n");

	pr_test_start("Testing HSMP_ERR_INVALID_MSG_ID ");
	hsmp_errstring = hsmp_strerror(HSMP_ERR_INVALID_MSG_ID, 0);
	if (strncmp(hsmp_errstring, "Invalid HSMP message ID", 23)) {
		pr_fail(0);
		pr_test_note("Incorrect string returned: \"%s\"\n", hsmp_errstring);
	} else {
		pr_pass();
	}

	pr_test_start("Testing HSMP_ERR_INVALID_ARG ");
	hsmp_errstring = hsmp_strerror(HSMP_ERR_INVALID_ARG, 0);
	if (strncmp(hsmp_errstring, "Invalid HSMP argument", 21)) {
		pr_fail(0);
		pr_test_note("Incorrect string returned: \"%s\"\n", hsmp_errstring);
	} else {
		pr_pass();
	}

	pr_test_start("Testing \"Success\", rc = 0 ");
	hsmp_errstring = hsmp_strerror(0, 0);
	if (strncmp(hsmp_errstring, "Success", 7)) {
		pr_fail(0);
		pr_test_note("Incorrect string returned: \"%s\"\n", hsmp_errstring);
	} else {
		pr_pass();
	}

	pr_test_start("Testing EINVAL, rc = -1 ");
	hsmp_errstring = hsmp_strerror(-1, EINVAL);
	if (strncmp(hsmp_errstring, "Invalid argument", 16)) {
		pr_fail(0);
		pr_test_note("Incorrect string returned: \"%s\"\n", hsmp_errstring);
	} else {
		pr_pass();
	}
}

void get_cpu_info(void)
{
	unsigned int eax, ebx, ecx, edx;

        __cpuid(1, eax, ebx, ecx, edx);
        cpu_family = (eax >> 8) & 0xf;
        cpu_model = (eax >> 4) & 0xf;

	if (cpu_family == 0xf)
		cpu_family += (eax >> 20) & 0xff;

	if (cpu_family >= 6)
		cpu_model += ((eax >> 16) & 0xf) << 4;
}

void print_results(void)
{
	printf("\n\n");
	printf("Test Results:\n");
	printf("================\n");
	printf("Total Tests:  %d\n", total_tests);
	printf("Passed:       %d\n", passed_tests);
	printf("Failed:       %d\n", failed_tests);
	printf("EBADMSG:      %d\n", ebadmsg_tests);
}

struct hsmp_testcase hsmp_testcases[] = {
	{ "SMU Version",
	  test_smu_fw_version,
	},
	{ "Interface Version",
	  test_interface_version,
	},
	{ "Socket Power",
	  test_hsmp_socket_power,
	},
	{ "Boost Limits",
	  test_hsmp_boost_limit,
	},
	{ "Proc HOT Status",
	  test_proc_hot_status,
	},
	{ "XGMI Link Width",
	  test_hsmp_xgmi,
	},
	{ "Data Fabric P-state",
	  test_df_pstate,
	},
	{ "Fabric Clocks",
	  test_fabric_clocks,
	},
	{ "Core Clock Limit",
	  test_core_clock_max,
	},
	{ "C0 Residency",
	  test_c0_residency,
	},
	{ "NBIO P-state",
	  test_nbio_pstate,
	},
	{ "DDR Bandwidth",
	  test_hsmp_ddr,
	},
	{ "HSMP strerror",
	  test_hsmp_strerror,
	},
};

int max_testcase = 12;

void usage(void)
{
	int i;

	printf("hsmp_test [-v] [-f <test function>]\n");
	printf("Available test functions\n");

	printf("    Index    Description\n");
	for (i = 0; i <= max_testcase; i++)
		printf("    %5d    %s\n", i, hsmp_testcases[i].desc);
}

int main(int argc, char **argv)
{
	int test_index;
	int do_seteuid;
	uid_t euid;
	char opt;
	int rc;

	test_index = -1;
	do_seteuid = 0;

	while ((opt = getopt(argc, argv, "ef:v")) != -1) {
		switch (opt) {
		case 'e':
			do_seteuid = 1;
			break;
		case 'f':
			test_index = strtol(optarg, NULL, 0);

			if (test_index > max_testcase) {
				printf("Invalid test case %s specified\n",
				       optarg);
				usage();
				return -1;
			}

			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
			return -1;
		}
	}

	get_cpu_info();
	printf("Testing on CPU Family %xh, Model %xh\n", cpu_family, cpu_model);

	euid = geteuid();
	privileged_user = euid ? 0 : 1;
	printf("Running test as %sprivileged user (euid %d)\n",
	       euid ? "non-" : "", euid);

	test_hsmp_enablement();

	/* pretty print blank line */
	printf("\n");

	if (test_index != -1) {
		hsmp_testcases[test_index].func();
		print_results();
		return 0;
	}

	test_smu_fw_version();
	test_interface_version();
	test_hsmp_socket_power();
	test_hsmp_boost_limit();
	test_hsmp_xgmi();

	if (do_seteuid) {
		printf("*** Setting euid to 0 *** ");
		rc = seteuid(0);
		if (rc) {
			pr_fail(rc);
		} else {
			pr_pass();
			privileged_user = 1;
		}

		/* After switching to privileged user, we need to
		 * re-test for hsmp enablement.
		 */
		test_hsmp_enablement();
	}

	test_proc_hot_status();
	test_df_pstate();
	test_fabric_clocks();
	test_core_clock_max();
	test_c0_residency();

	if (do_seteuid) {
		printf("*** Reverting back to euid %d *** ", euid);
		rc = seteuid(euid);
		if (rc)
			pr_fail(rc);
		else
			pr_pass();

		if (euid != 0)
			privileged_user = 0;
	}

	test_nbio_pstate();
	test_hsmp_ddr();
	test_hsmp_strerror();

	print_results();
	return 0;
}
