// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime NVML/procfs reporting fixes for unlocked CMP 170HX cards on NVIDIA
 * open kernel modules:
 * 1. Restore the official "NVIDIA CMP 170HX" product name where GSP firmware
 *    and PCI branding tables omit 0x20C2 / 0x2082.
 * 2. Report real hardware PCIe Gen4 link speed through NV2080_CTRL_BUS_INFO
 *    when post-driver E1 retrained the link to 16 GT/s while closed GSP-RM
 *    still clamps NV2080_CTRL_BUS_INFO_INDEX_PCIE_GEN_INFO to its boot Gen3.
 *
 * Every hook operates only on public ABI structs/buffers with zero dependency
 * on private NVIDIA struct offsets and zero PCI/mutex I/O in probe handlers.
 */
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/ptrace.h>
#include <linux/string.h>

#include "name.h"

static const char gu_fallback_full[] = "NVIDIA Graphics Device";
static const char gu_fallback_short[] = "Graphics Device";
static const char gu_cmp170hx_full[] = GU_CMP170HX_NAME;
static const char gu_cmp170hx_short[] = GU_CMP170HX_SHORT_NAME;

#define GU_NV_BUS_INFO_PCIE_GEN_INFO	0x2du
#define GU_BUS_GEN_SHIFT		12
#define GU_BUS_CURR_SHIFT		16
#define GU_BUS_GPU_GEN_SHIFT		20
#define GU_BUS_GEN_MASK			0xfu
#define GU_RUSD_POLL_PCI		(1ULL << 5) /* NV00DE_RUSD_POLL_PCI */
#define GU_NV_ERR_NOT_SUPPORTED		0x56u

static bool buf_equals(const char *buf, size_t len, const char *lit, size_t lit_len)
{
	return buf && len > lit_len && !memcmp(buf, lit, lit_len + 1);
}

bool gu_name_rewrite_full(char *buf, size_t len)
{
	if (!buf || len < GU_NV_NAME_MAX)
		return false;
	if (!buf_equals(buf, len, gu_fallback_full, sizeof(gu_fallback_full) - 1))
		return false;
	memset(buf, 0, len);
	memcpy(buf, gu_cmp170hx_full, sizeof(gu_cmp170hx_full));
	return true;
}

bool gu_name_rewrite_short(char *buf, size_t len)
{
	if (!buf || len < GU_NV_NAME_MAX)
		return false;
	if (!buf_equals(buf, len, gu_fallback_short, sizeof(gu_fallback_short) - 1) &&
	    !buf_equals(buf, len, gu_fallback_full, sizeof(gu_fallback_full) - 1))
		return false;
	memset(buf, 0, len);
	memcpy(buf, gu_cmp170hx_short, sizeof(gu_cmp170hx_short));
	return true;
}

const char *gu_name_device_override(u16 device, const char *current_name)
{
	if (device != 0x20c2 && device != 0x2082)
		return NULL;
	if (!current_name || !strcmp(current_name, "Unknown"))
		return gu_cmp170hx_full;
	return NULL;
}

/* Rewrite stale Gen3-clamped NV2080_CTRL_BUS_INFO_INDEX_PCIE_GEN_INFO data
 * when GSP-RM advertises GPU_GEN >= Gen4 (0-based 3) while clamping link GEN
 * below Gen4 (< 3), and hardware has actually trained to Gen4+. */
u32 gu_pcie_gen_rewrite(u32 data, u8 hw_max, u8 hw_cur)
{
	u32 max_gen = (data >> GU_BUS_GEN_SHIFT) & GU_BUS_GEN_MASK;
	u32 cur_gen = (data >> GU_BUS_CURR_SHIFT) & GU_BUS_GEN_MASK;
	u32 gpu_gen = (data >> GU_BUS_GPU_GEN_SHIFT) & GU_BUS_GEN_MASK;
	u32 target_max, target_cur;

	if (hw_max < 4 || hw_cur < 4)
		return data;
	if (gpu_gen < 3 || max_gen >= 3)
		return data;
	target_max = (u32)(hw_max - 1) < gpu_gen ? (u32)(hw_max - 1) : gpu_gen;
	if (target_max <= max_gen)
		return data;
	data = (data & ~(GU_BUS_GEN_MASK << GU_BUS_GEN_SHIFT)) |
	       (target_max << GU_BUS_GEN_SHIFT);
	if (cur_gen >= max_gen) {
		target_cur = (u32)(hw_cur - 1) < target_max ?
			     (u32)(hw_cur - 1) : target_max;
		data = (data & ~(GU_BUS_GEN_MASK << GU_BUS_CURR_SHIFT)) |
		       (target_cur << GU_BUS_CURR_SHIFT);
	}
	return data;
}

