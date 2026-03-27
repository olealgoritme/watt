// SPDX-License-Identifier: GPL-2.0
/*
 * powmon.c - Per-process power monitoring kernel module
 *
 * Reads Intel/AMD RAPL MSRs on a timer, attributes energy to
 * individual PIDs proportional to their CPU time consumption,
 * and exposes results via /dev/powmon ioctl interface.
 *
 * Supports: per-PID, per-core, per-cgroup, per-package granularity.
 * Target: Linux 6.x, x86_64 (Intel & AMD)
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/cputime.h>
#include <linux/pid.h>
#include <linux/cgroup.h>
#include <linux/rcupdate.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/topology.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/cpu_device_id.h>

#include "powmon.h"

/* ---------- RAPL MSR definitions ---------- */
/* All MSR addresses come from <asm/msr-index.h> via <asm/msr.h> */

/* ---------- Module parameters ---------- */

static unsigned int sample_interval_ms = 100;
module_param(sample_interval_ms, uint, 0644);
MODULE_PARM_DESC(sample_interval_ms, "Sampling interval in milliseconds (default: 100)");

static bool track_all = false;
module_param(track_all, bool, 0644);
MODULE_PARM_DESC(track_all, "Track all processes on load (default: false)");

/* ---------- Internal data structures ---------- */

/* Per-PID tracking entry */
struct powmon_pid_entry {
	struct hlist_node  node;
	pid_t              pid;
	pid_t              tgid;
	char               comm[TASK_COMM_LEN];
	u64                prev_cpu_time_ns;  /* previous sample's total CPU time */
	u64                delta_cpu_ns;      /* CPU time delta in last interval */
	u64                cum_cpu_energy_uj;  /* cumulative attributed CPU energy */
	u64                cum_dram_energy_uj; /* cumulative attributed DRAM energy */
	u64                last_cpu_power_uw;  /* last interval's CPU power */
	u64                last_dram_power_uw; /* last interval's DRAM power */
	u64                total_cpu_time_ns;  /* total CPU time since tracking */
	u64                cgroup_id;
	ktime_t            last_update;
	bool               active;            /* process still alive */
};

/* Per-core tracking */
struct powmon_core_data {
	u64 prev_active_ns;
	u64 delta_active_ns;
	u64 attributed_energy_uj;
	u64 attributed_power_uw;
	u32 package_id;
};

/* Per-package RAPL state */
struct powmon_package_data {
	u32 package_id;
	u32 domain_mask;

	/* Previous raw RAPL counter values */
	u64 prev_energy_raw[POWMON_DOMAIN_COUNT];
	/* Cumulative energy in microjoules */
	u64 cum_energy_uj[POWMON_DOMAIN_COUNT];
	/* Delta energy in last interval (microjoules) */
	u64 delta_energy_uj[POWMON_DOMAIN_COUNT];
	/* Power in microwatts */
	u64 power_uw[POWMON_DOMAIN_COUNT];

	/* Which CPU to read MSRs from for this package */
	int representative_cpu;
};

/* Per-cgroup tracking */
struct powmon_cgroup_entry {
	struct hlist_node node;
	u64    cgroup_id;
	char   path[256];
	u64    cpu_energy_uj;
	u64    dram_energy_uj;
	u64    cpu_power_uw;
	u64    dram_power_uw;
	u32    nr_pids;
};

/* ---------- Global state ---------- */

#define PID_HASH_BITS   12
#define CGROUP_HASH_BITS 8

static DEFINE_HASHTABLE(pid_table, PID_HASH_BITS);
static DEFINE_HASHTABLE(cgroup_table, CGROUP_HASH_BITS);
static DEFINE_SPINLOCK(pid_lock);
static DEFINE_SPINLOCK(powmon_cgroup_lock);

static struct powmon_package_data *packages;
static struct powmon_core_data    *cores;
static u32 nr_packages;
static u32 nr_cores;
static u32 cpu_vendor;

/* RAPL energy units (microjoules per raw unit) */
static u64 energy_unit_uj_pkg;   /* fixed-point: units * 1000000 */
static u64 energy_unit_uj_dram;
static u32 raw_energy_unit_pkg;
static u32 raw_energy_unit_dram;

/* RAPL counter overflow: 32-bit counter */
#define RAPL_COUNTER_MAX  0xFFFFFFFFULL

/* Timer / workqueue */
static struct workqueue_struct *powmon_wq;
static struct delayed_work      powmon_work;
static ktime_t                  module_start_time;
static ktime_t                  last_sample_time;
static bool                     module_running;
static bool                     tracking_all;

/* ---------- RAPL helpers ---------- */

