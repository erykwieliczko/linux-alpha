/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_DWC3_HANDOFF_H
#define _APPLE_DWC3_HANDOFF_H

#include <linux/bits.h>
#include <linux/sizes.h>
#include <linux/types.h>

#define APPLE_DWC3_HANDOFF_MAGIC		0x33435744314e314dULL
#define APPLE_DWC3_HANDOFF_VERSION	2
#define APPLE_DWC3_HANDOFF_READY		BIT_ULL(0)

#define APPLE_DWC3_HANDOFF_TX_ENDPOINT	9
#define APPLE_DWC3_HANDOFF_RX_ENDPOINT	8

#define APPLE_DWC3_HANDOFF_ALIGNMENT	SZ_16K
#define APPLE_DWC3_BUFFER_ALIGNMENT	SZ_16K
#define APPLE_DWC3_TRB_ALIGNMENT		16
#define APPLE_DWC3_REGS_SIZE		SZ_64K

struct apple_dwc3_handoff_raw {
	__le64 magic;
	__le32 version;
	__le32 size;
	__le64 flags;
	__le64 regs_phys;
	__le64 event_buffer_phys;
	__le32 event_buffer_size;
	__le32 event_buffer_offset;
	__le64 scratchpad_phys;
	__le64 scratchpad_size;
	__le64 xfer_buffer_phys;
	__le64 xfer_buffer_size;
	__le64 trb_buffer_phys;
	__le64 trb_buffer_size;
	__le64 tx_buffer_phys;
	__le64 tx_buffer_iova;
	__le64 tx_trb_phys;
	__le64 tx_trb_iova;
	__le32 tx_endpoint;
	__le32 tx_max_packet;
	__le32 tx_busy;
	__le32 stage;
	__le64 rx_buffer_phys;
	__le64 rx_buffer_iova;
	__le64 rx_trb_phys;
	__le64 rx_trb_iova;
	__le64 wdt_regs_phys;
	__le32 rx_endpoint;
	__le32 rx_busy;
} __packed;

struct apple_dwc3_handoff_desc {
	u64 magic;
	u32 version;
	u32 size;
	u64 flags;
	u64 regs_phys;
	u64 event_buffer_phys;
	u32 event_buffer_size;
	u32 event_buffer_offset;
	u64 scratchpad_phys;
	u64 scratchpad_size;
	u64 xfer_buffer_phys;
	u64 xfer_buffer_size;
	u64 trb_buffer_phys;
	u64 trb_buffer_size;
	u64 tx_buffer_phys;
	u64 tx_buffer_iova;
	u64 tx_trb_phys;
	u64 tx_trb_iova;
	u32 tx_endpoint;
	u32 tx_max_packet;
	u32 tx_busy;
	u32 stage;
	u64 rx_buffer_phys;
	u64 rx_buffer_iova;
	u64 rx_trb_phys;
	u64 rx_trb_iova;
	u64 wdt_regs_phys;
	u32 rx_endpoint;
	u32 rx_busy;
};

struct apple_dwc3_handoff_region {
	u64 start;
	u64 size;
};

struct apple_dwc3_handoff_limits {
	const struct apple_dwc3_handoff_region *ram;
	size_t ram_count;
	const struct apple_dwc3_handoff_region *reserved;
	size_t reserved_count;
	const struct apple_dwc3_handoff_region *mmio;
	size_t mmio_count;
	u64 expected_wdt_regs;
};

int apple_dwc3_handoff_validate(const struct apple_dwc3_handoff_raw *raw,
				u64 descriptor_phys,
				const struct apple_dwc3_handoff_limits *limits,
				struct apple_dwc3_handoff_desc *desc);
u32 apple_dwc3_next_event(u32 offset, u32 event_buffer_size);

#endif
