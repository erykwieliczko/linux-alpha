// SPDX-License-Identifier: GPL-2.0-only
/* J700 retained-power USB2 hub/repeater supplier. No Type-C or VBUS policy. */
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/phy/phy.h>

struct j700_repeater {
	struct i2c_client *client;
	struct gpio_desc *hub_hold;
};

static int j700_repeater_update(struct i2c_client *client, u8 reg, u8 mask, u8 value)
{
	int old = i2c_smbus_read_byte_data(client, reg);

	if (old < 0)
		return old;
	return i2c_smbus_write_byte_data(client, reg, (old & ~mask) | value);
}

static int j700_repeater_init(struct phy *phy)
{
	struct j700_repeater *repeater = phy_get_drvdata(phy);
	struct i2c_client *client = repeater->client;
	static const struct { u8 reg, mask, value; } tuning[] = {
		{ 0x00, 0x0f, 0x00 }, { 0x01, 0x0f, 0x00 },
		{ 0x02, 0x07, 0x00 }, { 0x03, 0xf0, 0x10 },
		{ 0x82, 0x20, 0x20 },
	};
	int ret, value;

	for (size_t i = 0; i < ARRAY_SIZE(tuning); i++) {
		ret = j700_repeater_update(client, tuning[i].reg,
					   tuning[i].mask, tuning[i].value);
		if (ret < 0)
			return dev_err_probe(&client->dev, ret, "repeater tuning failed\n");
	}

	/* Option-zero open is a separate transaction, after the set-bit tuning. */
	value = i2c_smbus_read_byte_data(client, 0x82);
	if (value < 0)
		return value;
	value &= ~BIT(5);
	ret = i2c_smbus_write_byte_data(client, 0x82, value);
	if (ret < 0)
		return ret;
	ret = i2c_smbus_read_byte_data(client, 0x82);
	if (ret < 0)
		return ret;
	if (ret != value)
		return dev_err_probe(&client->dev, -EIO, "repeater open readback mismatch\n");

	dev_info(&client->dev, "J700 USB2 repeater ready; shared hub held low\n");
	return 0;
}

static const struct phy_ops j700_repeater_ops = {
	.init = j700_repeater_init,
	.owner = THIS_MODULE,
};

static int j700_repeater_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct j700_repeater *repeater;
	struct phy_provider *provider;
	struct phy *phy;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;
	repeater = devm_kzalloc(dev, sizeof(*repeater), GFP_KERNEL);
	if (!repeater)
		return -ENOMEM;
	repeater->client = client;
	/* Retained J700 board state: physical low, not a speculative reset pulse. */
	repeater->hub_hold = devm_gpiod_get(dev, "hub-hold", GPIOD_OUT_LOW);
	if (IS_ERR(repeater->hub_hold))
		return dev_err_probe(dev, PTR_ERR(repeater->hub_hold), "hub hold GPIO unavailable\n");
	phy = devm_phy_create(dev, NULL, &j700_repeater_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, repeater);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id j700_repeater_match[] = {
	{ .compatible = "apple,j700-usb2-repeater" },
	{}
};
MODULE_DEVICE_TABLE(of, j700_repeater_match);

static struct i2c_driver j700_repeater_driver = {
	.probe = j700_repeater_probe,
	.driver = {
		.name = "j700-usb2-repeater",
		.of_match_table = j700_repeater_match,
		.suppress_bind_attrs = true,
	},
};
module_i2c_driver(j700_repeater_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J700 retained-power USB2 repeater and fixed hub supplier");
