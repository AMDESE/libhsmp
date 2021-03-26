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
#include "libhsmp.c"
#endif

struct smu_fw_version smu_fw;
int interface_version;

int total_tests;
int passed_tests;
int failed_tests;

int verbose = 0;
int privileged_user = 0;

unsigned int cpu_family;
unsigned int cpu_model;

struct hsmp_testcase {
	char *desc;
	void (*func)(void);
};

int pr_verbose(const char *fmt, ...)
{
	va_list vargs;
	int len = 0;

	va_start(vargs, fmt);
	if (verbose)
		len = vprintf(fmt, vargs);
	va_end(vargs);

	return len;
}

int _pr_fmt(const char *fmt, va_list vargs)
{
	int len;

	len = printf("    ");
	len += vprintf(fmt, vargs);

	return len;
}

int pr_fmt(const char *fmt, ...)
{
	va_list vargs;
	int len;

	va_start(vargs, fmt);
	len = _pr_fmt(fmt, vargs);
	va_end(vargs);

	return len;
}

int pr_pass(const char *fmt, ...)
{
	va_list vargs;
	int len = 0;

	total_tests++;
	passed_tests++;

	va_start(vargs, fmt);
	if (verbose)
		len = _pr_fmt(fmt, vargs);
	va_end(vargs);

	return len;
}

int pr_fail(const char *fmt, ...)
{
	va_list vargs;
	int len;

	total_tests++;
	failed_tests++;

	va_start(vargs, fmt);
	len = _pr_fmt(fmt, vargs);
	va_end(vargs);

	return len;
}

#define einval_error(_r, _e)	((_r) == -1 && (_e) == EINVAL)
#define eperm_error(_r, _e)	(!privileged_user && (_r) == -1 && (_e) == EPERM)
#define enotsup_error(_r, _e)	(privileged_user && (_r) == -1 && (_e) == ENOTSUP)

/*
 * For normal test scenarios an rc != 0 indicates failure.
 * For our purposes non-privileged users should get a EPERM
 * errno returned in addition to a valid rc value. This
 * routine validates these conditions.
 */
int unprivileged_fail(int rc, int expected_fail)
{
	if (eperm_error(rc, errno))
		return 0;

	if (expected_fail && rc == 0)
		return 0;

	return 1;
}

int privileged_fail(int rc)
{
	if (rc == 0 || enotsup_error(rc, errno))
		return 0;

	return 1;
}

int test_failed(int rc)
{
	if (privileged_user)
		return privileged_fail(rc);

	return unprivileged_fail(rc, 0);
}

/*
 * Same as test_failed() except that this is called for tests
 * where we expect the test to fail, i.e. passing an invalid
 * value.
 */
int test_expected_failure(int rc)
{
	if (privileged_user)
		return 0;

	return unprivileged_fail(rc, 1);
}

/*
 * Validate the result when a null value pointer is passed as
 * an argument to the hsmp interface.
 */
void null_pointer_result(int rc, const char *name)
{
	if (einval_error(rc, errno) || eperm_error(rc, errno) ||
	    enotsup_error(rc, errno)) {
		pr_pass("Testing with NULL %s pointer passed\n", name);
	} else {
		pr_fail("Testing with NULL %s pointer failed (%d), %s\n",
			name, rc, hsmp_strerror(rc, errno));
	}
}

void test_smu_fw_version(void)
{
	int rc;

	printf("Testing hsmp_smu_fw_version()...\n");

	/* NULL pointer, should fail */
	rc = hsmp_smu_fw_version(NULL);
	null_pointer_result(rc, "smu fw version");

	/* Test with valid pointer */
	rc = hsmp_smu_fw_version(&smu_fw);
	if (test_failed(rc)) {
		pr_fail("Testing with valid pointer failed, %s\n",
			hsmp_strerror(rc, errno));
	} else {
		if (privileged_user) {
			pr_pass("Testing with valid pointer passed\n");
			pr_fmt("** HSMP version %d.%d.%d\n", smu_fw.major,
			       smu_fw.minor, smu_fw.debug);
		} else {
			pr_pass("Testing with valid pointer passed\n");
			pr_fmt("** HSMP version ??.??.?? (unprivileged user)\n");
		}
	}

}

