// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/sizes.h>

#include "apple_dwc3_handoff.h"

#define APPLE_DWC3_EVENT_COUNT_MASK	0xfffc
#define APPLE_DWC3_MAX_PACKET_SIZE	SZ_1M
#define APPLE_DWC3_TRB_SIZE		16

static bool range_end(u64 start, u64 size, u64 *end)
{
	return size && !check_add_overflow(start, size, end);
}

static bool range_contains(u64 outer_start, u64 outer_size, u64 inner_start,
			   u64 inner_size)
{
	u64 outer_end, inner_end;

	if (!range_end(outer_start, outer_size, &outer_end) ||
	    !range_end(inner_start, inner_size, &inner_end))
		return false;

	return inner_start >= outer_start && inner_end <= outer_end;
}

static bool range_in_regions(const struct apple_dwc3_handoff_region *regions,
			     size_t count, u64 start, u64 size)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (range_contains(regions[i].start, regions[i].size, start, size))
			return true;
	}

	return false;
}

static bool ranges_overlap(u64 first_start, u64 first_size, u64 second_start,
			   u64 second_size)
{
	u64 first_end, second_end;

	if (!range_end(first_start, first_size, &first_end) ||
	    !range_end(second_start, second_size, &second_end))
		return true;

	return first_start < second_end && second_start < first_end;
}

static void decode_descriptor(const struct apple_dwc3_handoff_raw *raw,
			      struct apple_dwc3_handoff_desc *desc)
{
	desc->magic = le64_to_cpu(raw->magic);
	desc->version = le32_to_cpu(raw->version);
	desc->size = le32_to_cpu(raw->size);
	desc->flags = le64_to_cpu(raw->flags);
	desc->regs_phys = le64_to_cpu(raw->regs_phys);
	desc->event_buffer_phys = le64_to_cpu(raw->event_buffer_phys);
	desc->event_buffer_size = le32_to_cpu(raw->event_buffer_size);
	desc->event_buffer_offset = le32_to_cpu(raw->event_buffer_offset);
	desc->scratchpad_phys = le64_to_cpu(raw->scratchpad_phys);
	desc->scratchpad_size = le64_to_cpu(raw->scratchpad_size);
	desc->xfer_buffer_phys = le64_to_cpu(raw->xfer_buffer_phys);
	desc->xfer_buffer_size = le64_to_cpu(raw->xfer_buffer_size);
	desc->trb_buffer_phys = le64_to_cpu(raw->trb_buffer_phys);
	desc->trb_buffer_size = le64_to_cpu(raw->trb_buffer_size);
	desc->tx_buffer_phys = le64_to_cpu(raw->tx_buffer_phys);
	desc->tx_buffer_iova = le64_to_cpu(raw->tx_buffer_iova);
	desc->tx_trb_phys = le64_to_cpu(raw->tx_trb_phys);
	desc->tx_trb_iova = le64_to_cpu(raw->tx_trb_iova);
	desc->tx_endpoint = le32_to_cpu(raw->tx_endpoint);
	desc->tx_max_packet = le32_to_cpu(raw->tx_max_packet);
	desc->tx_busy = le32_to_cpu(raw->tx_busy);
	desc->stage = le32_to_cpu(raw->stage);
	desc->rx_buffer_phys = le64_to_cpu(raw->rx_buffer_phys);
	desc->rx_buffer_iova = le64_to_cpu(raw->rx_buffer_iova);
	desc->rx_trb_phys = le64_to_cpu(raw->rx_trb_phys);
	desc->rx_trb_iova = le64_to_cpu(raw->rx_trb_iova);
	desc->wdt_regs_phys = le64_to_cpu(raw->wdt_regs_phys);
	desc->rx_endpoint = le32_to_cpu(raw->rx_endpoint);
	desc->rx_busy = le32_to_cpu(raw->rx_busy);
}

static bool range_is_shared(const struct apple_dwc3_handoff_limits *limits,
			    u64 start, u64 size)
{
	return range_in_regions(limits->ram, limits->ram_count, start, size) &&
	       range_in_regions(limits->reserved, limits->reserved_count,
				start, size);
}

