// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J713 Linux operating-temperature policy. These are not Apple safety limits.
 * PMP remains firmware-owned; this client only reads its documented records.
 */

#include <linux/cpu.h>
#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/soc/apple/pmp-temps.h>
#include <linux/thermal.h>

#define APPLE_PMP_PASSIVE_MC	70000
#define APPLE_PMP_HYST_MC		10000
#define APPLE_PMP_POLL_MS		25

struct apple_pmp_cluster {
	struct cpufreq_policy *policy;
	struct thermal_cooling_device *cdev;
	struct freq_qos_request guard;
	unsigned long floor;
};

static struct {
	void __iomem *sram;
	struct thermal_zone_device *zone;
	struct apple_pmp_cluster clusters[2];
	struct mutex lock; /* Serializes sensor reads, mode changes and guard updates. */
	unsigned int good_reads;
	bool enabled;
} pmp_thermal;

/* Fail closed independently of the governor, including while the zone is off. */
static int apple_pmp_guard(bool clamp)
{
	unsigned int i;
	int ret, err = 0;

	for (i = 0; i < ARRAY_SIZE(pmp_thermal.clusters); i++) {
		struct apple_pmp_cluster *c = &pmp_thermal.clusters[i];

		ret = freq_qos_update_request(&c->guard, clamp ?
					     c->policy->cpuinfo.min_freq :
					     FREQ_QOS_MAX_DEFAULT_VALUE);
		if (ret < 0)
			err = ret;
	}
	return err;
}

static bool apple_pmp_cooling_ready(void)
{
	unsigned int i;
	unsigned long state;

	for (i = 0; i < ARRAY_SIZE(pmp_thermal.clusters); i++) {
		struct apple_pmp_cluster *c = &pmp_thermal.clusters[i];

		if (c->cdev->ops->get_cur_state(c->cdev, &state) || state < c->floor)
			return false;
	}
	return true;
}

static int apple_pmp_get_temp(struct thermal_zone_device *zone, int *temp)
{
	long value;
	int ret;

	guard(mutex)(&pmp_thermal.lock);
	ret = apple_pmp_temp_hotspot(pmp_thermal.sram, &value);
	if (ret) {
		pmp_thermal.good_reads = 0;
		apple_pmp_guard(true);
		pr_warn_ratelimited("apple-pmp-thermal: sensor error %d; CPU minimum caps retained\n",
				    ret);
		return ret;
	}

	*temp = value;
	if (pmp_thermal.good_reads < 3)
		pmp_thermal.good_reads++;
	/* Do not remove startup/fault protection before hot-zone cooling is active. */
	if (pmp_thermal.enabled && pmp_thermal.good_reads == 3 &&
	    (value < APPLE_PMP_PASSIVE_MC || apple_pmp_cooling_ready())) {
		ret = apple_pmp_guard(false);
		if (ret)
			apple_pmp_guard(true);
	}
	return ret;
}

static int apple_pmp_change_mode(struct thermal_zone_device *zone,
				 enum thermal_device_mode mode)
{
	guard(mutex)(&pmp_thermal.lock);
	pmp_thermal.enabled = mode == THERMAL_DEVICE_ENABLED;
	pmp_thermal.good_reads = 0;
	return apple_pmp_guard(true);
}

static bool apple_pmp_should_bind(struct thermal_zone_device *zone,
				  const struct thermal_trip *trip,
				  struct thermal_cooling_device *cdev,
				  struct cooling_spec *spec)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pmp_thermal.clusters); i++) {
		struct apple_pmp_cluster *c = &pmp_thermal.clusters[i];

		if (cdev != c->cdev)
			continue;
		spec->lower = c->floor;
		spec->upper = cdev->max_state;
		return true;
	}
	return false;
}

static const struct thermal_zone_device_ops apple_pmp_thermal_ops = {
	.get_temp = apple_pmp_get_temp,
	.change_mode = apple_pmp_change_mode,
	.should_bind = apple_pmp_should_bind,
};

static const struct thermal_trip apple_pmp_trips[] = {
	{
		.temperature = APPLE_PMP_PASSIVE_MC,
		.hysteresis = APPLE_PMP_HYST_MC,
		.type = THERMAL_TRIP_PASSIVE,
	},
};

static const struct thermal_zone_params apple_pmp_params = {
	.governor_name = "step_wise",
	.no_hwmon = true, /* Keep the existing six-channel hwmon ABI. */
};

static int apple_pmp_check_bindings(struct thermal_trip *trip, void *data)
{
	struct thermal_zone_device *zone = data;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pmp_thermal.clusters); i++)
		if (!thermal_trip_is_bound_to_cdev(zone, trip, pmp_thermal.clusters[i].cdev))
			return -ENODEV;
	return 0;
}

