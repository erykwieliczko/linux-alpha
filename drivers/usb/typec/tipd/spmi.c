// SPDX-License-Identifier: GPL-2.0

#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/spmi.h>

#include "tps6598x.h"

#define tps_to_sn(tps) container_of_const((tps), struct sn201202x, cd.tps)

struct sn201202x_match_data {
	const struct tipd_data *tipd;
	bool select_irq_optional;
};

static int sn201202x_poll_select(struct spmi_device *sdev, u8 reg)
{
	int err, ret;
	u8 val;

	ret = read_poll_timeout(spmi_register_read, err,
				err || val != (reg | BIT(7)), 1000, 100000,
				false, sdev, 0, &val);
	if (ret)
		return ret;
	if (err)
		return err;
	if (val != reg)
		return -EIO;

	return 0;
}

static int regmap_sn201202x_select_reg(struct spmi_device *sdev, u8 reg)
{
	int err;
	u8 val;
	bool warned = false;
	int attempts = 5;
	struct tps6598x *tps = spmi_device_get_drvdata(sdev);
	struct sn201202x *sn = tps_to_sn(tps);

	if (sn->has_select_irq)
		reinit_completion(&sn->select_completion);
	err = spmi_register_zero_write(sdev, reg);
	if (err)
		return err;

	if (!sn->has_select_irq)
		return sn201202x_poll_select(sdev, reg);

	if (!wait_for_completion_timeout(&sn->select_completion, msecs_to_jiffies(100)))
		return -ETIMEDOUT;

	while (--attempts) {
		err = spmi_register_read(sdev, 0, &val);
		if (err)
			return err;
		if (val == (reg | 0x80)) {
			if (!warned) {
				dev_warn(tps->dev,
					 "Got interrupt but selection not complete?\n");
				warned = true;
			}
			msleep(20);
			continue;
		}
		if (val == reg)
			return 0;
		return -EIO;
	}

	return -EIO;
}

static int regmap_sn201202x_read(void *context,
				 const void *reg, size_t reg_size,
				 void *val, size_t val_size)
{
	int err;
	unsigned int offset = 0x20;
	size_t len;
	u8 addr;

	if (reg_size != 1) {
		WARN_ON(1);
		return -EINVAL;
	}
	if (val_size > 0x40) {
		WARN_ON(1);
		return -EINVAL;
	}

	addr = *(u8 *)reg;

	err = regmap_sn201202x_select_reg(context, addr);
	if (err)
		return err;

	while (val_size) {
		len = min_t(size_t, val_size, 16);
		err = spmi_ext_register_read(context, offset, val, len);
		if (err)
			return err;
		offset += len;
		val += len;
		val_size -= len;
	}

	return 0;
}

static int regmap_sn201202x_write(void *context, const void *data,
				  size_t count)
{
	int err = 0;
	unsigned int offset = 0xa0;
	size_t len;
	u8 addr;

	if (count < 1) {
		WARN_ON(1);
		return -EINVAL;
	}

	addr = *(u8 *)data;
	data += 1;
	count -= 1;

	if (count > 0x40) {
		WARN_ON(1);
		return -EINVAL;
	}

	err = regmap_sn201202x_select_reg(context, addr);
	if (err)
		return err;

	while (count) {
		len = min_t(size_t, count, 16);
		err = spmi_ext_register_write(context, offset, data, len);
		if (err)
			return err;
		offset += len;
		data += len;
		count -= len;
	}

	return err;
}

static irqreturn_t sn201202x_irq(int irq, void *data)
{
	struct completion *c = data;

	complete(c);
	return IRQ_HANDLED;
}

static const struct regmap_bus regmap_sn201202x = {
	.read				= regmap_sn201202x_read,
	.write				= regmap_sn201202x_write,
	.reg_format_endian_default	= REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default	= REGMAP_ENDIAN_NATIVE,
};

static struct regmap *__devm_regmap_init_sn201202x(struct spmi_device *sdev,
						   const struct regmap_config *config,
						   struct lock_class_key *lock_key,
						   const char *lock_name)
{
	return __devm_regmap_init(&sdev->dev, &regmap_sn201202x, sdev, config,
				  lock_key, lock_name);
}

#define devm_regmap_init_sn201202x(dev, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_sn201202x, #config,	\
				dev, config)

static const struct sn201202x_match_data sn201202x_data = {
	.tipd = &tipd_sn201202x_data,
};

static const struct sn201202x_match_data sn201202x_t8132_data = {
	.tipd = &tipd_sn201202x_data,
	.select_irq_optional = true,
};

static const struct of_device_id sn201202x_of_match[] = {
	{ .compatible = "apple,t8132-sn201202x", .data = &sn201202x_t8132_data },
	{ .compatible = "apple,sn201202x", .data = &sn201202x_data },
	{}
};

