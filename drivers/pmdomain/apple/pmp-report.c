// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC PMP power state reporting driver
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/hwmon.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/unaligned.h>

#define PMP_REPORT_READY 0x1

struct apple_pmp_report_offsets {
	u32 tgt_read;
	u32 tgt_write;
	u32 actual;
	u32 status;
};

struct apple_pmp_report {
	struct device *dev;
	const struct apple_pmp_report_offsets *offsets;
	void __iomem *base;
	void __iomem *temp_sram;
	spinlock_t lock;
};

#define PMP_TEMP_SRAM_BASE	0x380500000ULL
#define PMP_TEMP_SRAM_SIZE	0xc0000
#define PMP_TEMP_RECORD_SIZE	0x44
#define PMP_TEMP_SAMPLE_OFFSET	0x34
#define PMP_TEMP_VALID_OFFSET	0x40
#define PMP_TEMP_READ_ATTEMPTS	20
#define PMP_TEMP_CHANNELS	6

/* Firmware identifiers do not establish the physical placement of sensors. */
static const struct apple_pmp_temp_record {
	const char *name;
	const char *label;
	u32 offset;
} apple_pmp_temp_records[] = {
	{ "ta000m", "Ta000m", 0x68624 },
	{ "te000m", "Te000m", 0x686c8 },
	{ "te001m", "Te001m", 0x6876c },
	{ "tp000m", "Tp000m", 0x68810 },
	{ "tp001m", "Tp001m", 0x688b4 },
	/* No individual channel: include only valid samples in the hotspot. */
	{ "tp002m", "Tp002m", 0x68958 },
};

static int apple_pmp_temp_decode(const u8 *record, const char *name, long *value)
{
	s32 sample;

	if (memcmp(record, name, strlen(name) + 1))
		return -EIO;
	if (get_unaligned_le32(record + PMP_TEMP_VALID_OFFSET) != 1)
		return -ENODATA;

	sample = (s32)get_unaligned_le32(record + PMP_TEMP_SAMPLE_OFFSET);
	if (sample < -2560 || sample > 9600)
		return -ERANGE;

	*value = DIV_ROUND_CLOSEST((s64)sample * 1000, 64);
	return 0;
}

static int apple_pmp_temp_sample(struct apple_pmp_report *rep, unsigned int index,
				 long *value)
{
	const struct apple_pmp_temp_record *record = &apple_pmp_temp_records[index];
	u8 first[PMP_TEMP_RECORD_SIZE], second[PMP_TEMP_RECORD_SIZE];
	unsigned int attempt;

	for (attempt = 0; attempt < PMP_TEMP_READ_ATTEMPTS; attempt++) {
		memcpy_fromio(first, rep->temp_sram + record->offset, sizeof(first));
		memcpy_fromio(second, rep->temp_sram + record->offset, sizeof(second));
		if (!memcmp(first, second, sizeof(first)))
			return apple_pmp_temp_decode(first, record->name, value);
	}

	return -EAGAIN;
}

static int apple_pmp_temp_hotspot(struct apple_pmp_report *rep, long *value)
{
	bool found = false;
	long hottest = 0, sample;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(apple_pmp_temp_records); i++) {
		ret = apple_pmp_temp_sample(rep, i, &sample);
		if (ret == -ENODATA)
			continue;
		if (ret)
			return ret;
		if (!found || sample > hottest)
			hottest = sample;
		found = true;
	}

	if (!found)
		return -ENODATA;
	*value = hottest;
	return 0;
}

static umode_t apple_pmp_temp_is_visible(const void *data,
					 enum hwmon_sensor_types type, u32 attr,
					 int channel)
{
	if (channel < 0 || channel >= PMP_TEMP_CHANNELS || type != hwmon_temp)
		return 0;
	if (attr == hwmon_temp_input || attr == hwmon_temp_label)
		return 0444;
	return 0;
}

static int apple_pmp_temp_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *value)
{
	struct apple_pmp_report *rep = dev_get_drvdata(dev);

	if (channel < 0 || channel >= PMP_TEMP_CHANNELS)
		return -EINVAL;
	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;
	if (!channel)
		return apple_pmp_temp_hotspot(rep, value);
	return apple_pmp_temp_sample(rep, channel - 1, value);
}

