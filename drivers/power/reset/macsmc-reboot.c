// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SMC Reboot/Poweroff Handler
 * Copyright The Asahi Linux Contributors
 */

#include <linux/delay.h>
#include <linux/mfd/core.h>
#include <linux/mfd/macsmc.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/slab.h>

struct macsmc_reboot_nvmem {
	struct nvmem_cell *shutdown_flag;
	struct nvmem_cell *boot_stage;
	struct nvmem_cell *boot_error_count;
	struct nvmem_cell *panic_count;
};

static const char * const nvmem_names[] = {
	"shutdown_flag",
	"boot_stage",
	"boot_error_count",
	"panic_count",
};

enum boot_stage {
	BOOT_STAGE_SHUTDOWN		= 0x00, /* Clean shutdown */
	BOOT_STAGE_IBOOT_DONE		= 0x2f, /* Last stage of bootloader */
	BOOT_STAGE_KERNEL_STARTED	= 0x30, /* Normal OS booting */
};

enum macsmc_transition {
	MACSMC_TRANSITION_NONE,
	MACSMC_TRANSITION_RESTART,
	MACSMC_TRANSITION_POWER_OFF,
};

struct macsmc_reboot {
	struct device *dev;
	struct apple_smc *smc;
	struct notifier_block reboot_notify;
	bool strict_lifecycle;
	enum macsmc_transition prepared_transition;

	union {
		struct macsmc_reboot_nvmem nvm;
		struct nvmem_cell *nvm_cells[ARRAY_SIZE(nvmem_names)];
	};
};

/* Helpers to read/write a u8 given a struct nvmem_cell */
static int nvmem_cell_get_u8(struct nvmem_cell *cell)
{
	size_t len;
	void *bfr;
	u8 val;

	bfr = nvmem_cell_read(cell, &len);
	if (IS_ERR(bfr))
		return PTR_ERR(bfr);

	if (len < 1) {
		kfree(bfr);
		return -EINVAL;
	}

	val = *(u8 *)bfr;
	kfree(bfr);
	return val;
}

static int nvmem_cell_set_u8(struct nvmem_cell *cell, u8 val)
{
	return nvmem_cell_write(cell, &val, sizeof(val));
}

/*
 * SMC 'MBSE' key actions:
 *
 * 'offw' - shutdown warning
 * 'slpw' - sleep warning
 * 'rest' - restart warning
 * 'off1' - shutdown (needs PMU bit set to stay on)
 * 'susp' - suspend
 * 'phra' - restart ("PE Halt Restart Action"?)
 * 'panb' - panic beginning
 * 'pane' - panic end
 */

static int macsmc_prepare_atomic(struct macsmc_reboot *reboot,
				 enum macsmc_transition transition)
{
	int ret;

	if (reboot->strict_lifecycle &&
	    reboot->prepared_transition != transition) {
		dev_crit(reboot->dev,
			 "Refusing unprepared native lifecycle transition\n");
		return NOTIFY_STOP;
	}

	dev_info(reboot->dev, "Preparing SMC for atomic mode\n");

	ret = apple_smc_enter_atomic(reboot->smc);
	if (ret) {
		dev_crit(reboot->dev, "Failed to enter SMC atomic mode: %d\n", ret);
		return reboot->strict_lifecycle ? NOTIFY_STOP : NOTIFY_OK;
	}

	return NOTIFY_OK;
}

static int macsmc_power_off_prepare(struct sys_off_data *data)
{
	return macsmc_prepare_atomic(data->cb_data, MACSMC_TRANSITION_POWER_OFF);
}

static int macsmc_restart_prepare(struct sys_off_data *data)
{
	return macsmc_prepare_atomic(data->cb_data, MACSMC_TRANSITION_RESTART);
}

static int macsmc_power_off(struct sys_off_data *data)
{
	struct macsmc_reboot *reboot = data->cb_data;
	int ret;

	if (reboot->strict_lifecycle &&
	    reboot->prepared_transition != MACSMC_TRANSITION_POWER_OFF) {
		dev_crit(reboot->dev, "Refusing unprepared native poweroff\n");
		return NOTIFY_STOP;
	}

	dev_info(reboot->dev, "Issuing power off (off1)\n");

	ret = apple_smc_write_u32_atomic(reboot->smc, SMC_KEY(MBSE), SMC_KEY(off1));
	if (ret < 0) {
		if (reboot->strict_lifecycle)
			dev_crit(reboot->dev,
				 "Failed to issue MBSE = off1 (power_off): %d\n", ret);
		else
			dev_err(reboot->dev, "Failed to issue MBSE = off1 (power_off)\n");
	} else {
		mdelay(100);
		if (reboot->strict_lifecycle)
			dev_crit(reboot->dev, "Native poweroff command returned\n");
		else
			WARN_ONCE(1, "Unable to power off system\n");
	}

	return reboot->strict_lifecycle ? NOTIFY_STOP : NOTIFY_OK;
}

