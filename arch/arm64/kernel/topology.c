/*
 * arch/arm64/kernel/topology.c
 *
 * Copyright (C) 2011,2013,2014 Linaro Limited.
 *
 * Based on the arm32 version written by Vincent Guittot in turn based on
 * arch/sh/kernel/topology.c
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/percpu.h>
#include <linux/node.h>
#include <linux/nodemask.h>
#include <linux/of.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/cpufreq.h>

#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/topology.h>

static DEFINE_PER_CPU(unsigned long, cpu_scale) = SCHED_CAPACITY_SCALE;
static DEFINE_MUTEX(cpu_scale_mutex);

unsigned long arch_scale_cpu_capacity(struct sched_domain *sd, int cpu)
{
	return per_cpu(cpu_scale, cpu);
}

static void set_capacity_scale(unsigned int cpu, unsigned long capacity)
{
	per_cpu(cpu_scale, cpu) = capacity;
}

#ifdef CONFIG_PROC_SYSCTL
static ssize_t cpu_capacity_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct cpu *cpu = container_of(dev, struct cpu, dev);

	return sprintf(buf, "%lu\n",
			arch_scale_cpu_capacity(NULL, cpu->dev.id));
}

static ssize_t cpu_capacity_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t count)
{
	struct cpu *cpu = container_of(dev, struct cpu, dev);
	int this_cpu = cpu->dev.id, i;
	unsigned long new_capacity;
	ssize_t ret;

	if (count) {
		ret = kstrtoul(buf, 0, &new_capacity);
		if (ret)
			return ret;
		if (new_capacity > SCHED_CAPACITY_SCALE)
			return -EINVAL;

		mutex_lock(&cpu_scale_mutex);
		for_each_cpu(i, &cpu_topology[this_cpu].core_sibling)
			set_capacity_scale(i, new_capacity);
		mutex_unlock(&cpu_scale_mutex);
	}

	return count;
}

static DEVICE_ATTR_RW(cpu_capacity);

static int register_cpu_capacity_sysctl(void)
{
	int i;
	struct device *cpu;

	for_each_possible_cpu(i) {
		cpu = get_cpu_device(i);
		if (!cpu) {
			pr_err("%s: too early to get CPU%d device!\n",
			       __func__, i);
			continue;
		}
		device_create_file(cpu, &dev_attr_cpu_capacity);
	}

	return 0;
}
subsys_initcall(register_cpu_capacity_sysctl);
#endif

static u32 capacity_scale;
static u32 *raw_capacity;
static bool cap_parsing_failed;

static void __init parse_cpu_capacity(struct device_node *cpu_node, int cpu)
{
	int ret;
	u32 cpu_capacity;

	if (cap_parsing_failed)
		return;

	ret = of_property_read_u32(cpu_node,
				   "capacity-dmips-mhz",
				   &cpu_capacity);
	if (!ret) {
		if (!raw_capacity) {
			raw_capacity = kcalloc(num_possible_cpus(),
					       sizeof(*raw_capacity),
					       GFP_KERNEL);
			if (!raw_capacity) {
				pr_err("cpu_capacity: failed to allocate memory for raw capacities\n");
				cap_parsing_failed = true;
				return;
			}
		}
		capacity_scale = max(cpu_capacity, capacity_scale);
		raw_capacity[cpu] = cpu_capacity;
		pr_debug("cpu_capacity: %s cpu_capacity=%u (raw)\n",
			cpu_node->full_name, raw_capacity[cpu]);
	} else {
		if (raw_capacity) {
			pr_err("cpu_capacity: missing %s raw capacity\n",
				cpu_node->full_name);
			pr_err("cpu_capacity: partial information: fallback to 1024 for all CPUs\n");
		}
		cap_parsing_failed = true;
		kfree(raw_capacity);
	}
}

static void normalize_cpu_capacity(void)
{
	u64 capacity;
	int cpu;

	if (!raw_capacity || cap_parsing_failed)
		return;

	pr_debug("cpu_capacity: capacity_scale=%u\n", capacity_scale);
	mutex_lock(&cpu_scale_mutex);
	for_each_possible_cpu(cpu) {
		pr_debug("cpu_capacity: cpu=%d raw_capacity=%u\n",
			 cpu, raw_capacity[cpu]);
		capacity = (raw_capacity[cpu] << SCHED_CAPACITY_SHIFT)
			/ capacity_scale;
		set_capacity_scale(cpu, capacity);
		pr_debug("cpu_capacity: CPU%d cpu_capacity=%lu\n",
			cpu, arch_scale_cpu_capacity(NULL, cpu));
	}
	mutex_unlock(&cpu_scale_mutex);
}

#ifdef CONFIG_CPU_FREQ
static cpumask_var_t cpus_to_visit;
static bool cap_parsing_done;
static void parsing_done_workfn(struct work_struct *work);
static DECLARE_WORK(parsing_done_work, parsing_done_workfn);

static int
init_cpu_capacity_callback(struct notifier_block *nb,
			   unsigned long val,
			   void *data)
{
	struct cpufreq_policy *policy = data;
	int cpu;

	if (cap_parsing_failed || cap_parsing_done)
		return 0;

	switch (val) {
	case CPUFREQ_NOTIFY:
		pr_debug("cpu_capacity: init cpu capacity for CPUs [%*pbl] (to_visit=%*pbl)\n",
				cpumask_pr_args(policy->related_cpus),
				cpumask_pr_args(cpus_to_visit));
		cpumask_andnot(cpus_to_visit,
			       cpus_to_visit,
			       policy->related_cpus);
		for_each_cpu(cpu, policy->related_cpus) {
			raw_capacity[cpu] = arch_scale_cpu_capacity(NULL, cpu) *
					    policy->cpuinfo.max_freq / 1000UL;
			capacity_scale = max(raw_capacity[cpu], capacity_scale);
		}
		if (cpumask_empty(cpus_to_visit)) {
			normalize_cpu_capacity();
			kfree(raw_capacity);
			pr_debug("cpu_capacity: parsing done\n");
			cap_parsing_done = true;
			schedule_work(&parsing_done_work);
		}
	}
	return 0;
}

static struct notifier_block init_cpu_capacity_notifier = {
	.notifier_call = init_cpu_capacity_callback,
};

static int __init register_cpufreq_notifier(void)
{
	if (cap_parsing_failed)
		return -EINVAL;

	if (!alloc_cpumask_var(&cpus_to_visit, GFP_KERNEL)) {
		pr_err("cpu_capacity: failed to allocate memory for cpus_to_visit\n");
		return -ENOMEM;
	}
	cpumask_copy(cpus_to_visit, cpu_possible_mask);

	return cpufreq_register_notifier(&init_cpu_capacity_notifier,
					 CPUFREQ_POLICY_NOTIFIER);
}
core_initcall(register_cpufreq_notifier);

static void parsing_done_workfn(struct work_struct *work)
{
	cpufreq_unregister_notifier(&init_cpu_capacity_notifier,
					 CPUFREQ_POLICY_NOTIFIER);
}

#else
static int __init free_raw_capacity(void)
{
	kfree(raw_capacity);

	return 0;
}
core_initcall(free_raw_capacity);
#endif

static int __init get_cpu_for_node(struct device_node *node)
{
	struct device_node *cpu_node;
	int cpu;

	cpu_node = of_parse_phandle(node, "cpu", 0);
	if (!cpu_node)
		return -1;

	for_each_possible_cpu(cpu) {
		if (of_get_cpu_node(cpu, NULL) == cpu_node) {
			parse_cpu_capacity(cpu_node, cpu);
			of_node_put(cpu_node);
			return cpu;
		}
	}

	pr_crit("Unable to find CPU node for %s\n", cpu_node->full_name);

	of_node_put(cpu_node);
	return -1;
}

static int __init parse_core(struct device_node *core, int cluster_id,
			     int core_id)
{
	char name[10];
	bool leaf = true;
	int i = 0;
	int cpu;
	struct device_node *t;

	do {
		snprintf(name, sizeof(name), "thread%d", i);
		t = of_get_child_by_name(core, name);
		if (t) {
			leaf = false;
			cpu = get_cpu_for_node(t);
			if (cpu >= 0) {
				cpu_topology[cpu].cluster_id = cluster_id;
				cpu_topology[cpu].core_id = core_id;
				cpu_topology[cpu].thread_id = i;
			} else {
				pr_err("%s: Can't get CPU for thread\n",
				       t->full_name);
				of_node_put(t);
				return -EINVAL;
			}
			of_node_put(t);
		}
		i++;
	} while (t);

	cpu = get_cpu_for_node(core);
	if (cpu >= 0) {
		if (!leaf) {
			pr_err("%s: Core has both threads and CPU\n",
			       core->full_name);
			return -EINVAL;
		}

		cpu_topology[cpu].cluster_id = cluster_id;
		cpu_topology[cpu].core_id = core_id;
	} else if (leaf) {
		pr_err("%s: Can't get CPU for leaf core\n", core->full_name);
		return -EINVAL;
	}

	return 0;
}

static int __init parse_cluster(struct device_node *cluster, int depth)
{
	char name[10];
	bool leaf = true;
	bool has_cores = false;
	struct device_node *c;
	static int cluster_id __initdata;
	int core_id = 0;
	int i, ret;

	/*
	 * First check for child clusters; we currently ignore any
	 * information about the nesting of clusters and present the
	 * scheduler with a flat list of them.
	 */
	i = 0;
	do {
		snprintf(name, sizeof(name), "cluster%d", i);
		c = of_get_child_by_name(cluster, name);
		if (c) {
			leaf = false;
			ret = parse_cluster(c, depth + 1);
			of_node_put(c);
			if (ret != 0)
				return ret;
		}
		i++;
	} while (c);

	/* Now check for cores */
	i = 0;
	do {
		snprintf(name, sizeof(name), "core%d", i);
		c = of_get_child_by_name(cluster, name);
		if (c) {
			has_cores = true;

			if (depth == 0) {
				pr_err("%s: cpu-map children should be clusters\n",
				       c->full_name);
				of_node_put(c);
				return -EINVAL;
			}

			if (leaf) {
				ret = parse_core(c, cluster_id, core_id++);
			} else {
				pr_err("%s: Non-leaf cluster with core %s\n",
				       cluster->full_name, name);
				ret = -EINVAL;
			}

			of_node_put(c);
			if (ret != 0)
				return ret;
		}
		i++;
	} while (c);

	if (leaf && !has_cores)
		pr_warn("%s: empty cluster\n", cluster->full_name);

	if (leaf)
		cluster_id++;

	return 0;
}