#if defined(CONFIG_KRETPROBES) && defined(CONFIG_X86_64)

#define GU_MAX_TRACKED_CARDS	16

struct gu_nv_bus_info {
	u32 index;
	u32 data;
};

struct name_buf_ctx {
	char *buf;
};

struct dev_name_ctx {
	u16 device;
};

struct pcie_info_ctx {
	struct gu_nv_bus_info *info;
};

struct rusd_poll_ctx {
	u64 *mask;
	u64 orig_mask;
};

struct tracked_card {
	struct pci_dev *pdev;
	u8 hw_max;
	u8 hw_cur;
};

static DEFINE_MUTEX(name_lock);
static bool notifier_registered;
static bool want_name;
static bool want_pcie_gen;
static bool full_attached;
static bool short_attached;
static bool proc_attached;
static bool pcie_attached;
static bool rusd_attached;

static struct tracked_card tracked[GU_MAX_TRACKED_CARDS];
static unsigned int tracked_count;
static u32 pcie_report_gen; /* packed (min_max << 8) | min_cur, or 0 */

static struct kretprobe full_rp;
static struct kretprobe short_rp;
static struct kretprobe proc_rp;
static struct kretprobe pcie_rp;
static struct kretprobe rusd_rp;

static void recompute_pcie_report(void)
{
	u8 min_max = 0xff, min_cur = 0xff;
	unsigned int i;

	if (!tracked_count) {
		WRITE_ONCE(pcie_report_gen, 0);
		return;
	}
	for (i = 0; i < tracked_count; i++) {
		if (tracked[i].hw_max < min_max)
			min_max = tracked[i].hw_max;
		if (tracked[i].hw_cur < min_cur)
			min_cur = tracked[i].hw_cur;
	}
	if (min_max >= 4 && min_cur >= 4)
		WRITE_ONCE(pcie_report_gen, ((u32)min_max << 8) | min_cur);
	else
		WRITE_ONCE(pcie_report_gen, 0);
}

void gu_name_register_card(struct pci_dev *pdev)
{
	unsigned int i;

	if (!pdev)
		return;
	mutex_lock(&name_lock);
	for (i = 0; i < tracked_count; i++) {
		if (tracked[i].pdev == pdev) {
			mutex_unlock(&name_lock);
			return;
		}
	}
	if (tracked_count < GU_MAX_TRACKED_CARDS) {
		tracked[tracked_count].pdev = pdev;
		tracked[tracked_count].hw_max = 0;
		tracked[tracked_count].hw_cur = 0;
		tracked_count++;
		recompute_pcie_report();
	}
	mutex_unlock(&name_lock);
}

void gu_name_set_pcie_gen(struct pci_dev *pdev, u8 hw_max, u8 hw_cur)
{
	unsigned int i;

	if (!pdev)
		return;
	mutex_lock(&name_lock);
	for (i = 0; i < tracked_count; i++) {
		if (tracked[i].pdev == pdev) {
			tracked[i].hw_max = hw_max;
			tracked[i].hw_cur = hw_cur;
			recompute_pcie_report();
			break;
		}
	}
	mutex_unlock(&name_lock);
}

void gu_name_reset_pcie_gen(void)
{
	unsigned int i;

	mutex_lock(&name_lock);
	for (i = 0; i < tracked_count; i++) {
		tracked[i].hw_max = 0;
		tracked[i].hw_cur = 0;
	}
	WRITE_ONCE(pcie_report_gen, 0);
	mutex_unlock(&name_lock);
}

static int name_buf_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct name_buf_ctx *ctx = (struct name_buf_ctx *)ri->data;

	ctx->buf = (char *)regs_get_kernel_argument(regs, 1);
	return ctx->buf ? 0 : 1;
}

