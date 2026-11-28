// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define __iomem

static void udelay(unsigned long us) { (void)us; }
static void ndelay(unsigned long ns) { (void)ns; }

static int stop_machine(int (*fn)(void *), void *data, const void *cpus)
{
	(void)cpus;
	return fn(data);
}

#include "registers.h"

/* Simulated 24-FBPA GA100 BAR0 matching Card #3 (OPT_FBP_DEFECTIVE=0x2D:
 * FBP0, FBP2, FBP3, FBP5 fused off -> FBPA 0,1,4,5,6,7,10,11 return PRI error;
 * 16 active FBPAs: 2,3,8,9,12..23). */
#define SIM_ACTIVE_MASK 0x00FFF30Cu

struct sim_gpu {
	u32 gfw_boot;
	u32 gsp_status;
	u32 fb_stop;
	u32 pll_cfg[24];
	u32 pll_coeff[24];
	u32 pll_plm[24];
	u32 refresh[24];
	u32 refresh_plm[24];
	u32 sw_ref_writes[24];
	u32 ctr_count[16];
	bool fail_next_lock;
};

static struct sim_gpu sim;

static void sim_init(u32 stock_ndiv)
{
	memset(&sim, 0, sizeof(sim));
	sim.gfw_boot = 0x000003ffu;
	sim.gsp_status = 0x00000005u;
	for (unsigned int i = 0; i < 24; i++) {
		if (SIM_ACTIVE_MASK & (1u << i)) {
			sim.pll_plm[i] = 0xffffffffu;
			sim.pll_cfg[i] = 0x8000192du; /* bit0=1, bit5=PLL_LOCK */
			sim.pll_coeff[i] = 0x00010001u | (stock_ndiv << 8);
			sim.refresh_plm[i] = 0xffffffffu;
			sim.refresh[i] = 0x00000090u; /* refresh field = 18 << 3 */
		} else {
			sim.pll_plm[i] = 0xbadf2010u;
			sim.pll_cfg[i] = 0xbadf2010u;
			sim.pll_coeff[i] = 0xbadf2010u;
			sim.refresh_plm[i] = 0xbadf2010u;
			sim.refresh[i] = 0xbadf2010u;
		}
	}
}

static u32 readl(const volatile void __iomem *p)
{
	u32 off = (u32)(uintptr_t)p;

	if (off == GU_GFW_BOOT_OFFSET)
		return sim.gfw_boot;
	if (off == GU_GSP_RISCV_STATUS_OFFSET)
		return sim.gsp_status;
	if (off == 0x0010a7c0u || off == 0x0010a7c4u)
		return sim.fb_stop;
	if (off >= 0x00900000u && off < 0x00960000u) {
		unsigned int i = (off - 0x00900000u) / 0x4000u;
		u32 r = (off - 0x00900000u) % 0x4000u;
		if (!(SIM_ACTIVE_MASK & (1u << i)))
			return 0xbadf2010u;
		if (r == 0x0204u) return 0x02779000u; /* FBPA_CFG1 */
		if (r == 0x0168u) return sim.refresh_plm[i];
		if (r == 0x02a0u) return sim.refresh[i];
		if (r == 0x3c7cu) return sim.pll_plm[i];
		if (r == 0x3c90u) return sim.pll_cfg[i];
		if (r == 0x3c98u) return sim.pll_coeff[i];
		return 0;
	}
	/* Activity counter slot 0 covers all required banks. */
	if (off == 0x10be40u) return 0x00380081u;
	if (off == 0x10be80u) return 0x00700000u;
	if (off == 0x10bec0u) return 0x00000007u;
	if (off == 0x10bfc0u) return 2u;
	if (off == 0x10be00u) return 0u;
	if (off >= 0x10bf80u && off < 0x10bfc0u)
		return sim.ctr_count[(off - 0x10bf80u) / 4];
	return 0;
}

static void writel(u32 val, volatile void __iomem *p)
{
	u32 off = (u32)(uintptr_t)p;

	if (off == 0x0010a7e0u) {
		sim.fb_stop |= val;
		return;
	}
	if (off == 0x0010a7e4u) {
		sim.fb_stop &= ~val;
		return;
	}
	if (off == 0x0098bc98u) { /* PLL_COEFF_MC2 */
		for (unsigned int i = 0; i < 24; i++) {
			if (!(SIM_ACTIVE_MASK & (1u << i)))
				continue;
			sim.pll_coeff[i] = val;
			if (sim.fail_next_lock)
				sim.pll_cfg[i] &= ~0x20u;
			else
				sim.pll_cfg[i] |= 0x20u;
		}
		sim.fail_next_lock = false;
		return;
	}
	if (off == 0x0098bc90u) { /* PLL_CFG_MC2 */
		for (unsigned int i = 0; i < 24; i++)
			if (SIM_ACTIVE_MASK & (1u << i))
				sim.pll_cfg[i] = val;
		return;
	}
	if (off >= 0x00900000u && off < 0x00960000u) {
		unsigned int i = (off - 0x00900000u) / 0x4000u;
		u32 r = (off - 0x00900000u) % 0x4000u;
		if (!(SIM_ACTIVE_MASK & (1u << i)))
			return;
		if (r == 0x02a0u) sim.refresh[i] = val;
		if (r == 0x0310u) sim.sw_ref_writes[i]++;
	}
}

