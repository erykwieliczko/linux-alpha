// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "m1n1-dwc3: " fmt

#include <linux/console.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/overflow.h>
#include <linux/serial_core.h>
#include <linux/spinlock.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/workqueue.h>

#include <asm/cacheflush.h>
#include <asm/early_ioremap.h>

#include "apple_dwc3_handoff.h"

#define HANDOFF_PROPERTY	"linux-enablement-mac,m1n1-dwc3-handoff"
#define HANDOFF_MAX_REGIONS	32
#define HANDOFF_TX_CHUNK	511
#define HANDOFF_TX_TIMEOUT_US	250000
#define HANDOFF_POLL_MS		1

#define DWC3_GEVNTCOUNT_MASK	0xfffc
#define DWC3_GSNPSID_MASK	0xffff0000
#define DWC3_GSNPSID		0xc120
#define DWC3_GEVNTSIZ(n)	(0xc408 + (n) * 0x10)
#define DWC3_GEVNTCOUNT(n)	(0xc40c + (n) * 0x10)
#define DWC3_DCTL		0xc704
#define DWC3_DALEPENA		0xc720
#define DWC3_DEP_BASE(n)	(0xc800 + (n) * 0x10)
#define DWC3_DEPCMDPAR2(n)	(DWC3_DEP_BASE(n) + 0x00)
#define DWC3_DEPCMDPAR1(n)	(DWC3_DEP_BASE(n) + 0x04)
#define DWC3_DEPCMDPAR0(n)	(DWC3_DEP_BASE(n) + 0x08)
#define DWC3_DEPCMD(n)		(DWC3_DEP_BASE(n) + 0x0c)

#define DWC3_DCTL_RUN_STOP	BIT(31)
#define DWC3_DALEPENA_EP(n)	BIT(n)
#define DWC3_DEPCMD_STATUS(n)	(((n) >> 12) & 0xf)
#define DWC3_DEPCMD_CMDACT	BIT(10)
#define DWC3_DEPCMD_STARTTRANSFER 0x06

#define DWC3_TRB_SIZE_MASK	GENMASK(23, 0)
#define DWC3_TRB_CTRL_HWO	BIT(0)
#define DWC3_TRB_CTRL_LST	BIT(1)
#define DWC3_TRB_CTRL_NORMAL	BIT(4)
#define DWC3_TRB_CTRL_ISP_IMI	BIT(10)

#define DWC3_EVENT_DEVSPEC	BIT(0)
#define DWC3_EVENT_TYPE(raw)	(((raw) >> 1) & 0x7f)
#define DWC3_EVENT_ENDPOINT(raw) (((raw) >> 1) & 0x1f)
#define DWC3_EVENT_EPTYPE(raw)	(((raw) >> 6) & 0xf)
#define DWC3_EVENT_STATUS(raw)	(((raw) >> 12) & 0xf)
#define DWC3_EVENT_DEVTYPE(raw)	(((raw) >> 8) & 0xf)
#define DWC3_EVENT_TYPE_DEVICE	0
#define DWC3_DEPEVT_XFERCOMPLETE 1
#define DWC3_DEPEVT_BUSERR	BIT(0)
#define DWC3_DEVICE_DISCONNECT	0
#define DWC3_DEVICE_RESET	1

struct handoff_trb {
	__le32 bpl;
	__le32 bph;
	__le32 size;
	__le32 ctrl;
} __packed;

struct handoff_platform_limits {
	struct apple_dwc3_handoff_limits limits;
	struct apple_dwc3_handoff_region ram[HANDOFF_MAX_REGIONS];
	struct apple_dwc3_handoff_region reserved[HANDOFF_MAX_REGIONS];
};

struct handoff_state {
	struct apple_dwc3_handoff_desc desc;
	void __iomem *descriptor;
	void __iomem *regs;
	__le32 *event_buffer;
	void *event_mapping;
	void *xfer_mapping;
	void *trb_mapping;
	struct handoff_trb *tx_trb;
	struct handoff_trb *rx_trb;
	u8 *tx_buffer;
	u8 *rx_buffer;
	u64 descriptor_phys;
	u32 event_cursor;
	u32 rx_offset;
	u32 rx_length;
	bool tx_busy;
	bool rx_busy;
	bool active;
	bool io_failed;
	bool early;
	bool owns_descriptor;
	bool owns_regs;
};