static void apple_pmp_thermal_cleanup(void)
{
	unsigned int i;

	if (pmp_thermal.zone) {
		thermal_zone_device_unregister(pmp_thermal.zone);
		pmp_thermal.zone = NULL;
	}
	for (i = 0; i < ARRAY_SIZE(pmp_thermal.clusters); i++) {
		struct apple_pmp_cluster *c = &pmp_thermal.clusters[i];

		cpufreq_cooling_unregister(c->cdev);
		if (freq_qos_request_active(&c->guard))
			freq_qos_remove_request(&c->guard);
		if (c->policy)
			cpufreq_cpu_put(c->policy);
	}
	if (pmp_thermal.sram)
		iounmap(pmp_thermal.sram);
}

static int apple_pmp_add_cluster(unsigned int index, unsigned int cpu,
				 unsigned long members, unsigned int cap)
{
	struct apple_pmp_cluster *c = &pmp_thermal.clusters[index];
	struct cpufreq_frequency_table *entry;
	unsigned int n;
	bool found = false;
	int ret;

	c->policy = cpufreq_cpu_get(cpu);
	if (!c->policy)
		return -EPROBE_DEFER;
	for_each_possible_cpu(n)
		if (cpumask_test_cpu(n, c->policy->related_cpus) !=
		    (n < BITS_PER_LONG && !!(members & BIT(n))))
			return -EINVAL;
	if (!c->policy->freq_table)
		return -EINVAL;
	cpufreq_for_each_valid_entry(entry, c->policy->freq_table) {
		if (entry->frequency == cap)
			found = true;
		if (entry->frequency > cap)
			c->floor++;
	}
	if (!found)
		return -EINVAL;

	ret = freq_qos_add_request(&c->policy->constraints, &c->guard,
				   FREQ_QOS_MAX, c->policy->cpuinfo.min_freq);
	if (ret < 0)
		return ret;
	c->cdev = cpufreq_cooling_register(c->policy);
	if (IS_ERR(c->cdev)) {
		ret = PTR_ERR(c->cdev);
		c->cdev = NULL;
		return ret;
	}
	return 0;
}

static int __init apple_pmp_thermal_init(void)
{
	struct thermal_zone_device *zone;
	struct device_node *np;
	struct platform_device *report;
	int ret;

	/* Board policy is opt-in; never guess the sensor layout on another Mac. */
	if (!of_machine_is_compatible("apple,j713"))
		return -ENODEV;
	np = of_find_compatible_node(NULL, NULL, "apple,t8132-pmp-v2-report");
	if (!np)
		return -ENODEV;
	report = of_find_device_by_node(np);
	ret = of_device_is_available(np) && report && device_is_bound(&report->dev) ?
		0 : -EPROBE_DEFER;
	if (report)
		put_device(&report->dev);
	of_node_put(np);
	if (ret)
		return ret;

	mutex_init(&pmp_thermal.lock);
	/* Independent read-only mapping permits insmod without rebinding PMP. */
	pmp_thermal.sram = ioremap(PMP_TEMP_SRAM_BASE, PMP_TEMP_SRAM_SIZE);
	if (!pmp_thermal.sram)
		return -ENOMEM;
	ret = apple_pmp_add_cluster(0, 0, BIT(0) | GENMASK(9, 7), 2616000);
	if (ret)
		goto fail;
	ret = apple_pmp_add_cluster(1, 1, GENMASK(6, 1), 1860000);
	if (ret)
		goto fail;
	zone = thermal_zone_device_register_with_trips("apple-pmp-die", apple_pmp_trips,
						       ARRAY_SIZE(apple_pmp_trips), NULL,
						       &apple_pmp_thermal_ops, &apple_pmp_params,
						       APPLE_PMP_POLL_MS, APPLE_PMP_POLL_MS);
	pmp_thermal.zone = zone;
	if (IS_ERR(pmp_thermal.zone)) {
		ret = PTR_ERR(pmp_thermal.zone);
		pmp_thermal.zone = NULL;
		goto fail;
	}
	/* Trip array is immutable; the binding query takes the zone lock itself. */
	ret = for_each_thermal_trip(pmp_thermal.zone, apple_pmp_check_bindings,
				    pmp_thermal.zone);
	if (ret)
		goto fail;
	ret = thermal_zone_device_enable(pmp_thermal.zone);
	if (ret)
		goto fail;
	pr_info("apple-pmp-thermal: J713 70 C passive trip, 10 C hysteresis, 25 ms polling; target ~80 C\n");
	return 0;
fail:
	apple_pmp_thermal_cleanup();
	return ret;
}

static void __exit apple_pmp_thermal_exit(void)
{
	pr_warn("apple-pmp-thermal: removing Linux thermal policy and its CPU caps\n");
	apple_pmp_thermal_cleanup();
}

module_init(apple_pmp_thermal_init);
module_exit(apple_pmp_thermal_exit);

MODULE_DESCRIPTION("Apple J713 PMP hotspot CPU thermal policy");
MODULE_LICENSE("Dual MIT/GPL");