static int full_name_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct name_buf_ctx *ctx = (struct name_buf_ctx *)ri->data;

	if (regs_return_value(regs) == 0)
		gu_name_rewrite_full(ctx->buf, GU_NV_NAME_MAX);
	return 0;
}

static int short_name_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct name_buf_ctx *ctx = (struct name_buf_ctx *)ri->data;

	if (regs_return_value(regs) == 0)
		gu_name_rewrite_short(ctx->buf, GU_NV_NAME_MAX);
	return 0;
}

static int dev_name_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct dev_name_ctx *ctx = (struct dev_name_ctx *)ri->data;

	ctx->device = (u16)regs_get_kernel_argument(regs, 0);
	return (ctx->device == 0x20c2 || ctx->device == 0x2082) ? 0 : 1;
}

static int dev_name_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct dev_name_ctx *ctx = (struct dev_name_ctx *)ri->data;
	const char *cur = (const char *)regs_return_value(regs);
	const char *override = gu_name_device_override(ctx->device, cur);

	if (override)
		regs_set_return_value(regs, (unsigned long)override);
	return 0;
}

static int pcie_info_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct pcie_info_ctx *ctx = (struct pcie_info_ctx *)ri->data;
	struct gu_nv_bus_info *info;

	if (!READ_ONCE(pcie_report_gen))
		return 1;
	info = (struct gu_nv_bus_info *)regs_get_kernel_argument(regs, 2);
	if (!info || info->index != GU_NV_BUS_INFO_PCIE_GEN_INFO)
		return 1;
	ctx->info = info;
	return 0;
}

static int pcie_info_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct pcie_info_ctx *ctx = (struct pcie_info_ctx *)ri->data;
	u32 packed;

	if (regs_return_value(regs) != 0)
		return 0;
	packed = READ_ONCE(pcie_report_gen);
	if (!packed)
		return 0;
	ctx->info->data = gu_pcie_gen_rewrite(ctx->info->data,
					      (u8)(packed >> 8),
					      (u8)(packed & 0xffu));
	return 0;
}

/* On R550+ GSP drivers, NVML polls PCIe link generation via RM User Shared
 * Data (RUSD, NV00DE_RUSD_POLL_PCI), where GSP DMAs its cached boot Gen3
 * directly into host RAM bypassing kbusSendBusInfo_IMPL. Strip POLL_PCI from
 * RUSD poll requests (returning NV_ERR_NOT_SUPPORTED for PCI-only polls) so
 * NVML falls back to NV2080_CTRL_CMD_BUS_GET_INFO -> kbusSendBusInfo_IMPL,
 * while all other RUSD telemetry (power, clocks, temps, util) stays intact. */
static int rusd_poll_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct rusd_poll_ctx *ctx = (struct rusd_poll_ctx *)ri->data;
	u64 *mask;

	if (!READ_ONCE(pcie_report_gen))
		return 1;
	mask = (u64 *)regs_get_kernel_argument(regs, 1);
	if (!mask || !(*mask & GU_RUSD_POLL_PCI))
		return 1;
	ctx->mask = mask;
	ctx->orig_mask = *mask;
	*mask &= ~GU_RUSD_POLL_PCI;
	return 0;
}

static int rusd_poll_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct rusd_poll_ctx *ctx = (struct rusd_poll_ctx *)ri->data;

	*ctx->mask = ctx->orig_mask;
	if (ctx->orig_mask == GU_RUSD_POLL_PCI && regs_return_value(regs) == 0)
		regs_set_return_value(regs, GU_NV_ERR_NOT_SUPPORTED);
	return 0;
}

static bool register_one(struct kretprobe *rp, const char *symbol,
			 kretprobe_handler_t entry, kretprobe_handler_t ret_fn,
			 size_t data_size)
{
	int ret;

	memset(rp, 0, sizeof(*rp));
	rp->kp.symbol_name = symbol;
	rp->entry_handler = entry;
	rp->handler = ret_fn;
	rp->data_size = data_size;
	ret = register_kretprobe(rp);
	if (ret) {
		pr_debug("gpu_unlock: CMP 170HX probe %s unavailable (%d)\n",
			 symbol, ret);
		return false;
	}
	return true;
}