static DEFINE_SPINLOCK(handoff_lock);
static struct handoff_state handoff;
static struct console *handoff_early_console;
static struct tty_driver *handoff_tty_driver;
static struct tty_port handoff_tty_port;
static struct delayed_work handoff_poll_work;
static struct handoff_platform_limits handoff_platform __initdata;

static const struct apple_dwc3_handoff_region j713_mmio[] = {
	{ 0x700000000ULL, SZ_1G },
};

static_assert(sizeof(struct apple_dwc3_handoff_raw) == 192);
static_assert(offsetof(struct apple_dwc3_handoff_raw, flags) == 0x10);
static_assert(offsetof(struct apple_dwc3_handoff_raw, event_buffer_phys) == 0x20);
static_assert(offsetof(struct apple_dwc3_handoff_raw, tx_buffer_phys) == 0x60);
static_assert(offsetof(struct apple_dwc3_handoff_raw, rx_buffer_phys) == 0x90);
static_assert(offsetof(struct apple_dwc3_handoff_raw, rx_busy) == 0xbc);

static bool handoff_range_contains(u64 outer_start, u64 outer_size,
				   u64 inner_start, u64 inner_size)
{
	u64 outer_end, inner_end;

	if (!outer_size || !inner_size ||
	    check_add_overflow(outer_start, outer_size, &outer_end) ||
	    check_add_overflow(inner_start, inner_size, &inner_end))
		return false;

	return inner_start >= outer_start && inner_end <= outer_end;
}

static bool range_in_regions(const struct apple_dwc3_handoff_region *regions,
			     size_t count, u64 start, u64 size)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (handoff_range_contains(regions[i].start, regions[i].size,
					   start, size))
			return true;
	}

	return false;
}

static int __init find_wdt_node(unsigned long node, const char *uname,
				int depth, void *data)
{
	u64 *address = data;

	if (!of_flat_dt_is_compatible(node, "apple,t8132-wdt"))
		return 0;

	*address = of_flat_dt_translate_address(node);
	return *address == OF_BAD_ADDR ? 0 : 1;
}

static int __init
handoff_get_platform_limits(struct handoff_platform_limits *storage)
{
	struct memblock_region *region;
	const void *fdt = initial_boot_params;
	u64 wdt_address = 0;
	int count, i;

	if (!fdt ||
	    !of_flat_dt_is_compatible(of_get_flat_dt_root(), "apple,j713") ||
	    !of_flat_dt_is_compatible(of_get_flat_dt_root(), "apple,t8132"))
		return -ENODEV;

	memset(storage, 0, sizeof(*storage));
	for_each_mem_region(region) {
		if (storage->limits.ram_count == ARRAY_SIZE(storage->ram))
			return -E2BIG;
		storage->ram[storage->limits.ram_count].start = region->base;
		storage->ram[storage->limits.ram_count].size = region->size;
		storage->limits.ram_count++;
	}
	if (!storage->limits.ram_count)
		return -EINVAL;

	count = fdt_num_mem_rsv(fdt);
	if (count <= 0 || count > ARRAY_SIZE(storage->reserved))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (fdt_get_mem_rsv(fdt, i, &storage->reserved[i].start,
				    &storage->reserved[i].size))
			return -EINVAL;
	}

	of_scan_flat_dt(find_wdt_node, &wdt_address);
	if (!wdt_address)
		return -EINVAL;

	storage->limits.ram = storage->ram;
	storage->limits.reserved = storage->reserved;
	storage->limits.reserved_count = count;
	storage->limits.mmio = j713_mmio;
	storage->limits.mmio_count = ARRAY_SIZE(j713_mmio);
	storage->limits.expected_wdt_regs = wdt_address;
	return 0;
}

static int __init handoff_descriptor_phys(u64 *address)
{
	const void *fdt = initial_boot_params;
	const fdt64_t *property;
	int chosen, length;

	if (!fdt)
		return -ENODEV;

	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0)
		return chosen;

	property = fdt_getprop(fdt, chosen, HANDOFF_PROPERTY, &length);
	if (!property)
		return -ENOENT;
	if (length != sizeof(*property))
		return -EINVAL;

	*address = fdt64_to_cpu(*property);
	return 0;
}