void test_interface_version(void)
{
	int rc;

	printf("Testing hsmp_interface_version()...\n");

	/* NULL pointer, should fail */
	rc = hsmp_interface_version(NULL);
	null_pointer_result(rc, "interface version");

	rc = hsmp_interface_version(&interface_version);
	if (test_failed(rc)) {
		pr_fail("Testing with valid pointer failed, %s\n",
			hsmp_strerror(rc, errno));
	} else {
		if (privileged_user) {
			pr_pass("Testing with valid pointer passed\n");
			pr_fmt("** HSMP Interface Version %d\n", interface_version);
		} else {
			pr_pass("Testing with valid pointer passed\n");
			pr_fmt("** HSMP Interface Version ?? (unprivileged user)\n");
		}
	}

}

void test_hsmp_ddr_supported(void)
{
	u32 bw;
	int rc;

	printf("Testing hsmp_ddr_max_bandwidth()...\n");

	/* NULL pointer, should fail */
	rc = hsmp_ddr_max_bandwidth(NULL);
	null_pointer_result(rc, "max bandwidth");

	rc = hsmp_ddr_max_bandwidth(&bw);
	if (test_failed(rc)) {
		pr_fail("Failed to get max bandwidth, %s\n", hsmp_strerror(rc, errno));
	} else {
		if (privileged_user)
			pr_pass("Getting max bandwidth passed, bandwidth is %d\n", bw);
		else
			pr_pass("Testing max bandwidth passed\n");
	}

	printf("Testing hsmp_ddr_utilized_bandwidth()...\n");

	/* NULL pointer, should fail */
	rc = hsmp_ddr_utilized_bandwidth(NULL);
	null_pointer_result(rc, "utilized bandwidth");

	rc = hsmp_ddr_utilized_bandwidth(&bw);
	if (test_failed(rc)) {
		pr_fail("Failed to get utilized bandwidth, %s\n", hsmp_strerror(rc, errno));
	} else {
		if (privileged_user)
			pr_pass("Getting utilized bandwidth passed, bandwidth is %d\n", bw);
		else
			pr_pass("Testing utilized bandwidth passed\n");
	}

	printf("Testing hsmp_ddr_utilized_percent()...\n");

	/* NULL pointer, should fail */
	rc = hsmp_ddr_utilized_percent(NULL);
	null_pointer_result(rc, "utilized percent");

	rc = hsmp_ddr_utilized_percent(&bw);
	if (test_failed(rc)) {
		pr_fail("Failed to get utilized percent bandwidth, %s\n", hsmp_strerror(rc, errno));
	} else {
		if (privileged_user)
			pr_pass("Getting utilized percent bandwidth passed, percent is %d\n", bw);
		else
			pr_pass("Testing utilized percent bandwidth passed\n");
	}
}

void test_hsmp_ddr_unsupported(void)
{
	u32 bw;
	int rc;

	printf("Testing unsupported hsmp_ddr_max_bandwidth()...\n");

	rc = hsmp_ddr_max_bandwidth(NULL);
	null_pointer_result(rc, "max bandwidth");

	rc = hsmp_ddr_max_bandwidth(&bw);
	if (!test_failed(rc) || enotsup_error(rc, errno))
		pr_pass("Testing max bandwidth passed\n");
	else
		pr_fail("Testing max bandwidth failed\n");

	printf("Testing unsupported hsmp_ddr_utilized_bandwidth()...\n");

	rc = hsmp_ddr_utilized_bandwidth(NULL);
	null_pointer_result(rc, "utilized bandwidth");

	rc = hsmp_ddr_utilized_bandwidth(&bw);
	if (!test_failed(rc) || enotsup_error(rc, errno))
		pr_pass("Testing utilized bandwidth passed\n");
	else
		pr_fail("Testing utilized bandwidth failed\n");

	printf("Testing unsupported hsmp_ddr_utilized_percent()...\n");

	rc = hsmp_ddr_utilized_percent(NULL);
	null_pointer_result(rc, "utilized percent");

	rc = hsmp_ddr_utilized_percent(&bw);
	if (!test_failed(rc) || enotsup_error(rc, errno))
		pr_pass("Testing utilized percent bandwidth passed\n");
	else
		pr_fail("Testing utilized percent bandwidth failed\n");
}