struct cpu_efficiency {
	const char *compatible;
	unsigned long efficiency;
};

/*
 * Table of relative efficiency of each processors
 * The efficiency value must fit in 20bit and the final
 * cpu_scale value must be in the range
 *   0 < cpu_scale < 3*SCHED_CAPACITY_SCALE/2
 * in order to return at most 1 when DIV_ROUND_CLOSEST
 * is used to compute the capacity of a CPU.
 * Processors that are not defined in the table,
 * use the default SCHED_CAPACITY_SCALE value for cpu_scale.
 */
static const struct cpu_efficiency table_efficiency[] = {
	{ NULL, },
};

static unsigned long *__cpu_capacity;
#define cpu_capacity(cpu)	__cpu_capacity[cpu]

static unsigned long middle_capacity = 1;

static DEFINE_PER_CPU(unsigned long, cpu_efficiency) = SCHED_CAPACITY_SCALE;

unsigned long arch_get_cpu_efficiency(int cpu)
{
	return per_cpu(cpu_efficiency, cpu);
}
EXPORT_SYMBOL(arch_get_cpu_efficiency);

/*
 * Iterate all CPUs' descriptor in DT and compute the efficiency
 * (as per table_efficiency). Also calculate a middle efficiency
 * as close as possible to  (max{eff_i} - min{eff_i}) / 2
 * This is later used to scale the cpu_power field such that an
 * 'average' CPU is of middle power. Also see the comments near
 * table_efficiency[] and update_cpu_power().
 */