/*
 * Read the RAPL energy unit MSR and convert to a multiplier.
 * Intel: MSR_RAPL_POWER_UNIT bits [12:8] = ESU
 *        energy_unit = 1 / (2^ESU) joules
 * AMD:   MSR_AMD_RAPL_POWER_UNIT, same bit layout
 *
 * We store as micro-joules per raw unit, scaled by 2^20 for precision.
 */
static int powmon_read_energy_units(void)
{
	u64 val;
	u32 esu;
	int ret;

	if (cpu_vendor == POWMON_VENDOR_INTEL) {
		ret = rdmsrl_safe(MSR_RAPL_POWER_UNIT, &val);
	} else if (cpu_vendor == POWMON_VENDOR_AMD) {
		ret = rdmsrl_safe(MSR_AMD_RAPL_POWER_UNIT, &val);
	} else {
		return -ENODEV;
	}

	if (ret)
		return ret;

	/* ESU is bits [12:8] */
	esu = (val >> 8) & 0x1F;
	raw_energy_unit_pkg = esu;

	/*
	 * energy_unit = 1.0 / (1 << esu) joules
	 * In microjoules: 1000000 / (1 << esu)
	 * We store this scaled by 1024 for better integer precision.
	 */
	energy_unit_uj_pkg = (1000000ULL << 10) / (1ULL << esu);

	/*
	 * DRAM may use a different unit on some Intel server parts.
	 * For simplicity, assume same unit; override if needed.
	 */
	energy_unit_uj_dram = energy_unit_uj_pkg;
	raw_energy_unit_dram = esu;

	pr_info("RAPL energy unit: ESU=%u -> %llu uJ/unit (scaled)\n",
		esu, energy_unit_uj_pkg >> 10);

	return 0;
}

/* Convert raw RAPL counter delta to microjoules */
static inline u64 rapl_raw_to_uj(u64 raw_delta, u64 unit_scaled)
{
	/* unit_scaled is (uJ_per_unit << 10), so: */
	return (raw_delta * unit_scaled) >> 10;
}

/* Read a RAPL MSR on a specific CPU */
static int read_rapl_msr_on_cpu(int cpu, u32 msr, u64 *val)
{
	return rdmsrl_safe_on_cpu(cpu, msr, val);
}

/*
 * Detect available RAPL domains for a package.
 * Try reading each MSR; if it succeeds, the domain is available.
 */
static u32 detect_rapl_domains(int cpu)
{
	u64 val;
	u32 mask = 0;

	if (cpu_vendor == POWMON_VENDOR_INTEL) {
		if (!read_rapl_msr_on_cpu(cpu, MSR_PKG_ENERGY_STATUS, &val))
			mask |= (1 << POWMON_DOMAIN_PKG);
		if (!read_rapl_msr_on_cpu(cpu, MSR_PP0_ENERGY_STATUS, &val))
			mask |= (1 << POWMON_DOMAIN_CORE);
		if (!read_rapl_msr_on_cpu(cpu, MSR_PP1_ENERGY_STATUS, &val))
			mask |= (1 << POWMON_DOMAIN_UNCORE);
		if (!read_rapl_msr_on_cpu(cpu, MSR_DRAM_ENERGY_STATUS, &val))
			mask |= (1 << POWMON_DOMAIN_DRAM);
		if (!read_rapl_msr_on_cpu(cpu, MSR_PLATFORM_ENERGY_STATUS, &val))
			mask |= (1 << POWMON_DOMAIN_PSYS);
	} else if (cpu_vendor == POWMON_VENDOR_AMD) {
		if (!read_rapl_msr_on_cpu(cpu, MSR_AMD_PKG_ENERGY_STATUS, &val))
			mask |= (1 << POWMON_DOMAIN_PKG);
		if (!read_rapl_msr_on_cpu(cpu, MSR_AMD_CORE_ENERGY_STATUS, &val))
			mask |= (1 << POWMON_DOMAIN_CORE);
		/* AMD doesn't have UNCORE/PSYS; some server parts have DRAM */
		if (!read_rapl_msr_on_cpu(cpu, MSR_DRAM_ENERGY_STATUS, &val))
			mask |= (1 << POWMON_DOMAIN_DRAM);
	}

	return mask;
}