void test_hsmp_ddr(void)
{
	if (interface_version < 3)
		return test_hsmp_ddr_unsupported();

	return test_hsmp_ddr_supported();
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
	rc = hsmp_set_cpu_boost_limit(0, set_limit);
	if (rc)
		pr_pass("Testing with invalid boost limit (0x%x) passed\n", set_limit);
	else
		pr_fail("Testing with invalid boost limit failed\n");
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
	rc = hsmp_set_cpu_boost_limit(-1, set_limit);
	if (test_expected_failure(rc))
		pr_fail("Testing with invalid socket id failed\n");
	else
		pr_pass("Testing with invalid socket id passed\n");

	rc = hsmp_set_cpu_boost_limit(0, set_limit);
	if (test_failed(rc)) {
		pr_fail("Setting CPU 0 boost limit to 0x%x failed, %s\n",
			set_limit, hsmp_strerror(rc, errno));
	} else {
		if (privileged_user)
			pr_pass("Setting CPU 0 boost limit to 0x%x passed\n", set_limit);
		else
			pr_pass("Testing setting CPU boost limit passed\n");
	}

	printf("Testing hsmp_cpu_boost_limit()...\n");
	rc = hsmp_cpu_boost_limit(0, &limit);
	if (test_failed(rc)) {
		pr_fail("CPU boost limit for cpu 0 failed, %s\n", hsmp_strerror(rc, errno));
	} else {
		if (!eperm_error(rc, errno) && !enotsup_error(rc, errno)) {
			if (limit != set_limit)
				pr_fail("CPU boost limit returned incorrect value 0x%x instead of 0x%x\n",
					limit, set_limit);
			else
				pr_pass("CPU boost limit passed, boost limit 0x%x\n", limit);
		} else {
			pr_pass("Testing cpu boost limit passed\n");
		}
	}

	rc = hsmp_cpu_boost_limit(0, NULL);
	null_pointer_result(rc, "cpu boost limit");

	rc = hsmp_cpu_boost_limit(-1, &limit);
	if (test_expected_failure(rc))
		pr_fail("Testing with invalid socket id failed\n");
	else
		pr_pass("Testing with invalid socket id passed\n");

	printf("Testing hsmp_set_socket_boost_limit()...\n");
#if 0
	rc = hsmp_set_socket_boost_limit(0, -1);
	if (test_failed(rc))
		pr_fail("Testing with invalid boost limit failed\n");
	else
		pr_pass("Testing with invalid boost limit passed\n");
#endif

	rc = hsmp_set_socket_boost_limit(-1, set_limit);
	if (test_expected_failure(rc))
		pr_fail("Testing with invalid socket id failed\n");
	else
		pr_pass("Testing with invalid socket id passed\n");

	rc = hsmp_set_socket_boost_limit(0, set_limit);
	if (test_failed(rc)) {
		pr_fail("Set socket boost limit for cpu 0 failed, %s\n", hsmp_strerror(rc, errno));
	} else {
		if (!eperm_error(rc, errno) && !enotsup_error(rc, errno)) {
			hsmp_cpu_boost_limit(0, &limit);
			if (limit != set_limit)
				pr_fail("Incorrect value returned for CPU 0 in Socket 0\n");
			else
				pr_pass("Set socket boost limit to 0x%x passed\n", set_limit);
		} else {
			pr_pass("Testing socket boost limit passed\n");
		}
	}

	printf("Testing hsmp_set_system_boost_limit()...\n");
#if 0
	rc = hsmp_set_system_boost_limit(-1);
	if (test_failed(rc))
		pr_fail("Testing with invalid boost limit failed\n");
	else
		pr_pass("Testing with invalid boost limit passed\n");
#endif

	rc = hsmp_set_system_boost_limit(set_limit);
	if (test_failed(rc)) {
		pr_fail("Set system boost limit failed, %s\n", hsmp_strerror(rc, errno));
	} else {
		if (!eperm_error(rc, errno) && !enotsup_error(rc, errno)) {
			hsmp_cpu_boost_limit(0, &limit);
			if (limit != set_limit)
				pr_fail("Incorrect value returned for CPU 0 in Socket 0\n");
			else
				pr_pass("Set system boost limit to 0x%x passed\n", set_limit);
		} else {
			pr_pass("Testing system boost limit passed\n");
		}
	}
}