static int macsmc_restart(struct sys_off_data *data)
{
	struct macsmc_reboot *reboot = data->cb_data;
	int ret;

	if (reboot->strict_lifecycle &&
	    reboot->prepared_transition != MACSMC_TRANSITION_RESTART) {
		dev_crit(reboot->dev, "Refusing unprepared native restart\n");
		return NOTIFY_STOP;
	}

	dev_info(reboot->dev, "Issuing restart (phra)\n");

	ret = apple_smc_write_u32_atomic(reboot->smc, SMC_KEY(MBSE), SMC_KEY(phra));
	if (ret < 0) {
		if (reboot->strict_lifecycle)
			dev_crit(reboot->dev,
				 "Failed to issue MBSE = phra (restart): %d\n", ret);
		else
			dev_err(reboot->dev, "Failed to issue MBSE = phra (restart)\n");
	} else {
		mdelay(100);
		if (reboot->strict_lifecycle)
			dev_crit(reboot->dev, "Native restart command returned\n");
		else
			WARN_ONCE(1, "Unable to restart system\n");
	}

	return reboot->strict_lifecycle ? NOTIFY_STOP : NOTIFY_OK;
}

static int macsmc_reboot_notify(struct notifier_block *this, unsigned long action, void *data)
{
	struct macsmc_reboot *reboot = container_of(this, struct macsmc_reboot, reboot_notify);
	enum macsmc_transition transition;
	u8 shutdown_flag;
	int ret;
	u32 val;

	reboot->prepared_transition = MACSMC_TRANSITION_NONE;

	switch (action) {
	case SYS_RESTART:
		val = SMC_KEY(rest);
		shutdown_flag = 0;
		transition = MACSMC_TRANSITION_RESTART;
		break;
	case SYS_POWER_OFF:
		val = SMC_KEY(offw);
		shutdown_flag = 1;
		transition = MACSMC_TRANSITION_POWER_OFF;
		break;
	default:
		return NOTIFY_DONE;
	}

	dev_info(reboot->dev, "Preparing for reboot (%p4ch)\n", &val);

	/* On the Mac Mini, this will turn off the LED for power off */
	ret = apple_smc_write_u32(reboot->smc, SMC_KEY(MBSE), val);
	if (ret < 0) {
		dev_err(reboot->dev,
			"Failed to issue MBSE = %p4ch (reboot_prepare): %d\n",
			&val, ret);
		if (reboot->strict_lifecycle)
			return NOTIFY_BAD;
	}

	/* Set the boot_stage to 0, which means we're doing a clean shutdown/reboot. */
	if (reboot->nvm.boot_stage) {
		ret = nvmem_cell_set_u8(reboot->nvm.boot_stage, BOOT_STAGE_SHUTDOWN);
		if (ret < 0) {
			dev_err(reboot->dev, "Failed to write boot_stage: %d\n", ret);
			if (reboot->strict_lifecycle)
				return NOTIFY_BAD;
		}
	}

	/*
	 * Set the PMU flag to actually reboot into the off state.
	 * Without this, the device will just reboot. We make it optional in case it is no longer
	 * necessary on newer hardware.
	 */
	if (reboot->nvm.shutdown_flag) {
		ret = nvmem_cell_set_u8(reboot->nvm.shutdown_flag, shutdown_flag);
		if (ret < 0) {
			dev_err(reboot->dev, "Failed to write shutdown_flag: %d\n", ret);
			if (reboot->strict_lifecycle)
				return NOTIFY_BAD;
		}
	}

	if (reboot->strict_lifecycle)
		reboot->prepared_transition = transition;

	return NOTIFY_OK;
}

static int macsmc_power_init_error_counts(struct macsmc_reboot *reboot)
{
	int boot_error_count, panic_count;
	int ret;

	if (!reboot->nvm.boot_error_count || !reboot->nvm.panic_count)
		return reboot->strict_lifecycle ? -ENODEV : 0;

	boot_error_count = nvmem_cell_get_u8(reboot->nvm.boot_error_count);
	if (boot_error_count < 0) {
		dev_err(reboot->dev, "Failed to read boot_error_count (%d)\n", boot_error_count);
		return reboot->strict_lifecycle ? boot_error_count : 0;
	}

	panic_count = nvmem_cell_get_u8(reboot->nvm.panic_count);
	if (panic_count < 0) {
		dev_err(reboot->dev, "Failed to read panic_count (%d)\n", panic_count);
		return reboot->strict_lifecycle ? panic_count : 0;
	}

	if (!boot_error_count && !panic_count)
		return 0;

	dev_warn(reboot->dev, "PMU logged %d boot error(s) and %d panic(s)\n",
		 boot_error_count, panic_count);

	ret = nvmem_cell_set_u8(reboot->nvm.panic_count, 0);
	if (ret < 0) {
		dev_err(reboot->dev, "Failed to reset panic_count: %d\n", ret);
		if (reboot->strict_lifecycle)
			return ret;
	}
	ret = nvmem_cell_set_u8(reboot->nvm.boot_error_count, 0);
	if (ret < 0) {
		dev_err(reboot->dev, "Failed to reset boot_error_count: %d\n", ret);
		if (reboot->strict_lifecycle)
			return ret;
	}

	return 0;
}

