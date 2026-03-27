/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * powmon.h - Power Monitor kernel module interface
 *
 * Provides per-PID, per-core, per-cgroup energy attribution
 * using Intel/AMD RAPL MSRs with timer-based sampling.
 *
 * Userspace communicates via /dev/powmon ioctl interface.
 */

#ifndef _UAPI_POWMON_H
#define _UAPI_POWMON_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define POWMON_DEVICE_NAME  "powmon"
#define POWMON_MAX_CORES    256
#define POWMON_MAX_PACKAGES 8
#define POWMON_MAX_PIDS     4096
#define POWMON_MAX_CGROUPS  256

/* RAPL domain identifiers */
#define POWMON_DOMAIN_PKG    0  /* CPU package */
#define POWMON_DOMAIN_CORE   1  /* CPU cores (PP0) */
#define POWMON_DOMAIN_UNCORE 2  /* Uncore / GPU (PP1, Intel only) */
#define POWMON_DOMAIN_DRAM   3  /* DRAM */
#define POWMON_DOMAIN_PSYS   4  /* Platform (Psys, Intel only) */
#define POWMON_DOMAIN_COUNT  5

/* CPU vendor */
#define POWMON_VENDOR_UNKNOWN 0
#define POWMON_VENDOR_INTEL   1
#define POWMON_VENDOR_AMD     2

/*
 * Energy reading for a single RAPL domain.
 * energy_uj: cumulative energy in microjoules since module load
 * power_uw: average power in microwatts over last sample interval
 */
struct powmon_domain_energy {
	__u32 domain;
	__u64 energy_uj;
	__u64 power_uw;
	__u64 timestamp_ns;
};

/*
 * Per-package (socket) energy breakdown.
 */
struct powmon_package_energy {
	__u32 package_id;
	struct powmon_domain_energy domains[POWMON_DOMAIN_COUNT];
	__u32 domain_mask; /* bitmask of available domains */
};

/*
 * Per-CPU-core energy attribution.
 * Attributed from package energy proportional to core utilization.
 */
struct powmon_core_energy {
	__u32 core_id;
	__u32 package_id;
	__u64 energy_uj;       /* attributed energy in microjoules */
	__u64 power_uw;        /* attributed power in microwatts */
	__u64 active_time_ns;  /* time core was active in sample window */
	__u64 timestamp_ns;
};

/*
 * Per-PID energy attribution.
 * CPU energy is proportional to CPU time consumed.
 * DRAM is proportional to RSS-weighted memory pressure.
 */
struct powmon_pid_energy {
	__s32 pid;
	__s32 tgid;
	__u64 cpu_energy_uj;   /* attributed CPU energy */
	__u64 dram_energy_uj;  /* attributed DRAM energy */
	__u64 total_energy_uj; /* cpu + dram */
	__u64 cpu_power_uw;    /* instantaneous attributed CPU power */
	__u64 dram_power_uw;   /* instantaneous attributed DRAM power */
	__u64 total_power_uw;  /* cpu + dram power */
	__u64 cpu_time_ns;     /* total CPU time since tracking started */
	__u64 timestamp_ns;
	char  comm[16];        /* process name */
};

/*
 * Per-cgroup energy attribution.
 * Aggregates all PIDs within a cgroup hierarchy.
 */
struct powmon_cgroup_energy {
	__u64 cgroup_id;
	__u64 cpu_energy_uj;
	__u64 dram_energy_uj;
	__u64 total_energy_uj;
	__u64 cpu_power_uw;
	__u64 dram_power_uw;
	__u64 total_power_uw;
	__u32 nr_pids;         /* number of tracked PIDs in cgroup */
	__u64 timestamp_ns;
	char  path[256];       /* cgroup path */
};

/*
 * System-wide summary.
 */
struct powmon_system_info {
	__u32 vendor;          /* POWMON_VENDOR_* */
	__u32 nr_packages;
	__u32 nr_cores;
	__u32 nr_tracked_pids;
	__u32 sample_interval_ms;
	__u64 uptime_ns;       /* time since module loaded */
	__u32 rapl_unit_pkg;   /* raw RAPL energy unit for package */
	__u32 rapl_unit_dram;  /* raw RAPL energy unit for DRAM */
	__u32 domain_mask;     /* globally available RAPL domains */
};

/* --- Query structures for ioctls --- */

struct powmon_query_pid {
	__s32 pid;                        /* input: PID to query */
	struct powmon_pid_energy result;   /* output */
};

struct powmon_query_core {
	__u32 core_id;                      /* input: core to query */
	struct powmon_core_energy result;    /* output */
};

struct powmon_query_package {
	__u32 package_id;                     /* input: package to query */
	struct powmon_package_energy result;   /* output */
};

struct powmon_query_cgroup {
	__u64 cgroup_id;                       /* input: cgroup id (from stat) */
	struct powmon_cgroup_energy result;     /* output */
};

/*
 * Bulk query: get top-N PIDs by energy consumption.
 */
struct powmon_query_top {
	__u32 max_entries;                     /* input: max PIDs to return */
	__u32 nr_entries;                      /* output: actual count */
	struct powmon_pid_energy entries[];     /* output: flexible array */
};

/*
 * Configuration.
 */
struct powmon_config {
	__u32 sample_interval_ms;  /* sampling period (10-10000 ms) */
	__u32 flags;               /* reserved */
};

#define POWMON_CONFIG_MIN_INTERVAL_MS  10
#define POWMON_CONFIG_MAX_INTERVAL_MS  10000

/* --- ioctl commands --- */

#define POWMON_IOC_MAGIC  'P'

/* System info */
#define POWMON_IOC_GET_INFO       _IOR(POWMON_IOC_MAGIC, 0x01, struct powmon_system_info)

/* Per-PID queries */
#define POWMON_IOC_GET_PID        _IOWR(POWMON_IOC_MAGIC, 0x10, struct powmon_query_pid)
#define POWMON_IOC_GET_TOP        _IOWR(POWMON_IOC_MAGIC, 0x11, struct powmon_query_top)

/* Per-core queries */
#define POWMON_IOC_GET_CORE       _IOWR(POWMON_IOC_MAGIC, 0x20, struct powmon_query_core)

/* Per-package queries */
#define POWMON_IOC_GET_PACKAGE    _IOWR(POWMON_IOC_MAGIC, 0x30, struct powmon_query_package)

/* Per-cgroup queries */
#define POWMON_IOC_GET_CGROUP     _IOWR(POWMON_IOC_MAGIC, 0x40, struct powmon_query_cgroup)

/* Configuration */
#define POWMON_IOC_SET_CONFIG     _IOW(POWMON_IOC_MAGIC, 0x50, struct powmon_config)
#define POWMON_IOC_GET_CONFIG     _IOR(POWMON_IOC_MAGIC, 0x51, struct powmon_config)

/* Control */
#define POWMON_IOC_RESET_STATS    _IO(POWMON_IOC_MAGIC, 0x60)
#define POWMON_IOC_START_TRACKING _IOW(POWMON_IOC_MAGIC, 0x61, __s32)  /* start tracking a PID */
#define POWMON_IOC_STOP_TRACKING  _IOW(POWMON_IOC_MAGIC, 0x62, __s32)  /* stop tracking a PID */
#define POWMON_IOC_TRACK_ALL      _IO(POWMON_IOC_MAGIC, 0x63)          /* track all PIDs */

#endif /* _UAPI_POWMON_H */