static bool
handoff_descriptor_address_valid(u64 address,
				 const struct apple_dwc3_handoff_limits *limits)
{
	return IS_ALIGNED(address, APPLE_DWC3_HANDOFF_ALIGNMENT) &&
	       range_in_regions(limits->ram, limits->ram_count, address,
				sizeof(struct apple_dwc3_handoff_raw)) &&
	       range_in_regions(limits->reserved, limits->reserved_count, address,
				sizeof(struct apple_dwc3_handoff_raw));
}

static void handoff_read_descriptor(void __iomem *address,
				    struct apple_dwc3_handoff_raw *raw)
{
	memcpy_fromio(raw, address, sizeof(*raw));
}

static bool handoff_same_layout(const struct apple_dwc3_handoff_desc *first,
				const struct apple_dwc3_handoff_desc *second)
{
	return first->magic == second->magic &&
	       first->version == second->version &&
	       first->size == second->size &&
	       first->regs_phys == second->regs_phys &&
	       first->event_buffer_phys == second->event_buffer_phys &&
	       first->event_buffer_size == second->event_buffer_size &&
	       first->scratchpad_phys == second->scratchpad_phys &&
	       first->scratchpad_size == second->scratchpad_size &&
	       first->xfer_buffer_phys == second->xfer_buffer_phys &&
	       first->xfer_buffer_size == second->xfer_buffer_size &&
	       first->trb_buffer_phys == second->trb_buffer_phys &&
	       first->trb_buffer_size == second->trb_buffer_size &&
	       first->tx_buffer_phys == second->tx_buffer_phys &&
	       first->tx_buffer_iova == second->tx_buffer_iova &&
	       first->tx_trb_phys == second->tx_trb_phys &&
	       first->tx_trb_iova == second->tx_trb_iova &&
	       first->tx_endpoint == second->tx_endpoint &&
	       first->tx_max_packet == second->tx_max_packet &&
	       first->rx_buffer_phys == second->rx_buffer_phys &&
	       first->rx_buffer_iova == second->rx_buffer_iova &&
	       first->rx_trb_phys == second->rx_trb_phys &&
	       first->rx_trb_iova == second->rx_trb_iova &&
	       first->wdt_regs_phys == second->wdt_regs_phys &&
	       first->rx_endpoint == second->rx_endpoint;
}

static void handoff_publish_state_locked(void)
{
	iowrite32(handoff.event_cursor,
		  handoff.descriptor +
		  offsetof(struct apple_dwc3_handoff_raw, event_buffer_offset));
	iowrite32(handoff.tx_busy,
		  handoff.descriptor +
		  offsetof(struct apple_dwc3_handoff_raw, tx_busy));
	iowrite32(handoff.rx_busy,
		  handoff.descriptor +
		  offsetof(struct apple_dwc3_handoff_raw, rx_busy));
	/* Publish a complete cursor/busy snapshot to any subsequent owner. */
	wmb();
}

static void handoff_invalidate(const void *address, size_t size)
{
	dcache_inval_poc((unsigned long)address,
			 (unsigned long)address + size);
	dma_rmb();
}

static void handoff_clean(const void *address, size_t size)
{
	dcache_clean_poc((unsigned long)address,
			 (unsigned long)address + size);
	dma_wmb();
}

static void handoff_clean_invalidate(const void *address, size_t size)
{
	dcache_clean_inval_poc((unsigned long)address,
			       (unsigned long)address + size);
	dma_wmb();
}

static int handoff_endpoint_command_locked(u8 endpoint, u32 command,
					   u64 trb_iova)
{
	void __iomem *command_reg = handoff.regs + DWC3_DEPCMD(endpoint);
	u32 value;
	int i;

	writel(upper_32_bits(trb_iova),
	       handoff.regs + DWC3_DEPCMDPAR0(endpoint));
	writel(lower_32_bits(trb_iova),
	       handoff.regs + DWC3_DEPCMDPAR1(endpoint));
	writel(0, handoff.regs + DWC3_DEPCMDPAR2(endpoint));
	writel(command | DWC3_DEPCMD_CMDACT, command_reg);

	for (i = 0; i < 1000; i++) {
		value = readl(command_reg);
		if (!(value & DWC3_DEPCMD_CMDACT))
			return DWC3_DEPCMD_STATUS(value) ? -EIO : 0;
		udelay(1);
	}

	return -ETIMEDOUT;
}