static int macsmc_reboot_probe(struct platform_device *pdev)
{
	struct apple_smc *smc = dev_get_drvdata(pdev->dev.parent);
	struct macsmc_reboot *reboot;
	int ret, i;

	/*
	 * MFD will probe this device even without a node in the device tree,
	 * thus bail out early if the SMC on the current machines does not
	 * support reboot and has no node in the device tree.
	 */
	if (!pdev->dev.of_node)
		return -ENODEV;

	reboot = devm_kzalloc(&pdev->dev, sizeof(*reboot), GFP_KERNEL);
	if (!reboot)
		return -ENOMEM;

	reboot->dev = &pdev->dev;
	reboot->smc = smc;
	reboot->strict_lifecycle = !!device_get_match_data(&pdev->dev);

	if (reboot->strict_lifecycle &&
	    (!of_machine_is_compatible("apple,j713") ||
	     !of_device_is_compatible(pdev->dev.parent->of_node,
				      "apple,t8132-smc")))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "T8132 lifecycle requires an apple,j713 machine and SMC\n");

	platform_set_drvdata(pdev, reboot);

	for (i = 0; i < ARRAY_SIZE(nvmem_names); i++) {
		struct nvmem_cell *cell;

		cell = devm_nvmem_cell_get(&pdev->dev,
					   nvmem_names[i]);
		if (IS_ERR(cell)) {
			if (PTR_ERR(cell) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			if (reboot->strict_lifecycle)
				return dev_err_probe(&pdev->dev, PTR_ERR(cell),
						     "Missing required NVMEM cell %s\n",
						     nvmem_names[i]);
			dev_warn(&pdev->dev, "Missing NVMEM cell %s (%ld)\n",
				 nvmem_names[i], PTR_ERR(cell));
			/* Non fatal, we'll deal with it */
			cell = NULL;
		}
		reboot->nvm_cells[i] = cell;
	}

	/* Display and clear the error counts */
	ret = macsmc_power_init_error_counts(reboot);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to initialize lifecycle counters\n");

	/* Set the boot_stage to indicate we're running the OS kernel */
	if (reboot->nvm.boot_stage) {
		ret = nvmem_cell_set_u8(reboot->nvm.boot_stage,
					BOOT_STAGE_KERNEL_STARTED);
		if (ret < 0) {
			dev_err(reboot->dev, "Failed to write boot_stage: %d\n", ret);
			if (reboot->strict_lifecycle)
				return ret;
		}
	}

	reboot->reboot_notify.notifier_call = macsmc_reboot_notify;

	ret = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_POWER_OFF_PREPARE,
					    SYS_OFF_PRIO_HIGH, macsmc_power_off_prepare,
					    reboot);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register power-off prepare handler\n");
	ret = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_POWER_OFF, SYS_OFF_PRIO_HIGH,
					    macsmc_power_off, reboot);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register power-off handler\n");

	ret = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_RESTART_PREPARE,
					    SYS_OFF_PRIO_HIGH, macsmc_restart_prepare,
					    reboot);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register restart prepare handler\n");
	ret = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_RESTART, SYS_OFF_PRIO_HIGH,
					    macsmc_restart, reboot);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to register restart handler\n");

	ret = devm_register_reboot_notifier(&pdev->dev, &reboot->reboot_notify);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to register reboot notifier\n");

	if (reboot->strict_lifecycle)
		dev_info(&pdev->dev,
			 "Native SMC reboot and poweroff lifecycle is ready (J713/T8132 strict mode)\n");
	else
		dev_info(&pdev->dev, "Handling reboot and poweroff requests via SMC\n");

	return 0;
}

static const bool macsmc_strict_lifecycle = true;

static const struct of_device_id macsmc_reboot_of_table[] = {
	{ .compatible = "apple,t8132-smc-reboot", .data = &macsmc_strict_lifecycle },
	{ .compatible = "apple,smc-reboot", },
	{}
};
MODULE_DEVICE_TABLE(of, macsmc_reboot_of_table);

static struct platform_driver macsmc_reboot_driver = {
	.driver = {
		.name = "macsmc-reboot",
		.of_match_table = macsmc_reboot_of_table,
	},
	.probe = macsmc_reboot_probe,
};
module_platform_driver(macsmc_reboot_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple SMC reboot/poweroff driver");
MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