u32 apple_dwc3_next_event(u32 offset, u32 event_buffer_size)
{
	return (offset + 1) % (event_buffer_size / sizeof(u32));
}

int apple_dwc3_handoff_validate(const struct apple_dwc3_handoff_raw *raw,
				u64 descriptor_phys,
				const struct apple_dwc3_handoff_limits *limits,
				struct apple_dwc3_handoff_desc *desc)
{
	u64 ignored_end;

	if (!raw || !limits || !desc || !limits->ram || !limits->reserved ||
	    !limits->mmio || !limits->expected_wdt_regs)
		return -EINVAL;

	decode_descriptor(raw, desc);

	if (desc->magic != APPLE_DWC3_HANDOFF_MAGIC ||
	    desc->version != APPLE_DWC3_HANDOFF_VERSION ||
	    desc->size < sizeof(*raw) || desc->size > SZ_16K ||
	    !(desc->flags & APPLE_DWC3_HANDOFF_READY))
		return -EINVAL;

	if (!IS_ALIGNED(descriptor_phys, APPLE_DWC3_HANDOFF_ALIGNMENT) ||
	    !range_is_shared(limits, descriptor_phys, desc->size))
		return -EINVAL;

	if (!IS_ALIGNED(desc->regs_phys, SZ_4K) ||
	    !range_in_regions(limits->mmio, limits->mmio_count,
			      desc->regs_phys, APPLE_DWC3_REGS_SIZE) ||
	    desc->wdt_regs_phys != limits->expected_wdt_regs)
		return -EINVAL;

	if (!desc->event_buffer_size || !IS_ALIGNED(desc->event_buffer_size, 4) ||
	    desc->event_buffer_size > APPLE_DWC3_EVENT_COUNT_MASK ||
	    desc->event_buffer_offset >= desc->event_buffer_size / sizeof(u32) ||
	    !IS_ALIGNED(desc->event_buffer_phys, APPLE_DWC3_BUFFER_ALIGNMENT) ||
	    !range_is_shared(limits, desc->event_buffer_phys,
			     desc->event_buffer_size))
		return -EINVAL;

	if (!desc->scratchpad_size || !desc->xfer_buffer_size ||
	    !desc->trb_buffer_size ||
	    !IS_ALIGNED(desc->scratchpad_phys, APPLE_DWC3_BUFFER_ALIGNMENT) ||
	    !IS_ALIGNED(desc->xfer_buffer_phys, APPLE_DWC3_BUFFER_ALIGNMENT) ||
	    !IS_ALIGNED(desc->trb_buffer_phys, APPLE_DWC3_BUFFER_ALIGNMENT) ||
	    !range_is_shared(limits, desc->scratchpad_phys,
			     desc->scratchpad_size) ||
	    !range_is_shared(limits, desc->xfer_buffer_phys,
			     desc->xfer_buffer_size) ||
	    !range_is_shared(limits, desc->trb_buffer_phys,
			     desc->trb_buffer_size))
		return -EINVAL;

	if (ranges_overlap(descriptor_phys, desc->size,
			   desc->event_buffer_phys, desc->event_buffer_size) ||
	    ranges_overlap(descriptor_phys, desc->size,
			   desc->scratchpad_phys, desc->scratchpad_size) ||
	    ranges_overlap(descriptor_phys, desc->size,
			   desc->xfer_buffer_phys, desc->xfer_buffer_size) ||
	    ranges_overlap(descriptor_phys, desc->size,
			   desc->trb_buffer_phys, desc->trb_buffer_size) ||
	    ranges_overlap(desc->event_buffer_phys, desc->event_buffer_size,
			   desc->scratchpad_phys, desc->scratchpad_size) ||
	    ranges_overlap(desc->event_buffer_phys, desc->event_buffer_size,
			   desc->xfer_buffer_phys, desc->xfer_buffer_size) ||
	    ranges_overlap(desc->event_buffer_phys, desc->event_buffer_size,
			   desc->trb_buffer_phys, desc->trb_buffer_size) ||
	    ranges_overlap(desc->scratchpad_phys, desc->scratchpad_size,
			   desc->xfer_buffer_phys, desc->xfer_buffer_size) ||
	    ranges_overlap(desc->scratchpad_phys, desc->scratchpad_size,
			   desc->trb_buffer_phys, desc->trb_buffer_size) ||
	    ranges_overlap(desc->xfer_buffer_phys, desc->xfer_buffer_size,
			   desc->trb_buffer_phys, desc->trb_buffer_size))
		return -EINVAL;