void test_hsmp_xgmi(void)
{
	enum hsmp_xgmi_pstate xgmi_pstate;
	int rc;

	printf("Testing hsmp_set_xgmi_pstate()...\n");
	rc = hsmp_set_xgmi_pstate(5);
	if (test_expected_failure(rc))
		pr_fail("Testing xgmi pstate invalid value 5 failed\n");
	else
		pr_pass("Testing xgmi pstate invalid value 5 passed\n");

	xgmi_pstate = HSMP_XGMI_PSTATE_DYNAMIC;
	rc = hsmp_set_xgmi_pstate(xgmi_pstate);
	if (test_failed(rc))
		pr_fail("Testing HSMP_XGMI_PSTATE_DYNAMIC (%d) failed, %s\n",
			xgmi_pstate, hsmp_strerror(rc, errno));
	else
		pr_pass("Testing HSMP_XGMI_PSTATE_DYNAMIC (%d) passed\n", xgmi_pstate);

	xgmi_pstate = HSMP_XGMI_PSTATE_X2;
	rc = hsmp_set_xgmi_pstate(xgmi_pstate);
	if (test_failed(rc))
		pr_fail("Testing HSMP_XGMI_PSTATE_X2 (%d) failed, %s\n",
			xgmi_pstate, hsmp_strerror(rc, errno));
	else
		pr_pass("Testing HSMP_XGMI_PSTATE_X2 (%d) passed\n", xgmi_pstate);

	xgmi_pstate = HSMP_XGMI_PSTATE_X8;
	rc = hsmp_set_xgmi_pstate(xgmi_pstate);
	if (test_failed(rc))
		pr_fail("Testing HSMP_XGMI_PSTATE_X8 (%d) failed, %s\n",
			xgmi_pstate, hsmp_strerror(rc, errno));
	else
		pr_pass("Testing HSMP_XGMI_PSTATE_X8 (%d) passed\n", xgmi_pstate);

	xgmi_pstate = HSMP_XGMI_PSTATE_X16;
	rc = hsmp_set_xgmi_pstate(xgmi_pstate);
	/* HSMP_XGMI_PSTATE_X16 only valid on Family 19h systems */
	if (cpu_family == 0x19) {
		if (test_failed(rc))
			pr_fail("Testing HSMP_XGMI_PSTATE_X16 (%d) for CPU Family %xh failed, %s\n",
				xgmi_pstate, cpu_family, hsmp_strerror(rc, errno));
		else
			pr_pass("Testing HSMP_XGMI_PSTATE_X16 (%d) for CPU Family %xh passed\n",
				xgmi_pstate, cpu_family);
	} else {
		if (test_expected_failure(rc))
			pr_fail("Testing HSMP_XGMI_PSTATE_X16 (%d) for CPU Family %xh failed, %s\n",
				xgmi_pstate, cpu_family, hsmp_strerror(rc, errno));
		else
			pr_pass("Testing HSMP_XGMI_PSTATE_X16 (%d) for CPU Family %xh passed\n",
				xgmi_pstate, cpu_family);
	}
}

