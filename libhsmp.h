/* SPDX-License-Identifier: MIT License */
/*
 * Copyright (C) 2020 Advanced Micro Devices, Inc. - All Rights Reserved
 *
 * Author: Nathan Fontenot <nathan.fontenot@amd.com>
 * Author: Lewis Carroll <lewis.carroll@amd.com>
 *
 * AMD Host System Management Port library
 */

#ifndef _LIBHSMP_H_
#define _LIBHSMP_H_ 1

#include <pci/types.h>

/* HSMP error codes as defined in the PPR */
#define HSMP_ERR_INVALID_MSG_ID 0xFE
#define HSMP_ERR_INVALID_ARG    0xFF

/* Wrapper to retrieve HSMP error or errno string */
char *hsmp_strerror(int err, int ernno_val);

struct smu_fw_version {
	u8 debug;	/* Debug version number */
	u8 minor;	/* Minor version number */
	u8 major;	/* Major version number */
	u8 unused;
};

/* Get SMU FW version and store it in smu_fw */
int hsmp_smu_fw_version(struct smu_fw_version *smu_fw);

/* Get HSMP Interface version and store it in version */
int hsmp_interface_version(int *version);

/*
 * Get the average power consumption (in milliwatts) for the
 * specified socket.
 */
int hsmp_socket_power(int socket_id, u32 *power);

/*
 * Set the power consumption limit (in milliwatts) for
 * the specified socket.
 *
 * The power limit specified will be clipped to the maximum cTDP range for
 * the processor. Note that there is a limit on the minimum power that the
 * processor can operate at, no further power socket reduction occurs if
 * the limit is set below that minimum.
 */
int hsmp_set_socket_power_limit(int socket_id, u32 power_limit);

/*
 * Get the current power consumption limit (in milliwatts) for
 * the specified socket.
 */
int hsmp_socket_power_limit(int socket_id, u32 *power_limit);

/*
 * Get the maximum socket power consumption limit that can be set
 * for the specified socket.
 */
int hsmp_socket_max_power_limit(int socket_id, u32 *max_power);

/*
 * Note on boost limits.
 *
 * For every core, the System Management Unit maintains several different
 * limits on core frequency. Because limits are maintained by the SMN and
 * not by cores themselves, limits can be set and read regardless of the
 * core hotplug state. This also means when using hotplug to bring a core
 * back online, any previous limits at a core or socket level will remain
 * enforced.
 *
 * The various limits to per-core boost include the following:
 * - The fused in SKU-specific maximum boost frequency
 * - Any limit set by UEFI firmware
 * - Any limit set by Platform Management (BMC) via the processor's Advanced
 *   Platform Management Link (APML)
 * - Limit set by HSMP (this interface)
 *
 * When setting the boost limit for a core, the actual limit that is enforced
 * will be constrained to a range of valid values for the processor and will
 * be the lowest of any of the above limits. To remove the HSMP limit for a
 * core, write a value equal to or greater than the fused-in maximum
 * frequency. A value of 0xFFFF works well.
 *
 * If SMT is enabled, it is only necessary to set a limit for one of the
 * two thread siblings. Since the limit is a core limit, it will apply to
 * both siblings.
 *
 * Boost limits are set and read in units of MHz.
 */

/* Set HSMP Boost Limit for the specified core. */
int hsmp_set_cpu_boost_limit(int cpu, u32 boost_limit);

/* Set HSMP Boost Limit for all cores in the specified socket. */
int hsmp_set_socket_boost_limit(int socket_id, u32 boost_limit);

/* Set HSMP Boost Limit for the system. */
int hsmp_set_system_boost_limit(u32 boost_limit);

/* Get the HSMP Boost Limit for the specified core. */
int hsmp_cpu_boost_limit(int cpu, u32 *boost_limit);

/*
 * Get normalized status of the specified CPUs PROC_HOT status.
 * (1 = active, 0 = inactive)
 */
int hsmp_proc_hot_status(int socket_id, int *status);

/*
 * Note on xGMI link width.
 *
 * xGMI link width control is only for 2P platfroms. During normal
 * operation, Dynamic Link Width Management (DLWM) automatically
 * sets the link width based on socket-to-socket bandwidth demand.
 * This automatic link width selection is between x2 and x16 on
 * family 19h and between x8 and x16 on family 17h. The set link
 * width API allows you to modify these limits, including setting
 * a fixed link width by specifying min = max. You can return to
 * automatic/default operation with the set link width auto API.
 */
enum hsmp_xgmi_width {
	HSMP_XGMI_WIDTH_X2,
	HSMP_XGMI_WIDTH_X8,
	HSMP_XGMI_WIDTH_X16,
};

/* Set xGMI dynamic link width minimum and maximum */
int hsmp_set_xgmi_width(enum hsmp_xgmi_width min_width,
			enum hsmp_xgmi_width max_width);