	if (desc->tx_endpoint != APPLE_DWC3_HANDOFF_TX_ENDPOINT ||
	    desc->rx_endpoint != APPLE_DWC3_HANDOFF_RX_ENDPOINT ||
	    desc->tx_max_packet < 512 ||
	    desc->tx_max_packet > APPLE_DWC3_MAX_PACKET_SIZE ||
	    !IS_ALIGNED(desc->tx_max_packet, 4))
		return -EINVAL;

	if (!IS_ALIGNED(desc->tx_buffer_phys, APPLE_DWC3_BUFFER_ALIGNMENT) ||
	    !IS_ALIGNED(desc->rx_buffer_phys, APPLE_DWC3_BUFFER_ALIGNMENT) ||
	    !IS_ALIGNED(desc->tx_buffer_iova, APPLE_DWC3_BUFFER_ALIGNMENT) ||
	    !IS_ALIGNED(desc->rx_buffer_iova, APPLE_DWC3_BUFFER_ALIGNMENT) ||
	    !IS_ALIGNED(desc->tx_trb_phys, APPLE_DWC3_TRB_ALIGNMENT) ||
	    !IS_ALIGNED(desc->rx_trb_phys, APPLE_DWC3_TRB_ALIGNMENT) ||
	    !IS_ALIGNED(desc->tx_trb_iova, APPLE_DWC3_TRB_ALIGNMENT) ||
	    !IS_ALIGNED(desc->rx_trb_iova, APPLE_DWC3_TRB_ALIGNMENT))
		return -EINVAL;

	if (!range_contains(desc->xfer_buffer_phys, desc->xfer_buffer_size,
			    desc->tx_buffer_phys, desc->tx_max_packet) ||
	    !range_contains(desc->xfer_buffer_phys, desc->xfer_buffer_size,
			    desc->rx_buffer_phys, desc->tx_max_packet) ||
	    !range_contains(desc->trb_buffer_phys, desc->trb_buffer_size,
			    desc->tx_trb_phys, APPLE_DWC3_TRB_SIZE) ||
	    !range_contains(desc->trb_buffer_phys, desc->trb_buffer_size,
			    desc->rx_trb_phys, APPLE_DWC3_TRB_SIZE))
		return -EINVAL;

	if (ranges_overlap(desc->tx_buffer_phys, desc->tx_max_packet,
			   desc->rx_buffer_phys, desc->tx_max_packet) ||
	    ranges_overlap(desc->tx_buffer_iova, desc->tx_max_packet,
			   desc->rx_buffer_iova, desc->tx_max_packet) ||
	    ranges_overlap(desc->tx_trb_phys, APPLE_DWC3_TRB_SIZE,
			   desc->rx_trb_phys, APPLE_DWC3_TRB_SIZE) ||
	    ranges_overlap(desc->tx_trb_iova, APPLE_DWC3_TRB_SIZE,
			   desc->rx_trb_iova, APPLE_DWC3_TRB_SIZE))
		return -EINVAL;

	if (!range_end(desc->tx_buffer_iova, desc->tx_max_packet, &ignored_end) ||
	    !range_end(desc->rx_buffer_iova, desc->tx_max_packet, &ignored_end) ||
	    !range_end(desc->tx_trb_iova, APPLE_DWC3_TRB_SIZE, &ignored_end) ||
	    !range_end(desc->rx_trb_iova, APPLE_DWC3_TRB_SIZE, &ignored_end))
		return -EINVAL;

	return 0;
}