static int __init parse_dt_topology(void)
{
	struct device_node *cn, *map;
	int ret = 0;
	int cpu;

	cn = of_find_node_by_path("/cpus");
	if (!cn) {
		pr_err("No CPU information found in DT\n");
		return 0;
	}

	/*
	 * When topology is provided cpu-map is essentially a root
	 * cluster with restricted subnodes.
	 */
	map = of_get_child_by_name(cn, "cpu-map");
	if (!map) {
		cap_parsing_failed = true;
		goto out;
	}

	ret = parse_cluster(map, 0);
	if (ret != 0)
		goto out_map;

	normalize_cpu_capacity();

	/*
	 * Check that all cores are in the topology; the SMP code will
	 * only mark cores described in the DT as possible.
	 */
	for_each_possible_cpu(cpu)
		if (cpu_topology[cpu].cluster_id == -1)
			ret = -EINVAL;

out_map:
	of_node_put(map);
out:
	of_node_put(cn);
	return ret;
}

static void __init parse_dt_cpu_power(void)
{
	const struct cpu_efficiency *cpu_eff;
	struct device_node *cn;
	unsigned long min_capacity = ULONG_MAX;
	unsigned long max_capacity = 0;
	unsigned long capacity = 0;
	int cpu;

	__cpu_capacity = kcalloc(nr_cpu_ids, sizeof(*__cpu_capacity),
				 GFP_NOWAIT);

	for_each_possible_cpu(cpu) {
		const u32 *rate;
		int len;
		u32 efficiency;

		/* Too early to use cpu->of_node */
		cn = of_get_cpu_node(cpu, NULL);
		if (!cn) {
			pr_err("Missing device node for CPU %d\n", cpu);
			continue;
		}

		/*
		 * The CPU efficiency value passed from the device tree
		 * overrides the value defined in the table_efficiency[]
		 */
		if (of_property_read_u32(cn, "efficiency", &efficiency) < 0) {

			for (cpu_eff = table_efficiency;
					cpu_eff->compatible; cpu_eff++)

				if (of_device_is_compatible(cn,
						cpu_eff->compatible))
					break;

			if (cpu_eff->compatible == NULL) {
				pr_warn("%s: Unknown CPU type\n",
						cn->full_name);
				continue;
			}

			efficiency = cpu_eff->efficiency;
		}

		per_cpu(cpu_efficiency, cpu) = efficiency;

		rate = of_get_property(cn, "clock-frequency", &len);
		if (!rate || len != 4) {
			pr_err("%s: Missing clock-frequency property\n",
				cn->full_name);
			continue;
		}

		capacity = ((be32_to_cpup(rate)) >> 20) * efficiency;

		/* Save min capacity of the system */
		if (capacity < min_capacity)
			min_capacity = capacity;

		/* Save max capacity of the system */
		if (capacity > max_capacity)
			max_capacity = capacity;

		cpu_capacity(cpu) = capacity;
	}

	/* If min and max capacities are equal we bypass the update of the
	 * cpu_scale because all CPUs have the same capacity. Otherwise, we
	 * compute a middle_capacity factor that will ensure that the capacity
	 * of an 'average' CPU of the system will be as close as possible to
	 * SCHED_CAPACITY_SCALE, which is the default value, but with the
	 * constraint explained near table_efficiency[].
	 */
	if (min_capacity == max_capacity)
		return;
	else if (4 * max_capacity < (3 * (max_capacity + min_capacity)))
		middle_capacity = (min_capacity + max_capacity)
				>> (SCHED_CAPACITY_SHIFT+1);
	else
		middle_capacity = ((max_capacity / 3)
				>> (SCHED_CAPACITY_SHIFT-1)) + 1;
}

