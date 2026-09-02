// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>

#include "apple_dwc3_handoff.h"

#define TEST_DESCRIPTOR_PHYS	0x100000ULL
#define TEST_WDT_PHYS		0x3882b0000ULL
#define TEST_DWC3_PHYS		0x700100000ULL

static const struct apple_dwc3_handoff_region test_ram[] = {
	{ TEST_DESCRIPTOR_PHYS, 0x40000 },
};

static const struct apple_dwc3_handoff_region test_reserved[] = {
	{ TEST_DESCRIPTOR_PHYS, 0x40000 },
};

static const struct apple_dwc3_handoff_region test_mmio[] = {
	{ 0x700000000ULL, SZ_1G },
};

static const struct apple_dwc3_handoff_limits test_limits = {
	.ram = test_ram,
	.ram_count = ARRAY_SIZE(test_ram),
	.reserved = test_reserved,
	.reserved_count = ARRAY_SIZE(test_reserved),
	.mmio = test_mmio,
	.mmio_count = ARRAY_SIZE(test_mmio),
	.expected_wdt_regs = TEST_WDT_PHYS,
};

static void setup_valid_descriptor(struct apple_dwc3_handoff_raw *raw)
{
	memset(raw, 0, sizeof(*raw));
	raw->magic = cpu_to_le64(APPLE_DWC3_HANDOFF_MAGIC);
	raw->version = cpu_to_le32(APPLE_DWC3_HANDOFF_VERSION);
	raw->size = cpu_to_le32(sizeof(*raw));
	raw->flags = cpu_to_le64(APPLE_DWC3_HANDOFF_READY);
	raw->regs_phys = cpu_to_le64(TEST_DWC3_PHYS);
	raw->event_buffer_phys = cpu_to_le64(0x104000);
	raw->event_buffer_size = cpu_to_le32(SZ_16K);
	raw->event_buffer_offset = cpu_to_le32(7);
	raw->scratchpad_phys = cpu_to_le64(0x108000);
	raw->scratchpad_size = cpu_to_le64(SZ_16K);
	raw->xfer_buffer_phys = cpu_to_le64(0x10c000);
	raw->xfer_buffer_size = cpu_to_le64(2 * SZ_16K);
	raw->trb_buffer_phys = cpu_to_le64(0x114000);
	raw->trb_buffer_size = cpu_to_le64(SZ_16K);
	raw->tx_buffer_phys = cpu_to_le64(0x10c000);
	raw->tx_buffer_iova = cpu_to_le64(0x200000);
	raw->tx_trb_phys = cpu_to_le64(0x114000);
	raw->tx_trb_iova = cpu_to_le64(0x208000);
	raw->tx_endpoint = cpu_to_le32(APPLE_DWC3_HANDOFF_TX_ENDPOINT);
	raw->tx_max_packet = cpu_to_le32(SZ_16K);
	raw->tx_busy = cpu_to_le32(1);
	raw->stage = cpu_to_le32(5);
	raw->rx_buffer_phys = cpu_to_le64(0x110000);
	raw->rx_buffer_iova = cpu_to_le64(0x204000);
	raw->rx_trb_phys = cpu_to_le64(0x114010);
	raw->rx_trb_iova = cpu_to_le64(0x208010);
	raw->wdt_regs_phys = cpu_to_le64(TEST_WDT_PHYS);
	raw->rx_endpoint = cpu_to_le32(APPLE_DWC3_HANDOFF_RX_ENDPOINT);
	raw->rx_busy = cpu_to_le32(1);
}

static int test_validate(const struct apple_dwc3_handoff_raw *raw,
			 u64 descriptor_phys,
			 const struct apple_dwc3_handoff_limits *limits)
{
	struct apple_dwc3_handoff_desc desc;

	return apple_dwc3_handoff_validate(raw, descriptor_phys, limits, &desc);
}