static void attach_probes(void)
{
	bool added = false;

	mutex_lock(&name_lock);
	if (want_name && !full_attached) {
		full_attached = register_one(&full_rp, "gpuGetNameString_FWCLIENT",
					     name_buf_entry, full_name_ret,
					     sizeof(struct name_buf_ctx));
		added |= full_attached;
	}
	if (want_name && !short_attached) {
		short_attached = register_one(&short_rp, "gpuGetShortNameString_FWCLIENT",
					      name_buf_entry, short_name_ret,
					      sizeof(struct name_buf_ctx));
		added |= short_attached;
	}
	if (want_name && !proc_attached) {
		proc_attached = register_one(&proc_rp, "rm_get_device_name",
					     dev_name_entry, dev_name_ret,
					     sizeof(struct dev_name_ctx));
		added |= proc_attached;
	}
	if (want_pcie_gen && !pcie_attached) {
		pcie_attached = register_one(&pcie_rp, "kbusSendBusInfo_IMPL",
					     pcie_info_entry, pcie_info_ret,
					     sizeof(struct pcie_info_ctx));
		added |= pcie_attached;
	}
	if (want_pcie_gen && pcie_attached && !rusd_attached) {
		rusd_attached = register_one(&rusd_rp,
					     "gpushareddataCtrlCmdRequestDataPoll_IMPL",
					     rusd_poll_entry, rusd_poll_ret,
					     sizeof(struct rusd_poll_ctx));
		added |= rusd_attached;
	}
	if (added)
		pr_info("gpu_unlock: CMP 170HX runtime hooks active (name=%d/%d/%d pcie_gen=%d/%d)\n",
			full_attached, short_attached, proc_attached,
			pcie_attached, rusd_attached);
	mutex_unlock(&name_lock);
}

static void detach_probes(void)
{
	mutex_lock(&name_lock);
	if (rusd_attached) {
		unregister_kretprobe(&rusd_rp);
		rusd_attached = false;
	}
	if (pcie_attached) {
		unregister_kretprobe(&pcie_rp);
		pcie_attached = false;
	}
	if (proc_attached) {
		unregister_kretprobe(&proc_rp);
		proc_attached = false;
	}
	if (short_attached) {
		unregister_kretprobe(&short_rp);
		short_attached = false;
	}
	if (full_attached) {
		unregister_kretprobe(&full_rp);
		full_attached = false;
	}
	mutex_unlock(&name_lock);
}

static int nvidia_module_notify(struct notifier_block *nb, unsigned long action,
				void *data)
{
	struct module *mod = data;

	if (!mod || strcmp(mod->name, "nvidia"))
		return NOTIFY_DONE;
	if (action == MODULE_STATE_COMING || action == MODULE_STATE_LIVE)
		attach_probes();
	else if (action == MODULE_STATE_GOING)
		detach_probes();
	return NOTIFY_DONE;
}

/* Priority 1 runs ahead of kprobes' own MODULE_STATE_GOING cleanup. */
static struct notifier_block nvidia_mod_nb = {
	.notifier_call = nvidia_module_notify,
	.priority = 1,
};

void gu_name_init(bool enable_name, bool enable_pcie_gen)
{
	mutex_lock(&name_lock);
	want_name = enable_name;
	want_pcie_gen = enable_pcie_gen;
	mutex_unlock(&name_lock);

	if (!enable_name && !enable_pcie_gen)
		return;
	if (!notifier_registered) {
		if (!register_module_notifier(&nvidia_mod_nb))
			notifier_registered = true;
		else
			pr_warn("gpu_unlock: CMP 170HX runtime notifier registration failed\n");
	}
	/* Attach immediately if nvidia is already loaded. */
	attach_probes();
}

void gu_name_exit(void)
{
	if (notifier_registered) {
		unregister_module_notifier(&nvidia_mod_nb);
		notifier_registered = false;
	}
	detach_probes();
	mutex_lock(&name_lock);
	tracked_count = 0;
	want_name = false;
	want_pcie_gen = false;
	WRITE_ONCE(pcie_report_gen, 0);
	mutex_unlock(&name_lock);
}

#endif