static int sn201202x_probe(struct spmi_device *device)
{
	const struct of_device_id *match;
	const struct sn201202x_match_data *match_data;
	const struct tipd_data *data;
	struct sn201202x *sn;
	struct tps6598x *tps;
	int irq_select, irq_sleep, irq_wake;
	int ret;

	match = of_match_device(sn201202x_of_match, &device->dev);
	if (!match)
		return -EINVAL;
	match_data = match->data;
	data = match_data->tipd;

	sn = devm_kzalloc(&device->dev, data->tps_struct_size, GFP_KERNEL);
	if (!sn)
		return -ENOMEM;
	sn->sdev = device;
	tps = &sn->cd.tps;

	mutex_init(&tps->lock);
	tps->dev = &device->dev;
	tps->data = data;

	tps->irq = of_irq_get_byname(device->dev.of_node, "irq");
	if (tps->irq < 0)
		return tps->irq;
	irq_select = of_irq_get_byname(device->dev.of_node, "select");
	if (irq_select == -EPROBE_DEFER)
		return irq_select;
	if (irq_select < 0 &&
	    (!match_data->select_irq_optional ||
	     (irq_select != -EINVAL && irq_select != -ENODATA)))
		return irq_select;
	irq_sleep = of_irq_get_byname(device->dev.of_node, "sleep");
	if (irq_sleep < 0)
		return irq_sleep;
	irq_wake = of_irq_get_byname(device->dev.of_node, "wake");
	if (irq_wake < 0)
		return irq_wake;

	init_completion(&sn->select_completion);
	init_completion(&sn->sleep_completion);
	init_completion(&sn->wake_completion);

	if (irq_select >= 0) {
		ret = devm_request_irq(&device->dev, irq_select, sn201202x_irq,
				       0, NULL, &sn->select_completion);
		if (ret)
			return ret;
		sn->has_select_irq = true;
	}
	ret = devm_request_irq(&device->dev, irq_sleep, sn201202x_irq,
			       0, NULL, &sn->sleep_completion);
	if (ret)
		return ret;
	ret = devm_request_irq(&device->dev, irq_wake, sn201202x_irq,
			       0, NULL, &sn->wake_completion);
	if (ret)
		return ret;

	spmi_device_set_drvdata(device, tps);
	tps->regmap = devm_regmap_init_sn201202x(device, &tps6598x_regmap_config);
	if (IS_ERR(tps->regmap))
		return PTR_ERR(tps->regmap);

	ret = spmi_command_wakeup(device);
	if (ret)
		return ret;
	if (!wait_for_completion_timeout(&sn->wake_completion, msecs_to_jiffies(100)))
		return -ETIMEDOUT;

	ret = tipd_init(tps);
	if (ret)
		spmi_command_sleep(device);
	return ret;
}

static void sn201202x_remove(struct spmi_device *device)
{
	struct tps6598x *tps = spmi_device_get_drvdata(device);
	struct sn201202x *sn = tps_to_sn(tps);

	tipd_remove(tps);
	spmi_command_sleep(sn->sdev);
}

static int __maybe_unused sn201202x_resume(struct device *dev)
{
	struct tps6598x *tps = dev_get_drvdata(dev);
	struct sn201202x *sn = tps_to_sn(tps);
	int err;

	reinit_completion(&sn->wake_completion);
	err = spmi_command_wakeup(sn->sdev);
	if (err)
		return err;
	if (!wait_for_completion_timeout(&sn->wake_completion, msecs_to_jiffies(100)))
		return -ETIMEDOUT;
	return tipd_resume(tps);
}

static int __maybe_unused sn201202x_suspend(struct device *dev)
{
	struct tps6598x *tps = dev_get_drvdata(dev);
	struct sn201202x *sn = tps_to_sn(tps);
	int err;

	err = tipd_suspend(tps);
	if (err)
		return err;
	reinit_completion(&sn->sleep_completion);
	err = spmi_command_sleep(sn->sdev);
	if (err)
		goto out_resume;
	if (!wait_for_completion_timeout(&sn->sleep_completion, msecs_to_jiffies(100))) {
		err = -ETIMEDOUT;
		goto out_resume;
	}
	return 0;

out_resume:
	tipd_resume(tps);
	return err;
}

MODULE_DEVICE_TABLE(of, sn201202x_of_match);

static const struct dev_pm_ops sn201202x_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(sn201202x_suspend, sn201202x_resume)
};

static struct spmi_driver sn201202x_driver = {
	.driver = {
		.name = "sn201202x",
		.pm = &sn201202x_pm_ops,
		.of_match_table = sn201202x_of_match,
	},
	.probe = sn201202x_probe,
	.remove = sn201202x_remove,
};
module_spmi_driver(sn201202x_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TI SN201202x USB Power Delivery Controller Driver");
