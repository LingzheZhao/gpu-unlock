// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "registers.h"

typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t resource_size_t;
#define __iomem
#define BIT(n) (1U << (n))
#define GU_RELINK_SETTLE_MS 200
#define pci_warn(pdev, ...) ((void)(pdev))

struct pci_dev { int dummy; };
struct gu_memory_info {
	u64 physical_bytes;
	u64 usable_bytes;
};

static unsigned int hweight32(u32 w)
{
	return (unsigned int)__builtin_popcount(w);
}

/* Simulated BAR0 register bank for E1 PHY writes. */
struct reg_slot {
	u32 addr;
	u32 val;
};

static struct reg_slot regs[8];
static unsigned int reg_count;
static u32 write_log[16];
static unsigned int write_count;
static unsigned int sleep_calls;
static unsigned int policy_logs;

static u32 readl(const void *ptr)
{
	u32 off = (u32)(uintptr_t)ptr;
	for (unsigned int i = 0; i < reg_count; i++)
		if (regs[i].addr == off)
			return regs[i].val;
	return ~0u;
}

static void writel(u32 value, void *ptr)
{
	u32 off = (u32)(uintptr_t)ptr;
	assert(write_count < 16);
	write_log[write_count++] = off;
	for (unsigned int i = 0; i < reg_count; i++) {
		if (regs[i].addr == off) {
			regs[i].val = value;
			return;
		}
	}
	assert(!"write to unmapped register");
}

static void msleep(unsigned int ms)
{
	assert(ms == GU_RELINK_SETTLE_MS);
	sleep_calls++;
}

static void log_policy(struct pci_dev *pdev, void __iomem *bar,
		       resource_size_t len, unsigned int pass)
{
	(void)pdev; (void)bar; (void)len;
	assert(pass == 1);
	policy_logs++;
}

#include "ga100-mem.c"
#include "ga100-e1.c"

static void init_e1_regs(void)
{
	/* Before-values measured on hardware (cmp.wiki PCIe Gen4 lab table). */
	struct reg_slot init[] = {
		{ GU_XVE_GEN4_PHY_CFG1_OFFSET,  0x0000003fu },
		{ GU_XVE_GEN4_PHY_CFG0_OFFSET,  0x12345602u },
		{ GU_XP_CYA_0_OFFSET,           0x060731b2u },
		{ GU_XP_PL_LINK_RATE_OFFSET,    0x00130036u },
		{ GU_XP_LINK_CONFIG_0_OFFSET,   0x80544c00u },
		{ GU_XVE_PRIV_MISC_1_OFFSET,    0x20b40d00u },
		{ GU_XVE_VSEC_HIERARCHY_OFFSET, 0x00000000u },
	};
	memcpy(regs, init, sizeof(init));
	reg_count = 7;
	write_count = 0;
	sleep_calls = 0;
	policy_logs = 0;
}

static u32 reg_val(u32 addr)
{
	for (unsigned int i = 0; i < reg_count; i++)
		if (regs[i].addr == addr)
			return regs[i].val;
	assert(!"missing reg");
	return 0;
}