/* Enable automatic xGMI link width selection */
int hsmp_set_xgmi_auto(void);

/*
 * The data fabric P-state selection can be in the range of
 * HSMP_DF_PSTATE_0 (the highest DF P-state) to HSMP_DF_PSTATE_3
 * (the lowest DF P-state).
 *
 * Specifying HSMP_DF_PSTATE_AUTO will enable automatic P-state
 * selection based on data fabric utilization.
 */
enum hsmp_df_pstate {
	HSMP_DF_PSTATE_0,
	HSMP_DF_PSTATE_1,
	HSMP_DF_PSTATE_2,
	HSMP_DF_PSTATE_3,
	HSMP_DF_PSTATE_AUTO,
};

/* Set the data fabric P-state for the specified socket. */
int hsmp_set_data_fabric_pstate(int socket_id, enum hsmp_df_pstate pstate);

/*
 * Get the current data fabric clock (in MHz) and memory clock (in MHz)
 * for the specified socket.
 */
int hsmp_fabric_clocks(int socket_id, int *data_fabric_clock, int *mem_clock);

/* Get the current data fabric clock (in MHz) for the specified socket. */
int hsmp_data_fabric_clock(int socket_id, int *data_fabric_clock);

/* Get the current memory clock (in MHZ) for the specified socket. */
int hsmp_memory_clock(int socket_id, int *mem_clock);

/*
 * Get the maximum core clock (in MHZ) allowed by the most restrictive
 * limit at the time of the call.
 */
int hsmp_core_clock_max_frequency(int socket_id, u32 *max_freq);

/*
 * Get the C0 residency percentage for all cores in the specified
 * socket. Residency is returned as an integer between 0 - 100, where
 * 100 specifies that all enabled cores in the socket are running in C0.
 */
int hsmp_c0_residency(int socket_id, u32 *residency);

/*
 * The NBIO (PCI-e interface) P-state selection can be specified as
 * HSMP_NBIO_PSTATE_0 (the highest NBIO P-state) or HSMP_NBIO_PSTATE_AUTO
 * which will enable automatic P-state selection based on bus utilization.
 */
enum hsmp_nbio_pstate {
	HSMP_NBIO_PSTATE_AUTO,
	HSMP_NBIO_PSTATE_P0,
};

/*
 * Set the NBIO (PCI-e interface) P-state for the root complex hosting the
 * specified PCI bus number (0x00 - 0xFF).
 *
 * Only available on systems with hsmp interface version >= 2.
 */
int hsmp_set_nbio_pstate(u8 bus_num, enum hsmp_nbio_pstate pstate);

/*
 * Helper function to iterate over enumerated PCIe controller complexes in
 * the system. Begin a new search by setting idx = 0. If the return value
 * is -1, errno will indicate the reason, which can be any of the reasons
 * library initialization may fail (e.g. not running as root or HSMP support
 * is disabled in firmware, etc.), or an invalid parameter was supplied.
 *
 * Otherwise, the return value for the first call will be greater than zero
 * (usually 1 after the first call with idx=0) and bus_num will be set to
 * the lowest base bus in the system (usually 0x00 after the first call with
 * idx=0). The return value should be assigned to idx for each subsequent
 * iteration.
 *
 * When the API call succeeds in returning the last bus in the system, the
 * end of the bus list has been reached, the return value will be zero.
 *
 * Note this enumeration may not include every possible bus in the system.
 * It will only include the busses corresponding to the base bus for a PCIe
 * controller complex.
 */
int hsmp_next_bus(int idx, u8 *bus_num);

/*
 * Get the theoretical maximum DDR bandwidth in GB/s, the current utilized
 * DDR bandwidth (read + write) in GB/s, and the current utilized DDR bandwidth
 * as a percentage of the theoretical maximum for the specified socket.
 */
int hsmp_ddr_bandwidths(int socket_id, u32 *max_bw, u32 *utilized_bw,
			u32 *utilized_pct);

/*
 * Get the theoretical maximum DDR bandwidth in GB/s for the specified socket.
 *
 * Only available on systems with hsmp interface version >= 3.
 */
int hsmp_ddr_max_bandwidth(int socket_id, u32 *max_bw);

/*
 * Get the current utilized DDR bandwidth (read + write) in GB/s for
 * the specified socket.
 *
 * Only available on systems with hsmp interface version >= 3.
 */
int hsmp_ddr_utilized_bandwidth(int socket_id, u32 *utilized_bw);

/*
 * Get the current utilized DDR bandwidth as a percentage of the
 * theoretical maximum for the specified socket.
 *
 * Only available on systems with hsmp interface version >= 3.
 */
int hsmp_ddr_utilized_percent(int socket_id, u32 *utilized_pct);

#endif
