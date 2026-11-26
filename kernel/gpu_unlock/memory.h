/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_MEMORY_H
#define GPU_UNLOCK_MEMORY_H

#include <linux/types.h>

struct device;
struct gu_family;

enum gu_memory_kind {
	GU_MEMORY_RANGE, /* usable size decoded from the local range */
	GU_MEMORY_GDDR,  /* also verify/update GFW's published size */
};

struct gu_memory_info {
	u64 physical_bytes;
	u64 usable_bytes;
};

int gu_memory_decode_range(u32 range, struct gu_memory_info *info);

/* Compute an adjusted CMP 170HX LOCAL_MEMORY_RANGE from live FBP count/disable
 * registers when active FBPs exceed stock counts (e.g. 10-FBP 80 GiB or 12-FBP
 * 96 GiB on a VFI-unlocked 0x20c2:0x1585 card). Returns 0 to keep stock. */
u32 gu_memory_cmp170hx_range(u16 device, u16 subsystem_device,
			     u32 fbp_count, u32 fbp_disabled,
			     unsigned int *active_fbps, unsigned int *gib);

int gu_memory_accept_gddr_published(u64 range_usable, u64 published_bytes,
				    u64 *out_usable);

/* Firmware must be quiesced. Does not alter the temporary Booter layout.
 * Verify final capacity and reconcile firmware bookkeeping before PLM locks. */
int gu_memory_finalize(struct device *dev, void __iomem *bar, resource_size_t len,
		       const struct gu_family *family, bool ecc_enabled,
		       struct gu_memory_info *info);

#endif