int main(void)
{
	unsigned int active = 0, gib = 0;
	struct gu_memory_info info = { };
	struct pci_dev pdev = { 0 };
	u32 range;

	/* Stock 8-FBP CMP 170HX 8 GB (4 disabled FBPs) keeps stock 0x20b. */
	range = gu_memory_cmp170hx_range(0x20c2, 0x1585, 12, 0x852, &active, &gib);
	assert(range == 0 && active == 8 && gib == 0);

	/* VFI-unlocked 10-FBP CMP 170HX 8 GB (Card #1: FBP6+11 = 0x840) -> 80 GiB (0x28b). */
	range = gu_memory_cmp170hx_range(0x20c2, 0x1585, 12, 0x840, &active, &gib);
	assert(range == 0x28bu && active == 10 && gib == 80);
	assert(!gu_memory_decode_range(range, &info));
	assert(info.physical_bytes == (80ULL << 30) && info.usable_bytes == (80ULL << 30));

	/* Different defect mask (Card H: FBP2+5 = 0x024) -> still 80 GiB (0x28b). */
	range = gu_memory_cmp170hx_range(0x20c2, 0x1585, 12, 0x024, &active, &gib);
	assert(range == 0x28bu && active == 10 && gib == 80);

	/* Unicorn 12-FBP CMP 170HX 8 GB (0 disabled) -> 96 GiB (0x30b). */
	range = gu_memory_cmp170hx_range(0x20c2, 0x1585, 12, 0x000, &active, &gib);
	assert(range == 0x30bu && active == 12 && gib == 96);
	assert(!gu_memory_decode_range(range, &info));
	assert(info.physical_bytes == (96ULL << 30));

	/* 32 GB ES card (subsystem 0x1586) is never rescaled to 80 GiB. */
	range = gu_memory_cmp170hx_range(0x20c2, 0x1586, 12, 0x840, &active, &gib);
	assert(range == 0 && active == 10 && gib == 0);

	/* 10 GB card (0x2082): stock 10 FBP stays stock; 12 FBP scales to 48 GiB (0x30a). */
	range = gu_memory_cmp170hx_range(0x2082, 0x1557, 12, 0x840, &active, &gib);
	assert(range == 0 && active == 10 && gib == 0);
	range = gu_memory_cmp170hx_range(0x2082, 0x1557, 12, 0x000, &active, &gib);
	assert(range == 0x30au && active == 12 && gib == 48);
	assert(!gu_memory_decode_range(range, &info));
	assert(info.physical_bytes == (48ULL << 30));

	/* PRI / inaccessible readbacks safely keep stock range. */
	assert(gu_memory_cmp170hx_range(0x20c2, 0x1585, 0xbadf1100u, 0x840, &active, &gib) == 0);
	assert(gu_memory_cmp170hx_range(0x20c2, 0x1585, 12, 0xffffffffu, &active, &gib) == 0);

	/* E1 PHY writes: verify exact before->after bitmasks and LINK_CONFIG_0 written last. */
	init_e1_regs();
	assert(!ga100_apply_gen4_e1(&pdev, NULL, 0x100000));
	assert(write_count == 7 && sleep_calls == 1 && policy_logs == 1);
	assert(write_log[6] == GU_XP_LINK_CONFIG_0_OFFSET);
	assert(reg_val(GU_XVE_GEN4_PHY_CFG1_OFFSET) == 0x00000014u);
	assert(reg_val(GU_XVE_GEN4_PHY_CFG0_OFFSET) == 0x12345606u);
	assert(reg_val(GU_XP_CYA_0_OFFSET) == 0x060711b2u);
	assert(reg_val(GU_XP_PL_LINK_RATE_OFFSET) == 0x00140036u);
	assert(reg_val(GU_XP_LINK_CONFIG_0_OFFSET) == 0x80504c00u);
	assert(reg_val(GU_XVE_PRIV_MISC_1_OFFSET) == 0x20b42d00u);
	assert(reg_val(GU_XVE_VSEC_HIERARCHY_OFFSET) == 0x00001001u);

	/* E1 abort guard: any PRI-error / all-ones register aborts with zero writes. */
	for (unsigned int i = 0; i < 7; i++) {
		init_e1_regs();
		regs[i].val = (i & 1) ? 0xbadf1020u : 0xffffffffu;
		assert(ga100_apply_gen4_e1(&pdev, NULL, 0x100000) == -EIO);
		assert(write_count == 0 && sleep_calls == 0);
	}
	/* Short BAR length aborts cleanly with -ERANGE. */
	init_e1_regs();
	assert(ga100_apply_gen4_e1(&pdev, NULL, GU_XP_CYA_0_OFFSET) == -ERANGE);
	assert(write_count == 0);

	puts("GA100 VFI extensions: dynamic 64/80/96/48 GiB FBP sizing and atomic Gen4 E1 PHY writes passed");
	return 0;
}