static int handoff_start_transfer_locked(u8 endpoint, struct handoff_trb *trb,
					 u64 trb_iova, u64 buffer_iova,
					 u32 length)
{
	trb->bpl = cpu_to_le32(lower_32_bits(buffer_iova));
	trb->bph = cpu_to_le32(upper_32_bits(buffer_iova));
	trb->size = cpu_to_le32(length & DWC3_TRB_SIZE_MASK);
	trb->ctrl = cpu_to_le32(DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_LST |
				 DWC3_TRB_CTRL_NORMAL | DWC3_TRB_CTRL_ISP_IMI);
	handoff_clean(trb, sizeof(*trb));

	return handoff_endpoint_command_locked(endpoint,
					       DWC3_DEPCMD_STARTTRANSFER,
					       trb_iova);
}

static void handoff_fail_locked(void)
{
	handoff.io_failed = true;
}

static void handoff_handle_event_locked(u32 event)
{
	u32 endpoint, remaining;

	if (event & DWC3_EVENT_DEVSPEC) {
		if (DWC3_EVENT_TYPE(event) == DWC3_EVENT_TYPE_DEVICE &&
		    (DWC3_EVENT_DEVTYPE(event) == DWC3_DEVICE_DISCONNECT ||
		     DWC3_EVENT_DEVTYPE(event) == DWC3_DEVICE_RESET))
			handoff_fail_locked();
		return;
	}

	if (DWC3_EVENT_EPTYPE(event) != DWC3_DEPEVT_XFERCOMPLETE)
		return;
	if (DWC3_EVENT_STATUS(event) & DWC3_DEPEVT_BUSERR) {
		handoff_fail_locked();
		return;
	}

	endpoint = DWC3_EVENT_ENDPOINT(event);
	if (endpoint == handoff.desc.tx_endpoint) {
		handoff_invalidate(handoff.tx_trb, sizeof(*handoff.tx_trb));
		handoff.tx_busy = false;
	} else if (endpoint == handoff.desc.rx_endpoint) {
		handoff_invalidate(handoff.rx_trb, sizeof(*handoff.rx_trb));
		remaining = le32_to_cpu(handoff.rx_trb->size) &
			    DWC3_TRB_SIZE_MASK;
		if (remaining > handoff.desc.tx_max_packet) {
			handoff_fail_locked();
			return;
		}

		handoff.rx_busy = false;
		handoff.rx_offset = 0;
		handoff.rx_length = handoff.desc.tx_max_packet - remaining;
		if (handoff.rx_length)
			handoff_invalidate(handoff.rx_buffer, handoff.rx_length);
	}
}

static void handoff_handle_events_locked(void)
{
	u32 event_size = handoff.desc.event_buffer_size;
	u32 pending;

	if (!handoff.active || handoff.io_failed)
		return;

	pending = readl(handoff.regs + DWC3_GEVNTCOUNT(0)) &
		  DWC3_GEVNTCOUNT_MASK;
	if (!IS_ALIGNED(pending, sizeof(u32)) || pending > event_size) {
		handoff_fail_locked();
		return;
	}

	while (pending) {
		__le32 *event_word = &handoff.event_buffer[handoff.event_cursor];
		u32 cursor = handoff.event_cursor;
		u32 event, endpoint, next_event;

		handoff_invalidate(event_word, sizeof(*event_word));
		event = le32_to_cpu(READ_ONCE(*event_word));
		endpoint = DWC3_EVENT_ENDPOINT(event);
		if (!(event & DWC3_EVENT_DEVSPEC) &&
		    endpoint != handoff.desc.tx_endpoint &&
		    endpoint != handoff.desc.rx_endpoint) {
			handoff_fail_locked();
			return;
		}

		next_event = apple_dwc3_next_event(cursor, event_size);
		handoff.event_cursor = next_event;
		writel(sizeof(u32), handoff.regs + DWC3_GEVNTCOUNT(0));
		pending -= sizeof(u32);

		handoff_handle_event_locked(event);
		handoff_publish_state_locked();
		if (handoff.io_failed)
			return;
	}
}