/* Get the MSR address for a domain */
static u32 domain_to_msr(u32 domain)
{
	if (cpu_vendor == POWMON_VENDOR_INTEL) {
		switch (domain) {
		case POWMON_DOMAIN_PKG:    return MSR_PKG_ENERGY_STATUS;
		case POWMON_DOMAIN_CORE:   return MSR_PP0_ENERGY_STATUS;
		case POWMON_DOMAIN_UNCORE: return MSR_PP1_ENERGY_STATUS;
		case POWMON_DOMAIN_DRAM:   return MSR_DRAM_ENERGY_STATUS;
		case POWMON_DOMAIN_PSYS:   return MSR_PLATFORM_ENERGY_STATUS;
		}
	} else if (cpu_vendor == POWMON_VENDOR_AMD) {
		switch (domain) {
		case POWMON_DOMAIN_PKG:  return MSR_AMD_PKG_ENERGY_STATUS;
		case POWMON_DOMAIN_CORE: return MSR_AMD_CORE_ENERGY_STATUS;
		case POWMON_DOMAIN_DRAM: return MSR_DRAM_ENERGY_STATUS;
		}
	}
	return 0;
}

/* ---------- Topology detection ---------- */

static int powmon_detect_topology(void)
{
	int cpu, pkg;
	int max_pkg = -1;

	nr_cores = num_online_cpus();
	if (nr_cores > POWMON_MAX_CORES) {
		pr_warn("Too many CPUs (%u > %u), clamping\n",
			nr_cores, POWMON_MAX_CORES);
		nr_cores = POWMON_MAX_CORES;
	}

	/* Find number of packages */
	for_each_online_cpu(cpu) {
		pkg = topology_physical_package_id(cpu);
		if (pkg > max_pkg)
			max_pkg = pkg;
	}
	nr_packages = max_pkg + 1;
	if (nr_packages > POWMON_MAX_PACKAGES)
		nr_packages = POWMON_MAX_PACKAGES;

	pr_info("Detected %u cores, %u packages\n", nr_cores, nr_packages);
	return 0;
}

static int powmon_detect_vendor(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;

	if (c->x86_vendor == X86_VENDOR_INTEL) {
		cpu_vendor = POWMON_VENDOR_INTEL;
		pr_info("Detected Intel CPU (family %u, model %u)\n",
			c->x86, c->x86_model);
	} else if (c->x86_vendor == X86_VENDOR_AMD) {
		cpu_vendor = POWMON_VENDOR_AMD;
		pr_info("Detected AMD CPU (family %u, model %u)\n",
			c->x86, c->x86_model);
	} else {
		pr_err("Unsupported CPU vendor\n");
		return -ENODEV;
	}
	return 0;
}

/* ---------- Package / core init ---------- */

static int powmon_init_packages(void)
{
	int cpu;
	u32 i, d;

	packages = kcalloc(nr_packages, sizeof(*packages), GFP_KERNEL);
	if (!packages)
		return -ENOMEM;

	/* Initialize representative CPUs to -1 */
	for (i = 0; i < nr_packages; i++) {
		packages[i].package_id = i;
		packages[i].representative_cpu = -1;
	}

	/* Assign a representative CPU per package */
	for_each_online_cpu(cpu) {
		u32 pkg = topology_physical_package_id(cpu);
		if (pkg < nr_packages && packages[pkg].representative_cpu == -1)
			packages[pkg].representative_cpu = cpu;
	}

	/* Detect RAPL domains and read initial counters */
	for (i = 0; i < nr_packages; i++) {
		int rep = packages[i].representative_cpu;
		if (rep < 0)
			continue;

		packages[i].domain_mask = detect_rapl_domains(rep);
		pr_info("Package %u (CPU %d): domain_mask=0x%x\n",
			i, rep, packages[i].domain_mask);

		/* Read initial counters */
		for (d = 0; d < POWMON_DOMAIN_COUNT; d++) {
			u32 msr;
			u64 val;

			if (!(packages[i].domain_mask & (1 << d)))
				continue;

			msr = domain_to_msr(d);
			if (msr && !read_rapl_msr_on_cpu(rep, msr, &val))
				packages[i].prev_energy_raw[d] = val & RAPL_COUNTER_MAX;
		}
	}

	return 0;
}

static int powmon_init_cores(void)
{
	int cpu;

	cores = kcalloc(nr_cores, sizeof(*cores), GFP_KERNEL);
	if (!cores)
		return -ENOMEM;

	for_each_online_cpu(cpu) {
		if ((u32)cpu >= nr_cores)
			break;
		cores[cpu].package_id = topology_physical_package_id(cpu);
	}

	return 0;
}

/* ---------- PID tracking ---------- */

static struct powmon_pid_entry *find_pid_entry(pid_t pid)
{
	struct powmon_pid_entry *entry;

	hash_for_each_possible(pid_table, entry, node, pid) {
		if (entry->pid == pid)
			return entry;
	}
	return NULL;
}

static struct powmon_pid_entry *create_pid_entry(struct task_struct *task)
{
	struct powmon_pid_entry *entry;

	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return NULL;