static void handoff_valid_test(struct kunit *test)
{
	struct apple_dwc3_handoff_desc desc;
	struct apple_dwc3_handoff_raw raw;
	int ret;

	setup_valid_descriptor(&raw);
	ret = apple_dwc3_handoff_validate(&raw, TEST_DESCRIPTOR_PHYS,
					  &test_limits, &desc);
	KUNIT_ASSERT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, TEST_DWC3_PHYS, desc.regs_phys);
	KUNIT_EXPECT_EQ(test, 7U, desc.event_buffer_offset);
	KUNIT_EXPECT_EQ(test, 5U, desc.stage);

	raw.flags = cpu_to_le64(APPLE_DWC3_HANDOFF_READY | BIT_ULL(63));
	raw.size = cpu_to_le32(256);
	ret = apple_dwc3_handoff_validate(&raw, TEST_DESCRIPTOR_PHYS,
					  &test_limits, &desc);
	KUNIT_EXPECT_EQ(test, 0, ret);
}

static void handoff_header_test(struct kunit *test)
{
	struct apple_dwc3_handoff_raw raw;
	int ret;

	setup_valid_descriptor(&raw);
	raw.magic = 0;
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	setup_valid_descriptor(&raw);
	raw.version = cpu_to_le32(APPLE_DWC3_HANDOFF_VERSION + 1);
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	setup_valid_descriptor(&raw);
	raw.size = cpu_to_le32(sizeof(raw) - 1);
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	setup_valid_descriptor(&raw);
	raw.flags = 0;
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	setup_valid_descriptor(&raw);
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS + 8, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);
}

static void handoff_ranges_test(struct kunit *test)
{
	const struct apple_dwc3_handoff_region descriptor_only[] = {
		{ TEST_DESCRIPTOR_PHYS, SZ_16K },
	};
	struct apple_dwc3_handoff_limits limits = test_limits;
	struct apple_dwc3_handoff_raw raw;
	int ret;

	setup_valid_descriptor(&raw);
	raw.event_buffer_offset = cpu_to_le32(SZ_16K / sizeof(u32));
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	setup_valid_descriptor(&raw);
	raw.event_buffer_phys = cpu_to_le64(0x140000);
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	setup_valid_descriptor(&raw);
	limits.reserved = descriptor_only;
	limits.reserved_count = ARRAY_SIZE(descriptor_only);
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	setup_valid_descriptor(&raw);
	raw.rx_buffer_phys = raw.tx_buffer_phys;
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	setup_valid_descriptor(&raw);
	raw.rx_trb_iova = cpu_to_le64(U64_MAX - 7);
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	setup_valid_descriptor(&raw);
	raw.scratchpad_phys = raw.event_buffer_phys;
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);
}

static void handoff_platform_test(struct kunit *test)
{
	struct apple_dwc3_handoff_raw raw;
	int ret;

	setup_valid_descriptor(&raw);
	raw.regs_phys = cpu_to_le64(0x380000000ULL);
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	setup_valid_descriptor(&raw);
	raw.wdt_regs_phys = cpu_to_le64(TEST_WDT_PHYS + SZ_4K);
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	setup_valid_descriptor(&raw);
	raw.tx_endpoint = cpu_to_le32(7);
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	setup_valid_descriptor(&raw);
	raw.tx_max_packet = cpu_to_le32(508);
	ret = test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);
}

static void handoff_event_wrap_test(struct kunit *test)
{
	u32 next;

	next = apple_dwc3_next_event(0, SZ_16K);
	KUNIT_EXPECT_EQ(test, 1U, next);
	next = apple_dwc3_next_event(SZ_16K / sizeof(u32) - 1, SZ_16K);
	KUNIT_EXPECT_EQ(test, 0U, next);
}

static struct kunit_case handoff_test_cases[] = {
	KUNIT_CASE(handoff_valid_test),
	KUNIT_CASE(handoff_header_test),
	KUNIT_CASE(handoff_ranges_test),
	KUNIT_CASE(handoff_platform_test),
	KUNIT_CASE(handoff_event_wrap_test),
	{}
};

static struct kunit_suite handoff_test_suite = {
	.name = "apple-dwc3-handoff",
	.test_cases = handoff_test_cases,
};

kunit_test_suite(handoff_test_suite);

MODULE_LICENSE("GPL");