#include "hbm-unit.c"

int main(void)
{
	struct gu_hbm hbm;
	void __iomem *bar = (void __iomem *)0;
	u64 t = 1000000000ull;

	/* 1. Boot on FBPA0-fused card at stock NDIV 64 -> capture skips FBPA0/1, picks FBPA2. */
	sim_init(64);
	gu_hbm_init(&hbm);
	assert(gu_hbm_poll(&hbm, bar, t) == 0);
	assert(hbm.profile);
	assert(hbm.fbpas == SIM_ACTIVE_MASK);
	assert(hbm.first_fbpa == 0x00908000u); /* FBPA2 */
	assert(((hbm.stock_coeff >> 8) & 0xffu) == 64u);
	assert(((hbm.busy_coeff >> 8) & 0xffu) == 64u);
	assert(!hbm.idle);

	/* 2. Reject out-of-range NDIV. */
	assert(gu_hbm_set_busy_ndiv(&hbm, bar, 10, t) == -EINVAL);
	assert(gu_hbm_set_busy_ndiv(&hbm, bar, 99, t) == -EINVAL);

	/* 3. Set busy NDIV 68 while busy -> all 16 active FBPAs lock at 68, FBPA0 untouched. */
	t += 50000000ull;
	assert(gu_hbm_set_busy_ndiv(&hbm, bar, 68, t) == 0);
	assert(((hbm.busy_coeff >> 8) & 0xffu) == 68u);
	assert(hbm.target_busy_ndiv == 68u);
	assert(!hbm.idle);
	for (unsigned int i = 0; i < 24; i++) {
		if (SIM_ACTIVE_MASK & (1u << i))
			assert(((sim.pll_coeff[i] >> 8) & 0xffu) == 68u);
		else
			assert(sim.pll_coeff[i] == 0xbadf2010u);
	}

	/* 4. Poll for >5s with no counter delta -> governor enters idle (NDIV 20, refresh 48). */
	for (unsigned int step = 0; step < 60; step++) {
		t += 100000000ull; /* 100ms steps (below 1s MAX_SAMPLE_NS) */
		assert(gu_hbm_poll(&hbm, bar, t) == 0);
	}
	assert(hbm.idle);
	assert(((sim.pll_coeff[2] >> 8) & 0xffu) == GU_HBM_IDLE_NDIV);
	assert(((sim.refresh[2] & 0x7ff8u) >> 3) == GU_HBM_IDLE_REFRESH);

	/* 5. Activity counter ticks -> governor wakes straight to boosted NDIV 68 + stock refresh. */
	t += 50000000ull;
	sim.ctr_count[0] += 100;
	assert(gu_hbm_poll(&hbm, bar, t) == 0);
	assert(!hbm.idle);
	assert(((sim.pll_coeff[2] >> 8) & 0xffu) == 68u);
	assert(((sim.pll_coeff[23] >> 8) & 0xffu) == 68u);
	assert((sim.refresh[2] & 0x7ff8u) == 0x90u);

	/* 6. Enter idle again, then set busy NDIV 70 directly while idle -> wakes cleanly to 70! */
	for (unsigned int step = 0; step < 60; step++) {
		t += 100000000ull;
		assert(gu_hbm_poll(&hbm, bar, t) == 0);
	}
	assert(hbm.idle);
	t += 50000000ull;
	assert(gu_hbm_set_busy_ndiv(&hbm, bar, 70, t) == 0);
	assert(!hbm.idle);
	assert(((hbm.busy_coeff >> 8) & 0xffu) == 70u);
	assert(((sim.pll_coeff[2] >> 8) & 0xffu) == 70u);
	assert((sim.refresh[2] & 0x7ff8u) == 0x90u);

	/* 7. Simulated lock timeout rolls back cleanly to NDIV 70 without corrupting state. */
	sim.fail_next_lock = true;
	t += 50000000ull;
	assert(gu_hbm_set_busy_ndiv(&hbm, bar, 74, t) == -ETIMEDOUT);
	assert(((hbm.busy_coeff >> 8) & 0xffu) == 70u);
	assert(((sim.pll_coeff[2] >> 8) & 0xffu) == 70u);
	assert(!hbm.fb_stopped);

	/* 8. Reset with ndiv=0 restores factory stock NDIV 64. */
	t += 50000000ull;
	assert(gu_hbm_set_busy_ndiv(&hbm, bar, 0, t) == 0);
	assert(hbm.target_busy_ndiv == 0u);
	assert(((hbm.busy_coeff >> 8) & 0xffu) == 64u);
	assert(((sim.pll_coeff[2] >> 8) & 0xffu) == 64u);

	printf("PASS: gpu-unlock HBM busy-NDIV + governor coexistence (FBPA0-fused 16/24)\n");
	return 0;
}