	entry->pid = task->pid;
	entry->tgid = task->tgid;
	get_task_comm(entry->comm, task);
	entry->active = true;
	entry->last_update = ktime_get();

	/* Read initial CPU time */
	entry->prev_cpu_time_ns = task->utime + task->stime;

	/* Get cgroup ID if available */
#ifdef CONFIG_CGROUPS
	rcu_read_lock();
	{
		struct cgroup *cgrp = task_cgroup(task, cpu_cgrp_id);
		if (cgrp)
			entry->cgroup_id = cgroup_id(cgrp);
	}
	rcu_read_unlock();
#endif

	hash_add(pid_table, &entry->node, entry->pid);
	return entry;
}

static int powmon_start_tracking_pid(pid_t pid)
{
	struct task_struct *task;
	struct powmon_pid_entry *entry;
	unsigned long flags;

	spin_lock_irqsave(&pid_lock, flags);
	entry = find_pid_entry(pid);
	if (entry) {
		entry->active = true;
		spin_unlock_irqrestore(&pid_lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&pid_lock, flags);

	rcu_read_lock();
	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (!task) {
		rcu_read_unlock();
		return -ESRCH;
	}
	get_task_struct(task);
	rcu_read_unlock();

	spin_lock_irqsave(&pid_lock, flags);
	entry = create_pid_entry(task);
	spin_unlock_irqrestore(&pid_lock, flags);

	put_task_struct(task);
	return entry ? 0 : -ENOMEM;
}

static int powmon_stop_tracking_pid(pid_t pid)
{
	struct powmon_pid_entry *entry;
	unsigned long flags;

	spin_lock_irqsave(&pid_lock, flags);
	entry = find_pid_entry(pid);
	if (entry) {
		hash_del(&entry->node);
		kfree(entry);
	}
	spin_unlock_irqrestore(&pid_lock, flags);

	return entry ? 0 : -ESRCH;
}

/* ---------- Cgroup tracking ---------- */

static struct powmon_cgroup_entry *find_or_create_cgroup(u64 cgroup_id)
{
	struct powmon_cgroup_entry *entry;

	hash_for_each_possible(cgroup_table, entry, node, cgroup_id) {
		if (entry->cgroup_id == cgroup_id)
			return entry;
	}

	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return NULL;

	entry->cgroup_id = cgroup_id;
	hash_add(cgroup_table, &entry->node, cgroup_id);
	return entry;
}

static void reset_cgroup_stats(void)
{
	struct powmon_cgroup_entry *entry;
	struct hlist_node *tmp;
	int bkt;

	hash_for_each_safe(cgroup_table, bkt, tmp, entry, node) {
		entry->cpu_energy_uj = 0;
		entry->dram_energy_uj = 0;
		entry->cpu_power_uw = 0;
		entry->dram_power_uw = 0;
		entry->nr_pids = 0;
	}
}

/* ---------- Sampling work function ---------- */

static void powmon_sample_rapl(void)
{
	u32 i, d;
	ktime_t now = ktime_get();
	u64 dt_ns = ktime_to_ns(ktime_sub(now, last_sample_time));

	if (dt_ns == 0)
		dt_ns = 1;

	for (i = 0; i < nr_packages; i++) {
		int rep = packages[i].representative_cpu;
		if (rep < 0)
			continue;

		for (d = 0; d < POWMON_DOMAIN_COUNT; d++) {
			u32 msr;
			u64 raw, delta, unit;
			u64 prev;

			if (!(packages[i].domain_mask & (1 << d)))
				continue;

			msr = domain_to_msr(d);
			if (!msr || read_rapl_msr_on_cpu(rep, msr, &raw))
				continue;

			raw &= RAPL_COUNTER_MAX;
			prev = packages[i].prev_energy_raw[d];

			/* Handle 32-bit wraparound */
			if (raw >= prev)
				delta = raw - prev;
			else
				delta = (RAPL_COUNTER_MAX - prev) + raw + 1;

			packages[i].prev_energy_raw[d] = raw;

			/* Convert to microjoules */
			unit = (d == POWMON_DOMAIN_DRAM) ?
				energy_unit_uj_dram : energy_unit_uj_pkg;
			delta = rapl_raw_to_uj(delta, unit);

			packages[i].delta_energy_uj[d] = delta;
			packages[i].cum_energy_uj[d] += delta;

			/* Power = energy / time (in microwatts) */
			packages[i].power_uw[d] =
				div64_u64(delta * 1000000000ULL, dt_ns);
		}
	}

	last_sample_time = now;
}

static void powmon_attribute_to_pids(void)
{
	struct powmon_pid_entry *entry;
	struct hlist_node *tmp;
	int bkt;
	u64 total_delta_ns = 0;
	u64 total_pkg_delta_uj = 0;
	u64 total_dram_delta_uj = 0;
	u32 i;
	unsigned long flags;
	ktime_t now = ktime_get();

	/* Sum up package-level energy deltas across all packages */
	for (i = 0; i < nr_packages; i++) {
		if (packages[i].domain_mask & (1 << POWMON_DOMAIN_PKG))
			total_pkg_delta_uj += packages[i].delta_energy_uj[POWMON_DOMAIN_PKG];
		if (packages[i].domain_mask & (1 << POWMON_DOMAIN_DRAM))
			total_dram_delta_uj += packages[i].delta_energy_uj[POWMON_DOMAIN_DRAM];
	}

	spin_lock_irqsave(&pid_lock, flags);

	/* First pass: compute CPU time deltas and total */
	hash_for_each_safe(pid_table, bkt, tmp, entry, node) {
		struct task_struct *task;

		rcu_read_lock();
		task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
		if (task) {
			u64 current_cpu_ns = task->utime + task->stime;

			if (current_cpu_ns >= entry->prev_cpu_time_ns)
				entry->delta_cpu_ns = current_cpu_ns - entry->prev_cpu_time_ns;
			else
				entry->delta_cpu_ns = 0;

			entry->prev_cpu_time_ns = current_cpu_ns;
			entry->total_cpu_time_ns += entry->delta_cpu_ns;
			total_delta_ns += entry->delta_cpu_ns;

			/* Update comm in case it changed */
			get_task_comm(entry->comm, task);
			entry->active = true;
		} else {
			entry->delta_cpu_ns = 0;
			entry->active = false;
		}
		rcu_read_unlock();
	}

	/* Reset cgroup accumulators */
	spin_lock(&powmon_cgroup_lock);
	reset_cgroup_stats();

	/* Second pass: attribute energy proportionally */
	if (total_delta_ns > 0) {
		hash_for_each(pid_table, bkt, entry, node) {
			u64 fraction;
			u64 cpu_uj, dram_uj;

			if (entry->delta_cpu_ns == 0)
				continue;

			/* Proportional attribution: (pid_delta / total_delta) * energy */
			fraction = entry->delta_cpu_ns;
			cpu_uj = div64_u64(total_pkg_delta_uj * fraction, total_delta_ns);
			dram_uj = div64_u64(total_dram_delta_uj * fraction, total_delta_ns);

			entry->cum_cpu_energy_uj += cpu_uj;
			entry->cum_dram_energy_uj += dram_uj;

			/* Instantaneous power */
			entry->last_cpu_power_uw =
				div64_u64(cpu_uj * 1000000000ULL,
					  (u64)sample_interval_ms * 1000000ULL);
			entry->last_dram_power_uw =
				div64_u64(dram_uj * 1000000000ULL,
					  (u64)sample_interval_ms * 1000000ULL);

			entry->last_update = now;

			/* Aggregate to cgroup */
			if (entry->cgroup_id) {
				struct powmon_cgroup_entry *cg;
				cg = find_or_create_cgroup(entry->cgroup_id);
				if (cg) {
					cg->cpu_energy_uj += cpu_uj;
					cg->dram_energy_uj += dram_uj;
					cg->cpu_power_uw += entry->last_cpu_power_uw;
					cg->dram_power_uw += entry->last_dram_power_uw;
					cg->nr_pids++;
				}
			}
		}
	}

	spin_unlock(&powmon_cgroup_lock);
	spin_unlock_irqrestore(&pid_lock, flags);
}

static void powmon_attribute_to_cores(void)
{
	/*
	 * For per-core attribution, if Intel PP0 per-core MSRs are available
	 * (AMD family 19h+), we can read them directly. Otherwise, we
	 * approximate from CPU idle time ratios.
	 *
	 * For AMD Zen, MSR_AMD_CORE_ENERGY_STATUS can be read per-core
	 * via rdmsr on each core's CPU.
	 *
	 * Simplified approach: distribute package PKG energy proportional
	 * to each core's non-idle time.
	 */
	u32 i;

	for (i = 0; i < nr_cores && i < (u32)num_online_cpus(); i++) {
		u32 pkg = cores[i].package_id;
		u64 pkg_delta;

		if (pkg >= nr_packages)
			continue;

		pkg_delta = packages[pkg].delta_energy_uj[POWMON_DOMAIN_PKG];

		/*
		 * Simple equal distribution as baseline.
		 * A production implementation would use per-core idle
		 * counters from /proc/stat or kernel_cpustat.
		 */
		if (nr_cores > 0) {
			u32 cores_in_pkg = 0;
			u32 j;

			for (j = 0; j < nr_cores; j++) {
				if (cores[j].package_id == pkg)
					cores_in_pkg++;
			}

			if (cores_in_pkg > 0) {
				cores[i].attributed_energy_uj =
					div64_u64(pkg_delta, cores_in_pkg);
				cores[i].attributed_power_uw =
					div64_u64(packages[pkg].power_uw[POWMON_DOMAIN_PKG],
						  cores_in_pkg);
			}
		}
	}
}

/* Auto-discover and track new PIDs */
static void powmon_discover_pids(void)
{
	struct task_struct *task;

	if (!tracking_all)
		return;

	rcu_read_lock();
	for_each_process(task) {
		struct powmon_pid_entry *entry;
		unsigned long flags;

		if (task->pid == 0)  /* skip idle */
			continue;

		spin_lock_irqsave(&pid_lock, flags);
		entry = find_pid_entry(task->pid);
		if (!entry)
			create_pid_entry(task);
		spin_unlock_irqrestore(&pid_lock, flags);
	}
	rcu_read_unlock();
}

static void powmon_work_fn(struct work_struct *work)
{
	if (!module_running)
		return;

	/* Discover new PIDs if tracking all */
	powmon_discover_pids();

	/* Sample RAPL counters */
	powmon_sample_rapl();

	/* Attribute energy to PIDs */
	powmon_attribute_to_pids();

	/* Attribute energy to cores */
	powmon_attribute_to_cores();

	/* Reschedule */
	if (module_running)
		queue_delayed_work(powmon_wq, &powmon_work,
				   msecs_to_jiffies(sample_interval_ms));
}

/* ---------- ioctl handlers ---------- */

static long powmon_ioctl_get_info(unsigned long arg)
{
	struct powmon_system_info info = {};
	u32 i;

	info.vendor = cpu_vendor;
	info.nr_packages = nr_packages;
	info.nr_cores = nr_cores;
	info.sample_interval_ms = sample_interval_ms;
	info.uptime_ns = ktime_to_ns(ktime_sub(ktime_get(), module_start_time));
	info.rapl_unit_pkg = raw_energy_unit_pkg;
	info.rapl_unit_dram = raw_energy_unit_dram;

	/* Combine domain masks from all packages */
	for (i = 0; i < nr_packages; i++)
		info.domain_mask |= packages[i].domain_mask;

	/* Count tracked PIDs */
	{
		struct powmon_pid_entry *entry;
		int bkt;
		unsigned long flags;

		spin_lock_irqsave(&pid_lock, flags);
		hash_for_each(pid_table, bkt, entry, node)
			info.nr_tracked_pids++;
		spin_unlock_irqrestore(&pid_lock, flags);
	}

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static long powmon_ioctl_get_pid(unsigned long arg)
{
	struct powmon_query_pid query;
	struct powmon_pid_entry *entry;
	unsigned long flags;

	if (copy_from_user(&query, (void __user *)arg, sizeof(query)))
		return -EFAULT;

	spin_lock_irqsave(&pid_lock, flags);
	entry = find_pid_entry(query.pid);
	if (!entry) {
		spin_unlock_irqrestore(&pid_lock, flags);
		return -ESRCH;
	}

	memset(&query.result, 0, sizeof(query.result));
	query.result.pid = entry->pid;
	query.result.tgid = entry->tgid;
	query.result.cpu_energy_uj = entry->cum_cpu_energy_uj;
	query.result.dram_energy_uj = entry->cum_dram_energy_uj;
	query.result.total_energy_uj =
		entry->cum_cpu_energy_uj + entry->cum_dram_energy_uj;
	query.result.cpu_power_uw = entry->last_cpu_power_uw;
	query.result.dram_power_uw = entry->last_dram_power_uw;
	query.result.total_power_uw =
		entry->last_cpu_power_uw + entry->last_dram_power_uw;
	query.result.cpu_time_ns = entry->total_cpu_time_ns;
	query.result.timestamp_ns = ktime_to_ns(entry->last_update);
	memcpy(query.result.comm, entry->comm, sizeof(query.result.comm));

	spin_unlock_irqrestore(&pid_lock, flags);

	if (copy_to_user((void __user *)arg, &query, sizeof(query)))
		return -EFAULT;

	return 0;
}

static long powmon_ioctl_get_core(unsigned long arg)
{
	struct powmon_query_core query;

	if (copy_from_user(&query, (void __user *)arg, sizeof(query)))
		return -EFAULT;

	if (query.core_id >= nr_cores)
		return -EINVAL;

	memset(&query.result, 0, sizeof(query.result));
	query.result.core_id = query.core_id;
	query.result.package_id = cores[query.core_id].package_id;
	query.result.energy_uj = cores[query.core_id].attributed_energy_uj;
	query.result.power_uw = cores[query.core_id].attributed_power_uw;
	query.result.timestamp_ns = ktime_to_ns(ktime_get());

	if (copy_to_user((void __user *)arg, &query, sizeof(query)))
		return -EFAULT;

	return 0;
}

static long powmon_ioctl_get_package(unsigned long arg)
{
	struct powmon_query_package query;
	u32 d;

	if (copy_from_user(&query, (void __user *)arg, sizeof(query)))
		return -EFAULT;

	if (query.package_id >= nr_packages)
		return -EINVAL;

	memset(&query.result, 0, sizeof(query.result));
	query.result.package_id = query.package_id;
	query.result.domain_mask = packages[query.package_id].domain_mask;

	for (d = 0; d < POWMON_DOMAIN_COUNT; d++) {
		if (!(packages[query.package_id].domain_mask & (1 << d)))
			continue;
		query.result.domains[d].domain = d;
		query.result.domains[d].energy_uj =
			packages[query.package_id].cum_energy_uj[d];
		query.result.domains[d].power_uw =
			packages[query.package_id].power_uw[d];
		query.result.domains[d].timestamp_ns = ktime_to_ns(ktime_get());
	}

	if (copy_to_user((void __user *)arg, &query, sizeof(query)))
		return -EFAULT;

	return 0;
}

static long powmon_ioctl_get_cgroup(unsigned long arg)
{
	struct powmon_query_cgroup query;
	struct powmon_cgroup_entry *entry;
	unsigned long flags;

	if (copy_from_user(&query, (void __user *)arg, sizeof(query)))
		return -EFAULT;

	spin_lock_irqsave(&powmon_cgroup_lock, flags);
	hash_for_each_possible(cgroup_table, entry, node, query.cgroup_id) {
		if (entry->cgroup_id == query.cgroup_id) {
			memset(&query.result, 0, sizeof(query.result));
			query.result.cgroup_id = entry->cgroup_id;
			query.result.cpu_energy_uj = entry->cpu_energy_uj;
			query.result.dram_energy_uj = entry->dram_energy_uj;
			query.result.total_energy_uj =
				entry->cpu_energy_uj + entry->dram_energy_uj;
			query.result.cpu_power_uw = entry->cpu_power_uw;
			query.result.dram_power_uw = entry->dram_power_uw;
			query.result.total_power_uw =
				entry->cpu_power_uw + entry->dram_power_uw;
			query.result.nr_pids = entry->nr_pids;
			query.result.timestamp_ns = ktime_to_ns(ktime_get());
			memcpy(query.result.path, entry->path,
			       sizeof(query.result.path));
			spin_unlock_irqrestore(&powmon_cgroup_lock, flags);

			if (copy_to_user((void __user *)arg, &query, sizeof(query)))
				return -EFAULT;
			return 0;
		}
	}
	spin_unlock_irqrestore(&powmon_cgroup_lock, flags);

	return -ENOENT;
}

static long powmon_ioctl_set_config(unsigned long arg)
{
	struct powmon_config config;

	if (copy_from_user(&config, (void __user *)arg, sizeof(config)))
		return -EFAULT;

	if (config.sample_interval_ms < POWMON_CONFIG_MIN_INTERVAL_MS ||
	    config.sample_interval_ms > POWMON_CONFIG_MAX_INTERVAL_MS)
		return -EINVAL;

	sample_interval_ms = config.sample_interval_ms;
	pr_info("Sample interval changed to %u ms\n", sample_interval_ms);
	return 0;
}

static long powmon_ioctl_get_config(unsigned long arg)
{
	struct powmon_config config = {
		.sample_interval_ms = sample_interval_ms,
		.flags = 0,
	};

	if (copy_to_user((void __user *)arg, &config, sizeof(config)))
		return -EFAULT;

	return 0;
}

static long powmon_ioctl_reset(void)
{
	struct powmon_pid_entry *pid_entry;
	struct hlist_node *tmp;
	int bkt;
	unsigned long flags;

	spin_lock_irqsave(&pid_lock, flags);
	hash_for_each_safe(pid_table, bkt, tmp, pid_entry, node) {
		hash_del(&pid_entry->node);
		kfree(pid_entry);
	}
	spin_unlock_irqrestore(&pid_lock, flags);

	spin_lock_irqsave(&powmon_cgroup_lock, flags);
	{
		struct powmon_cgroup_entry *cg_entry;
		hash_for_each_safe(cgroup_table, bkt, tmp, cg_entry, node) {
			hash_del(&cg_entry->node);
			kfree(cg_entry);
		}
	}
	spin_unlock_irqrestore(&powmon_cgroup_lock, flags);

	pr_info("Stats reset\n");
	return 0;
}

/* ---------- File operations ---------- */

static long powmon_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case POWMON_IOC_GET_INFO:
		return powmon_ioctl_get_info(arg);
	case POWMON_IOC_GET_PID:
		return powmon_ioctl_get_pid(arg);
	case POWMON_IOC_GET_CORE:
		return powmon_ioctl_get_core(arg);
	case POWMON_IOC_GET_PACKAGE:
		return powmon_ioctl_get_package(arg);
	case POWMON_IOC_GET_CGROUP:
		return powmon_ioctl_get_cgroup(arg);
	case POWMON_IOC_SET_CONFIG:
		return powmon_ioctl_set_config(arg);
	case POWMON_IOC_GET_CONFIG:
		return powmon_ioctl_get_config(arg);
	case POWMON_IOC_RESET_STATS:
		return powmon_ioctl_reset();
	case POWMON_IOC_START_TRACKING: {
		__s32 pid;
		if (copy_from_user(&pid, (void __user *)arg, sizeof(pid)))
			return -EFAULT;
		return powmon_start_tracking_pid(pid);
	}
	case POWMON_IOC_STOP_TRACKING: {
		__s32 pid;
		if (copy_from_user(&pid, (void __user *)arg, sizeof(pid)))
			return -EFAULT;
		return powmon_stop_tracking_pid(pid);
	}
	case POWMON_IOC_TRACK_ALL:
		tracking_all = true;
		pr_info("Tracking all processes\n");
		return 0;
	default:
		return -ENOTTY;
	}
}