/*
 * Look for a customed capacity of a CPU in the cpu_topo_data table during the
 * boot. The update of all CPUs is in O(n^2) for heteregeneous system but the
 * function returns directly for SMP system.
 */
static void update_cpu_power(unsigned int cpu)
{
	if (!cpu_capacity(cpu))
		return;

	set_power_scale(cpu, cpu_capacity(cpu) / middle_capacity);

	pr_info("CPU%u: update cpu_power %lu\n",
		cpu, arch_scale_freq_power(NULL, cpu));
}

/*
 * cpu topology table
 */
struct cpu_topology cpu_topology[NR_CPUS];
EXPORT_SYMBOL_GPL(cpu_topology);

/* sd energy functions */
static inline
const struct sched_group_energy * const cpu_cluster_energy(int cpu)
{
	struct sched_group_energy *sge = sge_array[cpu][SD_LEVEL1];

	if (sched_is_energy_aware() && !sge) {
		pr_warn("Invalid sched_group_energy for Cluster%d\n", cpu);
		return NULL;
	}

	return sge;
}

static inline
const struct sched_group_energy * const cpu_core_energy(int cpu)
{
	struct sched_group_energy *sge = sge_array[cpu][SD_LEVEL0];

	if (sched_is_energy_aware() && !sge) {
		pr_warn("Invalid sched_group_energy for CPU%d\n", cpu);
		return NULL;
	}

	return sge;
}

const struct cpumask *cpu_coregroup_mask(int cpu)
{
	return &cpu_topology[cpu].core_sibling;
}

static int cpu_cpu_flags(void)
{
	return SD_ASYM_CPUCAPACITY;
}

static inline int cpu_corepower_flags(void)
{
	return SD_SHARE_PKG_RESOURCES  | SD_SHARE_POWERDOMAIN | \
	       SD_SHARE_CAP_STATES;
}

static struct sched_domain_topology_level arm64_topology[] = {
#ifdef CONFIG_SCHED_MC
	{ cpu_coregroup_mask, cpu_corepower_flags, cpu_core_energy, SD_INIT_NAME(MC) },
#endif
	{ cpu_cpu_mask, cpu_cpu_flags, cpu_cluster_energy, SD_INIT_NAME(DIE) },
	{ NULL, },
};