static int handoff_start_rx_locked(void)
{
	int ret;

	if (handoff.rx_busy || handoff.rx_length || handoff.io_failed)
		return 0;

	handoff_clean_invalidate(handoff.rx_buffer,
				 handoff.desc.tx_max_packet);
	ret = handoff_start_transfer_locked(handoff.desc.rx_endpoint,
					    handoff.rx_trb,
					    handoff.desc.rx_trb_iova,
					    handoff.desc.rx_buffer_iova,
					    handoff.desc.tx_max_packet);
	if (ret) {
		handoff_fail_locked();
		return ret;
	}

	handoff.rx_busy = true;
	handoff_publish_state_locked();
	return 0;
}

static int handoff_wait_for_tx_locked(void)
{
	unsigned int elapsed;

	for (elapsed = 0; elapsed < HANDOFF_TX_TIMEOUT_US &&
	     handoff.tx_busy && !handoff.io_failed; elapsed++) {
		handoff_handle_events_locked();
		if (!handoff.tx_busy || handoff.io_failed)
			break;
		udelay(1);
	}
	if (handoff.tx_busy && !handoff.io_failed) {
		handoff_fail_locked();
		return -ETIMEDOUT;
	}

	return handoff.io_failed ? -EIO : 0;
}

static size_t handoff_write_locked(const u8 *source, size_t count)
{
	size_t written = 0;

	while (written < count) {
		size_t length;

		if (handoff_wait_for_tx_locked())
			break;

		length = min(count - written, (size_t)HANDOFF_TX_CHUNK);
		memcpy(handoff.tx_buffer, source + written, length);
		handoff_clean(handoff.tx_buffer, length);
		if (handoff_start_transfer_locked(handoff.desc.tx_endpoint,
						  handoff.tx_trb,
						  handoff.desc.tx_trb_iova,
						  handoff.desc.tx_buffer_iova,
						  length)) {
			handoff_fail_locked();
			break;
		}

		handoff.tx_busy = true;
		handoff_publish_state_locked();
		written += length;
	}

	return written;
}

static void handoff_console_write(struct console *console, const char *string,
				  unsigned int count)
{
	unsigned long flags;
	const char *start = string;
	const char *end = string + count;
	const char *newline;

	if (!spin_trylock_irqsave(&handoff_lock, flags))
		return;
	if (!handoff.active || handoff.io_failed)
		goto out;

	while (start < end && (newline = memchr(start, '\n', end - start))) {
		handoff_write_locked((const u8 *)start, newline - start);
		handoff_write_locked((const u8 *)"\r\n", 2);
		start = newline + 1;
	}
	if (start < end)
		handoff_write_locked((const u8 *)start, end - start);
out:
	spin_unlock_irqrestore(&handoff_lock, flags);
}

static void __init handoff_unmap(struct handoff_state *state)
{
	if (state->early) {
		if (state->trb_mapping)
			early_memunmap(state->trb_mapping,
				       state->desc.trb_buffer_size);
		if (state->xfer_mapping)
			early_memunmap(state->xfer_mapping,
				       state->desc.xfer_buffer_size);
		if (state->event_mapping)
			early_memunmap(state->event_mapping,
				       state->desc.event_buffer_size);
		if (state->regs)
			early_iounmap(state->regs, APPLE_DWC3_REGS_SIZE);
	} else {
		if (state->trb_mapping)
			memunmap(state->trb_mapping);
		if (state->xfer_mapping)
			memunmap(state->xfer_mapping);
		if (state->event_mapping)
			memunmap(state->event_mapping);
		if (state->regs)
			iounmap(state->regs);
		if (state->owns_regs)
			release_mem_region(state->desc.regs_phys,
					   APPLE_DWC3_REGS_SIZE);
		if (state->descriptor && state->owns_descriptor)
			iounmap(state->descriptor);
	}
	memset(state, 0, sizeof(*state));
}

static int handoff_validate_hardware(const struct handoff_state *state)
{
	u32 endpoints;

	endpoints = DWC3_DALEPENA_EP(state->desc.tx_endpoint) |
		    DWC3_DALEPENA_EP(state->desc.rx_endpoint);
	if ((readl(state->regs + DWC3_GSNPSID) & DWC3_GSNPSID_MASK) !=
	    0x33310000 ||
	    !(readl(state->regs + DWC3_DCTL) & DWC3_DCTL_RUN_STOP) ||
	    (readl(state->regs + DWC3_DALEPENA) & endpoints) != endpoints ||
	    (readl(state->regs + DWC3_GEVNTSIZ(0)) & 0xffff) !=
	    state->desc.event_buffer_size)
		return -EINVAL;

	return 0;
}