static int powmon_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int powmon_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations powmon_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = powmon_ioctl,
	.compat_ioctl   = powmon_ioctl,
	.open           = powmon_open,
	.release        = powmon_release,
};

static struct miscdevice powmon_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = POWMON_DEVICE_NAME,
	.fops  = &powmon_fops,
	.mode  = 0666,
};

/* ---------- Module init / exit ---------- */

static int __init powmon_init(void)
{
	int ret;

	pr_info("Initializing power monitor module\n");

	ret = powmon_detect_vendor();
	if (ret)
		return ret;

	ret = powmon_detect_topology();
	if (ret)
		return ret;

	ret = powmon_read_energy_units();
	if (ret) {
		pr_err("Failed to read RAPL energy units: %d\n", ret);
		return ret;
	}

	ret = powmon_init_packages();
	if (ret)
		goto err_packages;

	ret = powmon_init_cores();
	if (ret)
		goto err_cores;

	/* Register character device */
	ret = misc_register(&powmon_misc);
	if (ret) {
		pr_err("Failed to register misc device: %d\n", ret);
		goto err_misc;
	}

	/* Create workqueue and start sampling */
	powmon_wq = create_singlethread_workqueue("powmon_wq");
	if (!powmon_wq) {
		ret = -ENOMEM;
		goto err_wq;
	}

	module_start_time = ktime_get();
	last_sample_time = module_start_time;
	module_running = true;
	tracking_all = track_all;

	INIT_DELAYED_WORK(&powmon_work, powmon_work_fn);
	queue_delayed_work(powmon_wq, &powmon_work,
			   msecs_to_jiffies(sample_interval_ms));

	pr_info("Module loaded - /dev/%s ready (interval=%ums, track_all=%d)\n",
		POWMON_DEVICE_NAME, sample_interval_ms, track_all);

	return 0;

err_wq:
	misc_deregister(&powmon_misc);
err_misc:
	kfree(cores);
err_cores:
	kfree(packages);
err_packages:
	return ret;
}

static void __exit powmon_exit(void)
{
	struct powmon_pid_entry *pid_entry;
	struct powmon_cgroup_entry *cg_entry;
	struct hlist_node *tmp;
	int bkt;

	pr_info("Unloading power monitor module\n");

	module_running = false;

	cancel_delayed_work_sync(&powmon_work);
	destroy_workqueue(powmon_wq);

	misc_deregister(&powmon_misc);

	/* Free PID entries */
	hash_for_each_safe(pid_table, bkt, tmp, pid_entry, node) {
		hash_del(&pid_entry->node);
		kfree(pid_entry);
	}

	/* Free cgroup entries */
	hash_for_each_safe(cgroup_table, bkt, tmp, cg_entry, node) {
		hash_del(&cg_entry->node);
		kfree(cg_entry);
	}

	kfree(cores);
	kfree(packages);

	pr_info("Module unloaded\n");
}

module_init(powmon_init);
module_exit(powmon_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("powmon");
MODULE_DESCRIPTION("Per-process power monitoring via RAPL MSRs");
MODULE_VERSION("1.0");