static int apple_pmp_temp_read_string(struct device *dev, enum hwmon_sensor_types type,
				      u32 attr, int channel, const char **str)
{
	if (channel < 0 || channel >= PMP_TEMP_CHANNELS)
		return -EINVAL;
	if (type != hwmon_temp || attr != hwmon_temp_label)
		return -EOPNOTSUPP;

	*str = channel ? apple_pmp_temp_records[channel - 1].label :
			 "PMP aggregate die hotspot";
	return 0;
}

static const struct hwmon_ops apple_pmp_temp_ops = {
	.is_visible = apple_pmp_temp_is_visible,
	.read = apple_pmp_temp_read,
	.read_string = apple_pmp_temp_read_string,
};

static const struct hwmon_channel_info * const apple_pmp_temp_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL,
};

static const struct hwmon_chip_info apple_pmp_temp_chip = {
	.ops = &apple_pmp_temp_ops,
	.info = apple_pmp_temp_info,
};

static int apple_pmp_temp_register(struct apple_pmp_report *rep)
{
	struct device *hwmon;

	/* Read-only telemetry from running firmware; never start or stop PMP. */
	rep->temp_sram = devm_ioremap(rep->dev, PMP_TEMP_SRAM_BASE, PMP_TEMP_SRAM_SIZE);
	if (!rep->temp_sram)
		return dev_err_probe(rep->dev, -ENOMEM, "cannot map PMP temperature SRAM\n");

	hwmon = devm_hwmon_device_register_with_info(rep->dev, "apple_pmp", rep,
						     &apple_pmp_temp_chip, NULL);
	if (IS_ERR(hwmon))
		return dev_err_probe(rep->dev, PTR_ERR(hwmon), "cannot register PMP hwmon\n");
	return 0;
}

static int apple_pmp_report_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct apple_pmp_report *rep;
	int ret;

	rep = devm_kzalloc(dev, sizeof(*rep), GFP_KERNEL);
	if (!rep)
		return -ENOMEM;

	rep->dev = dev;
	rep->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rep->base))
		return PTR_ERR(rep->base);
	rep->offsets = of_device_get_match_data(dev);
	dev_set_drvdata(dev, rep);
	if (of_device_is_compatible(np, "apple,t8132-pmp-v2-report")) {
		ret = apple_pmp_temp_register(rep);
		if (ret)
			return ret;
	}
	ret = of_platform_populate(np, NULL, NULL, dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create child devices\n");

	return 0;
}

static const struct apple_pmp_report_offsets apple_pmp_offsets_t600x = {
	.tgt_read = 0xf80,
	.tgt_write = 0x107c0,
	.actual = 0x1000,
	.status = 0x10,
};

static const struct apple_pmp_report_offsets apple_pmp_offsets_t602x = {
	.tgt_read = 0x2000,
	.tgt_write = 0x11000,
	.actual = 0x2080,
	.status = 0x10,
};

static const struct apple_pmp_report_offsets apple_pmp_offsets_t8112 = {
	.tgt_read = 0xa00,
	.tgt_write = 0x10500,
	.actual = 0xa40,
	.status = 0x10,
};

static const struct apple_pmp_report_offsets apple_pmp_offsets_t8132 = {
	.tgt_read = 0x1880,
	.tgt_write = 0x10c40,
	.actual = 0x1900,
	.status = 0x10,
};

static const struct of_device_id apple_pmp_report_of_match[] = {
	{ .compatible = "apple,t6000-pmp-v2-report", .data = &apple_pmp_offsets_t600x },
	{ .compatible = "apple,t6020-pmp-v2-report", .data = &apple_pmp_offsets_t602x },
	{ .compatible = "apple,t8112-pmp-v2-report", .data = &apple_pmp_offsets_t8112 },
	{ .compatible = "apple,t8132-pmp-v2-report", .data = &apple_pmp_offsets_t8132 },
	{}
};

static struct platform_driver apple_pmp_report_driver = {
	.probe = apple_pmp_report_probe,
	.driver = {
		.name = "apple-pmp-report",
		.of_match_table = apple_pmp_report_of_match,
	},
};

struct apple_pmp_report_entry {
	struct device *dev;
	struct generic_pm_domain genpd;
	u32 id;
};

#define genpd_to_apple_pmp_report_entry(_genpd) \
	container_of(_genpd, struct apple_pmp_report_entry, genpd)