void test_hsmp_socket_power(void)
{
	u32 power;
	u32 limit;
	int rc;

	printf("Testing hsmp_socket_power()...\n");
	rc = hsmp_socket_power(0, NULL);
	null_pointer_result(rc, "power pointer");

	rc = hsmp_socket_power(-1, &power);
	if (test_expected_failure(rc))
		pr_fail("Testing with invalid socket_id failed, %s\n", hsmp_strerror(rc, errno));
	else
		pr_pass("Testing with invalid socket_id passed\n");

	rc = hsmp_socket_power(0, &power);
	if (test_failed(rc)) {
		pr_fail("Testing socket power failed, %s\n", hsmp_strerror(rc, errno));
	} else {
		if (privileged_user)
			pr_pass("Testing socket power passed, power: 0x%x\n", power);
		else
			pr_pass("Testing socket power passed\n");
	}

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
	rc = hsmp_set_socket_power_limit(0, -1);
	if (test_failed(rc))
		pr_fail("Testing with invalid power limit failed, %s\n", hsmp_strerror(rc, errno));
	else
		pr_pass("Testing with invalid power limit passed\n");
#endif

	rc = hsmp_set_socket_power_limit(-1, limit);
	if (test_expected_failure(rc))
		pr_fail("Testing with invalid socket_id failed, %s\n", hsmp_strerror(rc, errno));
	else
		pr_pass("Testing with invalid socket_id passed\n");

	rc = hsmp_set_socket_power_limit(0, limit);
	if (test_failed(rc))
		pr_fail("Testing set socket power limit failed, %s\n", hsmp_strerror(rc, errno));
	else
		pr_pass("Testing set socket power limit passed\n");

	printf("Testing hsmp_socket_power_limit()...\n");
	rc = hsmp_socket_power_limit(0, &power);
	if (test_failed(rc)) {
		pr_fail("Testing socket power limit failed, %s\n", hsmp_strerror(rc, errno));
	} else {
		if (!eperm_error(rc, errno) && !enotsup_error(rc, errno)) {
			if (power != limit)
				pr_fail("socket power limit returned %d instead of %d\n",
					power, limit);
			else
				pr_pass("Testing socket power limit passed, limit = %d\n",
					power);
		} else {
			pr_pass("Testing socket power limit passed\n");
		}
	}

	rc = hsmp_socket_power_limit(0, NULL);
	null_pointer_result(rc, "socket power limit");

	rc = hsmp_socket_power_limit(-1, &limit);
	if (test_expected_failure(rc))
		pr_fail("Testing with invalid socket_id failed, %s\n", hsmp_strerror(rc, errno));
	else
		pr_pass("Testing with invalid socket_id passed\n");

	printf("Testing hsmp_socket_max_power_limit()...\n");
	rc = hsmp_socket_max_power_limit(0, NULL);
	null_pointer_result(rc, "max power limit");

	rc = hsmp_socket_max_power_limit(-1, &limit);
	if (test_expected_failure(rc))
		pr_fail("Testing with invalid socket_id failed, %s\n", hsmp_strerror(rc, errno));
	else
		pr_pass("Testing with invalid socket_id passed\n");

	rc = hsmp_socket_max_power_limit(0, &limit);
	if (test_failed(rc)) {
		pr_fail("Testing set socket power limit failed, %s\n", hsmp_strerror(rc, errno));
	} else {
		if (privileged_user)
			pr_pass("Testing set socket power limit passed, max limit %d\n", limit);
		else
			pr_pass("Testing set socket power limit passed\n");
	}

}

void test_proc_hot_status(void)
{
	int proc_hot;
	int rc;

	printf("Testing hsmp_proc_hot_status()...\n");

	rc = hsmp_proc_hot_status(0, NULL);
	null_pointer_result(rc, "proc hot");

	rc = hsmp_proc_hot_status(-1, &proc_hot);
	if (test_expected_failure(rc))
		pr_fail("Testing with invalid socket id failed\n");
	else
		pr_pass("Testing with invalid socket id passed\n");

	rc = hsmp_proc_hot_status(0, &proc_hot);
	if (test_failed(rc))
		pr_fail("Getting proc hot status failed, %s\n", hsmp_strerror(rc, errno));
	else
		pr_pass("Getting proc hot status passed, proc hot = %d\n", proc_hot);
}