static void *__init handoff_map_memory(bool early, u64 address, size_t size)
{
	if (early)
		return early_memremap(address, size);

	return memremap(address, size, MEMREMAP_WB);
}

static int __init handoff_map_runtime(struct handoff_state *state, bool early)
{
	u64 event_phys = state->desc.event_buffer_phys;
	u64 regs_phys = state->desc.regs_phys;
	u64 trb_phys = state->desc.trb_buffer_phys;
	u64 xfer_phys = state->desc.xfer_buffer_phys;
	size_t event_size = state->desc.event_buffer_size;
	size_t trb_size = state->desc.trb_buffer_size;
	size_t xfer_size = state->desc.xfer_buffer_size;

	state->early = early;
	if (early) {
		state->regs = early_ioremap(regs_phys,
					    APPLE_DWC3_REGS_SIZE);
	} else {
		if (!request_mem_region(regs_phys,
					APPLE_DWC3_REGS_SIZE,
					"m1n1-dwc3-handoff"))
			return -EBUSY;
		state->owns_regs = true;
		state->regs = ioremap(regs_phys, APPLE_DWC3_REGS_SIZE);
	}
	if (!state->regs)
		goto nomem;

	state->event_mapping = handoff_map_memory(early, event_phys, event_size);
	state->xfer_mapping = handoff_map_memory(early, xfer_phys, xfer_size);
	state->trb_mapping = handoff_map_memory(early, trb_phys, trb_size);
	if (!state->event_mapping || !state->xfer_mapping ||
	    !state->trb_mapping)
		goto nomem;

	state->event_buffer = state->event_mapping;
	state->tx_buffer = (u8 *)state->xfer_mapping +
		(state->desc.tx_buffer_phys - state->desc.xfer_buffer_phys);
	state->rx_buffer = (u8 *)state->xfer_mapping +
		(state->desc.rx_buffer_phys - state->desc.xfer_buffer_phys);
	state->tx_trb = (struct handoff_trb *)((u8 *)state->trb_mapping +
		(state->desc.tx_trb_phys - state->desc.trb_buffer_phys));
	state->rx_trb = (struct handoff_trb *)((u8 *)state->trb_mapping +
		(state->desc.rx_trb_phys - state->desc.trb_buffer_phys));

	if (handoff_validate_hardware(state)) {
		handoff_unmap(state);
		return -EINVAL;
	}

	return 0;

nomem:
	handoff_unmap(state);
	return -ENOMEM;
}

static int __init handoff_prepare_state(struct handoff_state *state,
					bool early, void __iomem *descriptor,
					u64 descriptor_phys)
{
	struct apple_dwc3_handoff_raw raw;
	int ret;

	ret = handoff_get_platform_limits(&handoff_platform);
	if (ret)
		return ret;
	if (!handoff_descriptor_address_valid(descriptor_phys,
					      &handoff_platform.limits))
		return -EINVAL;

	memset(state, 0, sizeof(*state));
	state->descriptor = descriptor;
	state->descriptor_phys = descriptor_phys;
	handoff_read_descriptor(descriptor, &raw);
	ret = apple_dwc3_handoff_validate(&raw, descriptor_phys,
					  &handoff_platform.limits,
					  &state->desc);
	if (ret)
		return ret;

	ret = handoff_map_runtime(state, early);
	if (ret)
		return ret;

	state->event_cursor = state->desc.event_buffer_offset;
	state->tx_busy = !!state->desc.tx_busy;
	state->rx_busy = !!state->desc.rx_busy;
	state->active = true;
	return 0;
}

