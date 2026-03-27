/*
 * powmon-cli.c - Command-line interface for the powmon kernel module
 *
 * Usage:
 *   powmon-cli info              - Show system info
 *   powmon-cli track <pid>       - Start tracking a PID
 *   powmon-cli untrack <pid>     - Stop tracking a PID
 *   powmon-cli track-all         - Track all processes
 *   powmon-cli pid <pid>         - Query per-PID energy
 *   powmon-cli core <id>         - Query per-core energy
 *   powmon-cli package <id>      - Query per-package energy
 *   powmon-cli config [interval] - Get/set sampling interval
 *   powmon-cli reset             - Reset all stats
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <inttypes.h>

#include "powmon.h"

#define DEVICE_PATH "/dev/powmon"

static const char *vendor_str(uint32_t v)
{
	switch (v) {
	case POWMON_VENDOR_INTEL: return "Intel";
	case POWMON_VENDOR_AMD:   return "AMD";
	default:                  return "Unknown";
	}
}

static const char *domain_str(uint32_t d)
{
	switch (d) {
	case POWMON_DOMAIN_PKG:    return "Package";
	case POWMON_DOMAIN_CORE:   return "Core (PP0)";
	case POWMON_DOMAIN_UNCORE: return "Uncore (PP1)";
	case POWMON_DOMAIN_DRAM:   return "DRAM";
	case POWMON_DOMAIN_PSYS:   return "Platform";
	default:                   return "Unknown";
	}
}

static int open_device(void)
{
	int fd = open(DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n",
			DEVICE_PATH, strerror(errno));
		fprintf(stderr, "Is the powmon module loaded? "
			"(sudo insmod powmon.ko)\n");
	}
	return fd;
}

static void cmd_info(int fd)
{
	struct powmon_system_info info;

	if (ioctl(fd, POWMON_IOC_GET_INFO, &info) < 0) {
		perror("ioctl GET_INFO");
		return;
	}

	printf("=== powmon System Info ===\n");
	printf("CPU Vendor:       %s\n", vendor_str(info.vendor));
	printf("Packages:         %u\n", info.nr_packages);
	printf("Cores:            %u\n", info.nr_cores);
	printf("Tracked PIDs:     %u\n", info.nr_tracked_pids);
	printf("Sample interval:  %u ms\n", info.sample_interval_ms);
	printf("Uptime:           %.2f s\n",
	       (double)info.uptime_ns / 1e9);
	printf("RAPL unit (pkg):  1/2^%u J\n", info.rapl_unit_pkg);
	printf("RAPL unit (dram): 1/2^%u J\n", info.rapl_unit_dram);

	printf("Available domains:");
	for (int d = 0; d < POWMON_DOMAIN_COUNT; d++) {
		if (info.domain_mask & (1 << d))
			printf(" %s", domain_str(d));
	}
	printf("\n");
}

static void cmd_track(int fd, pid_t pid)
{
	int32_t p = pid;
	if (ioctl(fd, POWMON_IOC_START_TRACKING, &p) < 0) {
		perror("ioctl START_TRACKING");
		return;
	}
	printf("Now tracking PID %d\n", pid);
}

static void cmd_untrack(int fd, pid_t pid)
{
	int32_t p = pid;
	if (ioctl(fd, POWMON_IOC_STOP_TRACKING, &p) < 0) {
		perror("ioctl STOP_TRACKING");
		return;
	}
	printf("Stopped tracking PID %d\n", pid);
}

static void cmd_track_all(int fd)
{
	if (ioctl(fd, POWMON_IOC_TRACK_ALL) < 0) {
		perror("ioctl TRACK_ALL");
		return;
	}
	printf("Now tracking all processes\n");
}

static void cmd_pid(int fd, pid_t pid)
{
	struct powmon_query_pid query = { .pid = pid };

	if (ioctl(fd, POWMON_IOC_GET_PID, &query) < 0) {
		if (errno == ESRCH)
			fprintf(stderr, "PID %d not tracked\n", pid);
		else
			perror("ioctl GET_PID");
		return;
	}

	struct powmon_pid_energy *r = &query.result;

	printf("=== PID %d (%s) ===\n", r->pid, r->comm);
	printf("TGID:          %d\n", r->tgid);
	printf("CPU time:      %.3f s\n", (double)r->cpu_time_ns / 1e9);
	printf("CPU energy:    %.3f mJ (%.3f W)\n",
	       (double)r->cpu_energy_uj / 1e3,
	       (double)r->cpu_power_uw / 1e6);
	printf("DRAM energy:   %.3f mJ (%.3f W)\n",
	       (double)r->dram_energy_uj / 1e3,
	       (double)r->dram_power_uw / 1e6);
	printf("Total energy:  %.3f mJ (%.3f W)\n",
	       (double)r->total_energy_uj / 1e3,
	       (double)r->total_power_uw / 1e6);
}

static void cmd_core(int fd, uint32_t core_id)
{
	struct powmon_query_core query = { .core_id = core_id };

	if (ioctl(fd, POWMON_IOC_GET_CORE, &query) < 0) {
		perror("ioctl GET_CORE");
		return;
	}

	struct powmon_core_energy *r = &query.result;

	printf("=== Core %u (Package %u) ===\n",
	       r->core_id, r->package_id);
	printf("Attributed energy: %.3f mJ\n",
	       (double)r->energy_uj / 1e3);
	printf("Attributed power:  %.3f W\n",
	       (double)r->power_uw / 1e6);
}

static void cmd_package(int fd, uint32_t pkg_id)
{
	struct powmon_query_package query = { .package_id = pkg_id };

	if (ioctl(fd, POWMON_IOC_GET_PACKAGE, &query) < 0) {
		perror("ioctl GET_PACKAGE");
		return;
	}

	struct powmon_package_energy *r = &query.result;

	printf("=== Package %u ===\n", r->package_id);
	printf("Domain mask: 0x%x\n", r->domain_mask);

	for (int d = 0; d < POWMON_DOMAIN_COUNT; d++) {
		if (!(r->domain_mask & (1 << d)))
			continue;

		printf("  %-12s energy: %8.3f J  power: %6.3f W\n",
		       domain_str(d),
		       (double)r->domains[d].energy_uj / 1e6,
		       (double)r->domains[d].power_uw / 1e6);
	}
}

static void cmd_config(int fd, int argc, char **argv)
{
	if (argc > 0) {
		struct powmon_config config = {
			.sample_interval_ms = atoi(argv[0]),
		};
		if (ioctl(fd, POWMON_IOC_SET_CONFIG, &config) < 0) {
			perror("ioctl SET_CONFIG");
			return;
		}
		printf("Set interval to %u ms\n", config.sample_interval_ms);
	} else {
		struct powmon_config config;
		if (ioctl(fd, POWMON_IOC_GET_CONFIG, &config) < 0) {
			perror("ioctl GET_CONFIG");
			return;
		}
		printf("Sample interval: %u ms\n", config.sample_interval_ms);
	}
}

static void cmd_reset(int fd)
{
	if (ioctl(fd, POWMON_IOC_RESET_STATS) < 0) {
		perror("ioctl RESET_STATS");
		return;
	}
	printf("Stats reset\n");
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s <command> [args]\n"
		"\n"
		"Commands:\n"
		"  info              Show system info and RAPL capabilities\n"
		"  track <pid>       Start tracking a process\n"
		"  untrack <pid>     Stop tracking a process\n"
		"  track-all         Track all processes\n"
		"  pid <pid>         Query energy for a tracked PID\n"
		"  core <id>         Query energy for a CPU core\n"
		"  package <id>      Query energy for a CPU package\n"
		"  config [ms]       Get/set sampling interval\n"
		"  reset             Reset all tracking data\n"
		"\n"
		"Example:\n"
		"  %s track-all\n"
		"  sleep 5\n"
		"  %s pid 1234\n",
		prog, prog, prog);
}

int main(int argc, char **argv)
{
	int fd;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	fd = open_device();
	if (fd < 0)
		return 1;

	if (strcmp(argv[1], "info") == 0) {
		cmd_info(fd);
	} else if (strcmp(argv[1], "track") == 0 && argc >= 3) {
		cmd_track(fd, atoi(argv[2]));
	} else if (strcmp(argv[1], "untrack") == 0 && argc >= 3) {
		cmd_untrack(fd, atoi(argv[2]));
	} else if (strcmp(argv[1], "track-all") == 0) {
		cmd_track_all(fd);
	} else if (strcmp(argv[1], "pid") == 0 && argc >= 3) {
		cmd_pid(fd, atoi(argv[2]));
	} else if (strcmp(argv[1], "core") == 0 && argc >= 3) {
		cmd_core(fd, atoi(argv[2]));
	} else if (strcmp(argv[1], "package") == 0 && argc >= 3) {
		cmd_package(fd, atoi(argv[2]));
	} else if (strcmp(argv[1], "config") == 0) {
		cmd_config(fd, argc - 2, argv + 2);
	} else if (strcmp(argv[1], "reset") == 0) {
		cmd_reset(fd);
	} else {
		usage(argv[0]);
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