void test_df_pstate(void)
{
	enum hsmp_df_pstate df_pstate;
	int rc;

	printf("Testing hsmp_set_data_fabric_pstate()...\n");

	df_pstate = 42;
	rc = hsmp_set_data_fabric_pstate(df_pstate);
	if (test_expected_failure(rc))
		pr_fail("Testing invalid df pstate value %d failed\n", df_pstate);
	else
		pr_pass("Testing invalid df pstate value %d passed\n", df_pstate);

	df_pstate = HSMP_DF_PSTATE_AUTO;
	rc = hsmp_set_data_fabric_pstate(df_pstate);
	if (test_failed(rc))
		pr_fail("Testing DF pstate HSMP_DF_PSTATE_AUTO (%d) failed, %s\n",
			df_pstate, hsmp_strerror(rc, errno));
	else
		pr_pass("Testing DF pstate HSMP_DF_PSTATE_AUTO (%d) passed\n", df_pstate);

	df_pstate = HSMP_DF_PSTATE_0;
	rc = hsmp_set_data_fabric_pstate(df_pstate);
	if (test_failed(rc))
		pr_fail("Testing DF pstate HSMP_DF_PSTATE_0 (%d) failed, %s\n",
			df_pstate, hsmp_strerror(rc, errno));
	else
		pr_pass("Testing DF pstate HSMP_DF_PSTATE_0 (%d) passed\n", df_pstate);

	df_pstate = HSMP_DF_PSTATE_1;
	rc = hsmp_set_data_fabric_pstate(df_pstate);
	if (test_failed(rc))
		pr_fail("Testing DF pstate HSMP_DF_PSTATE_1 (%d) failed, %s\n",
			df_pstate, hsmp_strerror(rc, errno));
	else
		pr_pass("Testing DF pstate HSMP_DF_PSTATE_1 (%d) passed\n", df_pstate);

	df_pstate = HSMP_DF_PSTATE_2;
	rc = hsmp_set_data_fabric_pstate(df_pstate);
	if (test_failed(rc))
		pr_fail("Testing DF pstate HSMP_DF_PSTATE_2 (%d) failed, %s\n",
			df_pstate, hsmp_strerror(rc, errno));
	else
		pr_pass("Testing DF pstate HSMP_DF_PSTATE_2 (%d) passed\n", df_pstate);

	df_pstate = HSMP_DF_PSTATE_3;
	rc = hsmp_set_data_fabric_pstate(df_pstate);
	if (test_failed(rc))
		pr_fail("Testing DF pstate HSMP_DF_PSTATE_3 (%d) failed, %s\n",
			df_pstate, hsmp_strerror(rc, errno));
	else
		pr_pass("Testing DF pstate HSMP_DF_PSTATE_3 (%d) passed\n", df_pstate);
}

void test_fabric_clocks(void)
{
	int clock;
	int rc;

	printf("Testing hsmp_memory_fabric_clock()...\n");

	rc = hsmp_memory_fabric_clock(NULL);
	null_pointer_result(rc, "memory clock");

	rc = hsmp_memory_fabric_clock(&clock);
	if (test_failed(rc))
		pr_fail("Failed to get memory fabric clock, %s\n", hsmp_strerror(rc, errno));
	else
		pr_pass("Getting memory fabric clock passed, clock is %d\n", clock);

	printf("Testing hsmp_data_fabric_clock()...\n");

	rc = hsmp_data_fabric_clock(NULL);
	null_pointer_result(rc, "fabric clock");
	if (rc)
		pr_pass("Testing with NULL clock pointer passed\n");
	else
		pr_fail("Testing with NULL clock pointer failed\n");

	rc = hsmp_data_fabric_clock(&clock);
	if (test_failed(rc))
		pr_fail("Failed to get data fabric clock, %s\n", hsmp_strerror(rc, errno));
	else
		pr_pass("Getting data fabric clock passed, clock is %d\n", clock);
}