static void update_cpu_capacity(unsigned int cpu)
{
	unsigned long capacity = SCHED_CAPACITY_SCALE;

	if (cpu_core_energy(cpu)) {
		int max_cap_idx = cpu_core_energy(cpu)->nr_cap_states - 1;
		capacity = cpu_core_energy(cpu)->cap_states[max_cap_idx].cap;
	}

	set_capacity_scale(cpu, capacity);

	pr_info("CPU%d: update cpu_capacity %lu\n",
		cpu, arch_scale_cpu_capacity(NULL, cpu));
}

void update_cpu_power_capacity(int cpu)
{
	update_cpu_power(cpu);
	update_cpu_capacity(cpu);
}

static void update_siblings_masks(unsigned int cpuid)
{
	struct cpu_topology *cpu_topo, *cpuid_topo = &cpu_topology[cpuid];
	int cpu;

	/* update core and thread sibling masks */
	for_each_possible_cpu(cpu) {
		cpu_topo = &cpu_topology[cpu];

		if (cpuid_topo->cluster_id != cpu_topo->cluster_id)
			continue;

		cpumask_set_cpu(cpuid, &cpu_topo->core_sibling);
		if (cpu != cpuid)
			cpumask_set_cpu(cpu, &cpuid_topo->core_sibling);

		if (cpuid_topo->core_id != cpu_topo->core_id)
			continue;

		cpumask_set_cpu(cpuid, &cpu_topo->thread_sibling);
		if (cpu != cpuid)
			cpumask_set_cpu(cpu, &cpuid_topo->thread_sibling);
	}
}

void store_cpu_topology(unsigned int cpuid)
{
	struct cpu_topology *cpuid_topo = &cpu_topology[cpuid];
	u64 mpidr;

	if (cpuid_topo->cluster_id != -1)
		goto topology_populated;

	mpidr = read_cpuid_mpidr();

	/* Uniprocessor systems can rely on default topology values */
	if (mpidr & MPIDR_UP_BITMASK)
		return;

	/* Create cpu topology mapping based on MPIDR. */
	if (mpidr & MPIDR_MT_BITMASK) {
		/* Multiprocessor system : Multi-threads per core */
		cpuid_topo->thread_id  = MPIDR_AFFINITY_LEVEL(mpidr, 0);
		cpuid_topo->core_id    = MPIDR_AFFINITY_LEVEL(mpidr, 1);
		cpuid_topo->cluster_id = MPIDR_AFFINITY_LEVEL(mpidr, 2) |
					 MPIDR_AFFINITY_LEVEL(mpidr, 3) << 8;
	} else {
		/* Multiprocessor system : Single-thread per core */
		cpuid_topo->thread_id  = -1;
		cpuid_topo->core_id    = MPIDR_AFFINITY_LEVEL(mpidr, 0);
		cpuid_topo->cluster_id = MPIDR_AFFINITY_LEVEL(mpidr, 1) |
					 MPIDR_AFFINITY_LEVEL(mpidr, 2) << 8 |
					 MPIDR_AFFINITY_LEVEL(mpidr, 3) << 16;
	}

	pr_debug("CPU%u: cluster %d core %d thread %d mpidr %#016llx\n",
		 cpuid, cpuid_topo->cluster_id, cpuid_topo->core_id,
		 cpuid_topo->thread_id, mpidr);

topology_populated:
	update_siblings_masks(cpuid);
}

static void __init reset_cpu_topology(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		struct cpu_topology *cpu_topo = &cpu_topology[cpu];

		cpu_topo->thread_id = -1;
		cpu_topo->core_id = 0;
		cpu_topo->cluster_id = -1;

		cpumask_clear(&cpu_topo->core_sibling);
		cpumask_set_cpu(cpu, &cpu_topo->core_sibling);
		cpumask_clear(&cpu_topo->thread_sibling);
		cpumask_set_cpu(cpu, &cpu_topo->thread_sibling);
	}
}

static void __init reset_cpu_power(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu)
		set_power_scale(cpu, SCHED_CAPACITY_SCALE);
}

void __init init_cpu_topology(void)
{
	int cpu;

	reset_cpu_topology();

	/*
	 * Discard anything that was parsed if we hit an error so we
	 * don't use partial information.
	 */
	if (of_have_populated_dt() && parse_dt_topology()) {
		reset_cpu_topology();
	} else {
		set_sched_topology(arm64_topology);
		for_each_possible_cpu(cpu)
			update_siblings_masks(cpu);
	}

	reset_cpu_power();
	parse_dt_cpu_power();
	init_sched_energy_costs();
}