static int __init handoff_earlycon_setup(struct earlycon_device *device,
					 const char *options)
{
	struct handoff_state state;
	u64 property_address;
	int ret;

	if (!device->port.mapbase || !device->port.membase)
		return -ENODEV;

	ret = handoff_descriptor_phys(&property_address);
	if (ret || property_address != device->port.mapbase)
		return -EINVAL;

	ret = handoff_prepare_state(&state, true, device->port.membase,
				    property_address);
	if (ret)
		return ret;

	spin_lock(&handoff_lock);
	handoff = state;
	handoff_handle_events_locked();
	if (!handoff.rx_busy && !handoff.rx_length)
		handoff_start_rx_locked();
	handoff_publish_state_locked();
	spin_unlock(&handoff_lock);
	if (handoff.io_failed) {
		handoff_unmap(&handoff);
		return -EIO;
	}

	handoff_early_console = device->con;
	device->con->write = handoff_console_write;
	return 0;
}

EARLYCON_DECLARE(m1n1_dwc3, handoff_earlycon_setup);

static int handoff_tty_open(struct tty_struct *tty, struct file *file)
{
	return tty_port_open(&handoff_tty_port, tty, file);
}

static void handoff_tty_close(struct tty_struct *tty, struct file *file)
{
	tty_port_close(&handoff_tty_port, tty, file);
}

static void handoff_tty_hangup(struct tty_struct *tty)
{
	tty_port_hangup(&handoff_tty_port);
}

static ssize_t handoff_tty_write(struct tty_struct *tty, const u8 *buffer,
				 size_t count)
{
	unsigned long flags;
	size_t written;

	spin_lock_irqsave(&handoff_lock, flags);
	written = handoff.active && !handoff.io_failed ?
		handoff_write_locked(buffer, count) : 0;
	spin_unlock_irqrestore(&handoff_lock, flags);
	return written;
}

static unsigned int handoff_tty_write_room(struct tty_struct *tty)
{
	unsigned long flags;
	unsigned int room;

	spin_lock_irqsave(&handoff_lock, flags);
	handoff_handle_events_locked();
	room = handoff.active && !handoff.io_failed && !handoff.tx_busy ?
		HANDOFF_TX_CHUNK : 0;
	spin_unlock_irqrestore(&handoff_lock, flags);
	return room;
}

static unsigned int handoff_tty_chars_in_buffer(struct tty_struct *tty)
{
	unsigned long flags;
	unsigned int count;

	spin_lock_irqsave(&handoff_lock, flags);
	handoff_handle_events_locked();
	count = handoff.tx_busy ? 1 : 0;
	spin_unlock_irqrestore(&handoff_lock, flags);
	return count;
}

static const struct tty_operations handoff_tty_ops = {
	.open = handoff_tty_open,
	.close = handoff_tty_close,
	.hangup = handoff_tty_hangup,
	.write = handoff_tty_write,
	.write_room = handoff_tty_write_room,
	.chars_in_buffer = handoff_tty_chars_in_buffer,
};

static struct tty_driver *handoff_console_device(struct console *console,
						 int *index)
{
	*index = 0;
	return handoff_tty_driver;
}

static int handoff_console_setup(struct console *console, char *options)
{
	return console->index == 1 && handoff.active && !handoff.io_failed ?
		0 : -ENODEV;
}

static struct console handoff_console = {
	.name = "ttyM",
	.write = handoff_console_write,
	.device = handoff_console_device,
	.setup = handoff_console_setup,
	.flags = CON_PRINTBUFFER,
	.index = 1,
};

static void handoff_poll(struct work_struct *work)
{
	unsigned long flags;
	size_t length, inserted;
	u8 *buffer;

	spin_lock_irqsave(&handoff_lock, flags);
	handoff_handle_events_locked();
	buffer = handoff.rx_buffer + handoff.rx_offset;
	length = handoff.rx_length;
	spin_unlock_irqrestore(&handoff_lock, flags);

	if (length) {
		inserted = tty_insert_flip_string(&handoff_tty_port, buffer, length);
		if (inserted)
			tty_flip_buffer_push(&handoff_tty_port);

		spin_lock_irqsave(&handoff_lock, flags);
		handoff.rx_offset += inserted;
		handoff.rx_length -= inserted;
		if (!handoff.rx_length) {
			handoff.rx_offset = 0;
			handoff_start_rx_locked();
		}
		spin_unlock_irqrestore(&handoff_lock, flags);
	}

	if (READ_ONCE(handoff.active) && !READ_ONCE(handoff.io_failed))
		schedule_delayed_work(&handoff_poll_work,
				      msecs_to_jiffies(HANDOFF_POLL_MS));
}