void test_core_clock_max(void)
{
	u32 clock;
	int rc;

	printf("Testing hsmp_core_clock_max_frequency()...\n");

	rc = hsmp_core_clock_max_frequency(0, NULL);
	null_pointer_result(rc, "clock pointer");

	rc = hsmp_core_clock_max_frequency(-1, &clock);
	if (test_expected_failure(rc))
		pr_fail("Testing with invalid socket id failed\n");
	else
		pr_pass("Testing with invalid socket id passed\n");

	rc = hsmp_core_clock_max_frequency(0, &clock);
	if (test_failed(rc)) {
		pr_fail("Getting core clock max freq failed, %s\n", hsmp_strerror(rc, errno));
	} else {
		if (privileged_user)
			pr_pass("Getting core clock max freq passed, clock is %d\n", clock);
		else
			pr_pass("Testing core clock max freq passed\n");
	}
}

void test_c0_residency(void)
{
	u32 residency;
	int rc;

	printf("Testing hsmp_c0_residency()...\n");

	rc = hsmp_c0_residency(0, NULL);
	null_pointer_result(rc, "residency");

	rc = hsmp_c0_residency(-1, &residency);
	if (test_expected_failure(rc))
		pr_fail("Testing with invalid socket id failed\n");
	else
		pr_pass("Testing with invalid socket id passed\n");

	rc = hsmp_c0_residency(0, &residency);
	if (test_failed(rc)) {
		pr_fail("Getting c0 residency failed, %s\n", hsmp_strerror(rc, errno));
	} else {
		if (privileged_user)
			pr_pass("Getting c0 residency passed, residency is %d\n", residency);
		else
			pr_pass("Testing c0 residency passed\n");
	}
}

void test_nbio_pstate_supported(void)
{
	enum hsmp_nbio_pstate pstate;
	int rc;

	printf("Testing hsmp_set_nbio_pstate()...\n");

	pstate = 5;
	rc = hsmp_set_nbio_pstate(0, pstate);
	if (test_expected_failure(rc))
		pr_fail("Testing with invalid pstate (%d) failed\n");
	else
		pr_pass("Testing with invalid pstate (%d) passed\n");

	pstate = HSMP_NBIO_PSTATE_AUTO;
	rc = hsmp_set_nbio_pstate(0, pstate);
	if (test_failed(rc)) {
		pr_fail("Setting HSMP_NBIO_PSTATE_AUTO pstate (%d) failed, %s\n",
			pstate, hsmp_strerror(rc, errno));
	} else {
		if (privileged_user)
			pr_pass("Setting HSMP_NBIO_PSTATE_AUTO pstate (%d) passed\n",
				pstate);
		else
			pr_pass("Testing HSMP_NBIO_PSTATE_AUTO pstate (%d) passed\n",
				pstate);
	}

	pstate = HSMP_NBIO_PSTATE_P0;
	rc = hsmp_set_nbio_pstate(0, pstate);
	if (test_failed(rc)) {
		pr_fail("Setting HSMP_NBIO_PSTATE_P0 pstate (%d) failed, %s\n",
			pstate, hsmp_strerror(rc, errno));
	} else {
		if (privileged_user)
			pr_pass("Setting HSMP_NBIO_PSTATE_P0 pstate (%d) passed\n",
				pstate);
		else
			pr_pass("Testing HSMP_NBIO_PSTATE_P0 pstate (%d) passed\n",
				pstate);
	}
}