static int apple_pmp_report_set_state(struct generic_pm_domain *genpd, bool enable)
{
	struct apple_pmp_report_entry *ent = genpd_to_apple_pmp_report_entry(genpd);
	struct apple_pmp_report *rep = dev_get_drvdata(ent->dev->parent);
	u64 bit_val = 1 << ent->id;
	u64 val;
	unsigned long flags;

	spin_lock_irqsave(&rep->lock, flags);
	val = readq(rep->base + rep->offsets->tgt_read);
	val &= ~bit_val;
	if (enable)
		val |= bit_val;
	writeq(val, rep->base + rep->offsets->tgt_write);
	spin_unlock_irqrestore(&rep->lock, flags);
	val = readq(rep->base + rep->offsets->status);
	if ((val & PMP_REPORT_READY) == 0)
		return 0;
	return readq_poll_timeout_atomic(
		rep->base + rep->offsets->actual,
		val,
		!!(val & bit_val) == !!enable,
		100,
		50000);
}

static int apple_pmp_report_entry_power_on(struct generic_pm_domain *genpd)
{
	return apple_pmp_report_set_state(genpd, true);
}

static int apple_pmp_report_entry_power_off(struct generic_pm_domain *genpd)
{
	return apple_pmp_report_set_state(genpd, false);
}

static int apple_pmp_report_entry_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct apple_pmp_report_entry *ent;
	int ret;
	const char *name;
	struct of_phandle_iterator it;

	ent = devm_kzalloc(dev, sizeof(*ent), GFP_KERNEL);
	if (!ent)
		return -ENOMEM;

	ent->dev = dev;

	ret = of_property_read_u32(node, "reg", &ent->id);
	if (ret)
		return dev_err_probe(dev, ret, "missing reg property\n");

	ret = of_property_read_string(node, "label", &name);
	if (ret < 0)
		return dev_err_probe(dev, ret, "missing label property\n");

	if (of_property_read_bool(node, "apple,always-on")) {
		ent->genpd.flags |= GENPD_FLAG_ACTIVE_WAKEUP;
		apple_pmp_report_set_state(&ent->genpd, true);
	}

	ent->genpd.name = name;
	ent->genpd.power_on = apple_pmp_report_entry_power_on;
	ent->genpd.power_off = apple_pmp_report_entry_power_off;

	ret = pm_genpd_init(&ent->genpd, NULL, true);
	if (ret)
		return dev_err_probe(dev, ret, "pm_genpd_init failed\n");

	ret = of_genpd_add_provider_simple(node, &ent->genpd);
	if (ret)
		return dev_err_probe(dev, ret, "of_genpd_add_provider_simple failed\n");

	of_for_each_phandle(&it, ret, node, "power-domains", "#power-domain-cells", -1) {
		struct of_phandle_args parent, child;

		parent.np = it.node;
		parent.args_count = of_phandle_iterator_args(&it, parent.args, MAX_PHANDLE_ARGS);
		child.np = node;
		child.args_count = 0;
		ret = of_genpd_add_subdomain(&parent, &child);

		if (ret == -EPROBE_DEFER) {
			of_node_put(parent.np);
			goto err_remove;
		} else if (ret < 0) {
			dev_err(dev, "failed to add to parent domain: %d (%s -> %s)\n",
				ret, it.node->name, node->name);
			of_node_put(parent.np);
			goto err_remove;
		}
	}

	pm_genpd_remove_device(dev);

	return 0;
err_remove:
	of_genpd_del_provider(node);
	pm_genpd_remove(&ent->genpd);
	return ret;
}

static const struct of_device_id apple_pmp_report_entry_of_match[] = {
	{ .compatible = "apple,t6000-pmp-v2-report-entry" },
	{}
};

static struct platform_driver apple_pmp_report_entry_driver = {
	.probe = apple_pmp_report_entry_probe,
	.driver = {
		.name = "apple-pmp-report-entry",
		.of_match_table = apple_pmp_report_entry_of_match,
	},
};

MODULE_DEVICE_TABLE(of, apple_pmp_report_of_match);
MODULE_DEVICE_TABLE(of, apple_pmp_report_entry_of_match);

static int __init apple_pmp_report_init(void)
{
	platform_driver_register(&apple_pmp_report_entry_driver);
	platform_driver_register(&apple_pmp_report_driver);
	return 0;
}

static void __exit apple_pmp_report_exit(void)
{
	platform_driver_unregister(&apple_pmp_report_entry_driver);
	platform_driver_unregister(&apple_pmp_report_driver);
}

module_init(apple_pmp_report_init);
module_exit(apple_pmp_report_exit);

MODULE_DESCRIPTION("PMP power state reporting driver for Apple SoCs");
MODULE_LICENSE("Dual MIT/GPL");