static int __init handoff_register_tty(void)
{
	static const struct tty_port_operations port_ops;
	struct tty_driver *driver;
	int ret;

	driver = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW |
				       TTY_DRIVER_RESET_TERMIOS);
	if (IS_ERR(driver))
		return PTR_ERR(driver);

	tty_port_init(&handoff_tty_port);
	handoff_tty_port.ops = &port_ops;
	driver->driver_name = "m1n1-dwc3-handoff";
	driver->name = "ttyM";
	driver->name_base = 1;
	driver->type = TTY_DRIVER_TYPE_CONSOLE;
	driver->subtype = SYSTEM_TYPE_CONSOLE;
	driver->init_termios = tty_std_termios;
	driver->init_termios.c_iflag = IGNBRK | IGNPAR;
	driver->init_termios.c_oflag = OPOST | ONLCR;
	driver->init_termios.c_cflag = B115200 | CS8 | CREAD | CLOCAL;
	tty_set_operations(driver, &handoff_tty_ops);
	tty_port_link_device(&handoff_tty_port, driver, 0);

	ret = tty_register_driver(driver);
	if (ret) {
		tty_driver_kref_put(driver);
		tty_port_destroy(&handoff_tty_port);
		return ret;
	}

	handoff_tty_driver = driver;
	if (handoff_early_console)
		handoff_console.flags &= ~CON_PRINTBUFFER;
	register_console(&handoff_console);
	return 0;
}

static int __init handoff_late_init(void)
{
	struct apple_dwc3_handoff_desc latest;
	struct apple_dwc3_handoff_raw raw;
	struct handoff_state next, previous;
	void __iomem *descriptor;
	unsigned long flags;
	u64 descriptor_phys;
	int ret;

	ret = handoff_descriptor_phys(&descriptor_phys);
	if (ret)
		return ret == -ENOENT ? 0 : ret;
	ret = handoff_get_platform_limits(&handoff_platform);
	if (ret)
		return ret;
	if (!handoff_descriptor_address_valid(descriptor_phys,
					      &handoff_platform.limits))
		return -EINVAL;

	descriptor = ioremap(descriptor_phys,
			     sizeof(struct apple_dwc3_handoff_raw));
	if (!descriptor)
		return -ENOMEM;

	ret = handoff_prepare_state(&next, false, descriptor, descriptor_phys);
	if (ret) {
		iounmap(descriptor);
		return ret;
	}
	next.owns_descriptor = true;

	spin_lock_irqsave(&handoff_lock, flags);
	handoff_read_descriptor(descriptor, &raw);
	ret = apple_dwc3_handoff_validate(&raw, descriptor_phys,
					  &handoff_platform.limits, &latest);
	if (ret || !handoff_same_layout(&next.desc, &latest)) {
		spin_unlock_irqrestore(&handoff_lock, flags);
		handoff_unmap(&next);
		return ret ?: -EINVAL;
	}
	next.desc = latest;
	next.event_cursor = latest.event_buffer_offset;
	next.tx_busy = !!latest.tx_busy;
	next.rx_busy = !!latest.rx_busy;
	previous = handoff;
	handoff = next;
	handoff_handle_events_locked();
	if (!handoff.rx_busy && !handoff.rx_length)
		handoff_start_rx_locked();
	handoff_publish_state_locked();
	spin_unlock_irqrestore(&handoff_lock, flags);

	if (handoff.io_failed) {
		handoff_unmap(&previous);
		handoff_unmap(&handoff);
		return -EIO;
	}

	ret = handoff_register_tty();
	if (ret) {
		spin_lock_irqsave(&handoff_lock, flags);
		handoff.active = false;
		spin_unlock_irqrestore(&handoff_lock, flags);
		handoff_unmap(&previous);
		handoff_unmap(&handoff);
		return ret;
	}

	if (handoff_early_console)
		unregister_console(handoff_early_console);
	handoff_unmap(&previous);
	INIT_DELAYED_WORK(&handoff_poll_work, handoff_poll);
	schedule_delayed_work(&handoff_poll_work,
			      msecs_to_jiffies(HANDOFF_POLL_MS));
	pr_info("adopted inherited console as ttyM1\n");
	return 0;
}
device_initcall(handoff_late_init);