void test_nbio_pstate_unsupported(void)
{
	enum hsmp_nbio_pstate pstate;
	int rc;

	printf("Testing unsupported hsmp_set_nbio_pstate()...\n");

	pstate = 5;
	rc = hsmp_set_nbio_pstate(0, pstate);
	if (!test_failed(rc) || errno == EOPNOTSUPP)
		pr_pass("Testing with invalid pstate (%d) passed\n");
	else
		pr_fail("Testing with invalid pstate (%d) failed\n");

	pstate = HSMP_NBIO_PSTATE_AUTO;
	rc = hsmp_set_nbio_pstate(0, pstate);
	if (!test_failed(rc) || errno == EOPNOTSUPP)
		pr_pass("Testing HSMP_NBIO_PSTATE_AUTO pstate (%d) passed\n",
			pstate);
	else
		pr_fail("Testing HSMP_NBIO_PSTATE_AUTO pstate (%d) failed, %s\n",
			pstate, hsmp_strerror(rc, errno));

	pstate = HSMP_NBIO_PSTATE_P0;
	rc = hsmp_set_nbio_pstate(0, pstate);
	if (!test_failed(rc) || errno == EOPNOTSUPP)
		pr_pass("Testing HSMP_NBIO_PSTATE_P0 pstate (%d) passed\n",
			pstate);
	else
		pr_fail("Testing HSMP_NBIO_PSTATE_P0 pstate (%d) failed, %s\n",
			pstate, hsmp_strerror(rc, errno));
}

void test_nbio_pstate(void)
{
	if (interface_version < 2)
		return test_nbio_pstate_unsupported();

	return test_nbio_pstate_supported();
}

void test_hsmp_strerror(void)
{
	char *hsmp_errstring;

	printf("Testing hsmp_errstring()...\n");

	hsmp_errstring = hsmp_strerror(HSMP_ERR_INVALID_MSG_ID, 0);
	if (strncmp(hsmp_errstring, "Invalid HSMP message ID", 23))
		pr_fail("Invalid string returned for HSMP_ERR_INVALID_MSG_ID\n\t\"%s\"\n",
			hsmp_errstring);
	else
		pr_pass("Correct string returned for HSMP_ERR_INVALID_MSG_ID\n");

	hsmp_errstring = hsmp_strerror(HSMP_ERR_INVALID_ARG, 0);
	if (strncmp(hsmp_errstring, "Invalid HSMP argument", 21))
		pr_fail("Invalid string returned for HSMP_ERR_INVALID_ARG\n\t\"%s\"\n",
			hsmp_errstring);
	else
		pr_pass("Correct string returned for HSMP_ERR_INVALID_ARG\n");

	hsmp_errstring = hsmp_strerror(0, 0);
	if (strncmp(hsmp_errstring, "Success", 7))
		pr_fail("Invalid string returned for rc == 0\n\t\"%s\"\n",
			hsmp_errstring);
	else
		pr_pass("Correct string returned for rc == 0\n");

	hsmp_errstring = hsmp_strerror(-1, EINVAL);
	if (strncmp(hsmp_errstring, "Invalid argument", 16))
		pr_fail("Invalid string returned for rc == -1, errno == EINVAL\n\t\"%s\"\n",
			hsmp_errstring);
	else
		pr_pass("Correct error string returned for rc == -1, errno == EINVAL\n");
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
	printf("Running test as %sprivileged user (euid %d)\n\n",
	       euid ? "non-" : "", euid);

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
		printf("* Setting euid to 0 *\n");
		rc = seteuid(0);
		if (rc)
			pr_fail("Failed to set euid to 0 (%d), %s\n",
				rc, hsmp_strerror(rc, errno));
		else
			privileged_user = 1;
	}

	test_proc_hot_status();
	test_df_pstate();
	test_fabric_clocks();
	test_core_clock_max();
	test_c0_residency();

	if (do_seteuid) {
		printf("* Reverting back to euid %d *\n", euid);
		rc = seteuid(euid);
		if (rc)
			pr_fail("Failed to set euid to %d (%d), %s\n",
				euid, rc, hsmp_strerror(rc, errno));

		if (euid != 0)
			privileged_user = 0;
	}

	test_nbio_pstate();
	test_hsmp_ddr();
	test_hsmp_strerror();

	print_results();
	return 0;
}
