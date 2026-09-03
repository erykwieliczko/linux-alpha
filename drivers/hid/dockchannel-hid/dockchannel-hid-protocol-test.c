// SPDX-License-Identifier: GPL-2.0 OR MIT
#include <kunit/test.h>

#include "dockchannel-hid-protocol.h"

static void dchid_power_method_2_will_change_test(struct kunit *test)
{
	const u8 expected[DCHID_POWER_METHOD_2_CMD_SIZE] = {
		DCHID_CMD_RESET_INTERFACE, DCHID_POWER_METHOD_2, 0x5a,
		DCHID_POWER_STATE_ON, 0, 0, 0, 0, 0,
	};
	u8 command[DCHID_POWER_METHOD_2_CMD_SIZE];

	dchid_pm2_command(0x5a, false, command);

	KUNIT_EXPECT_MEMEQ(test, command, expected, sizeof(expected));
}

static void dchid_power_method_2_has_changed_test(struct kunit *test)
{
	const u8 expected[DCHID_POWER_METHOD_2_CMD_SIZE] = {
		DCHID_CMD_RESET_INTERFACE, DCHID_POWER_METHOD_2, 0xff,
		DCHID_POWER_STATE_ON, 1, 0, 0, 0, 0,
	};
	u8 command[DCHID_POWER_METHOD_2_CMD_SIZE];

	dchid_pm2_command(0xff, true, command);

	KUNIT_EXPECT_MEMEQ(test, command, expected, sizeof(expected));
}

static struct kunit_case dchid_protocol_test_cases[] = {
	KUNIT_CASE(dchid_power_method_2_will_change_test),
	KUNIT_CASE(dchid_power_method_2_has_changed_test),
	{}
};

static struct kunit_suite dchid_protocol_test_suite = {
	.name = "dockchannel-hid-protocol",
	.test_cases = dchid_protocol_test_cases,
};

kunit_test_suite(dchid_protocol_test_suite);

MODULE_DESCRIPTION("Apple DockChannel HID protocol KUnit tests");
MODULE_LICENSE("Dual MIT/GPL");
