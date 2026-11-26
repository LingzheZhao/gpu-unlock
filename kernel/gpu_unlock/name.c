// SPDX-License-Identifier: GPL-2.0-only
/*
 * Restore the official "NVIDIA CMP 170HX" product name on NVIDIA open kernel
 * modules whose GSP firmware and PCI branding tables omit 0x20C2 / 0x2082.
 *
 * Hooks only public ABI return buffers (64-byte ASCII name string in rsi for
 * gpuGetNameString_FWCLIENT / gpuGetShortNameString_FWCLIENT, and the Device ID
 * lookup return value in rm_get_device_name) with zero private struct offsets.
 * Only replaces the exact generic fallback strings ("NVIDIA Graphics Device",
 * "Graphics Device", "Unknown"), leaving every other GPU untouched.
 */
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/ptrace.h>
#include <linux/string.h>

#include "name.h"

static const char gu_fallback_full[] = "NVIDIA Graphics Device";
static const char gu_fallback_short[] = "Graphics Device";
static const char gu_cmp170hx_full[] = GU_CMP170HX_NAME;
static const char gu_cmp170hx_short[] = GU_CMP170HX_SHORT_NAME;

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

#if defined(CONFIG_KRETPROBES) && defined(CONFIG_X86_64)

struct name_buf_ctx {
	char *buf;
};

struct dev_name_ctx {
	u16 device;
};

static DEFINE_MUTEX(name_lock);
static bool notifier_registered;
static bool full_attached;
static bool short_attached;
static bool proc_attached;

static struct kretprobe full_rp;
static struct kretprobe short_rp;
static struct kretprobe proc_rp;

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
		pr_debug("gpu_unlock: CMP 170HX name probe %s unavailable (%d)\n",
			 symbol, ret);
		return false;
	}
	return true;
}

static void attach_probes(void)
{
	bool added = false;

	mutex_lock(&name_lock);
	if (!full_attached) {
		full_attached = register_one(&full_rp, "gpuGetNameString_FWCLIENT",
					     name_buf_entry, full_name_ret,
					     sizeof(struct name_buf_ctx));
		added |= full_attached;
	}
	if (!short_attached) {
		short_attached = register_one(&short_rp, "gpuGetShortNameString_FWCLIENT",
					      name_buf_entry, short_name_ret,
					      sizeof(struct name_buf_ctx));
		added |= short_attached;
	}
	if (!proc_attached) {
		proc_attached = register_one(&proc_rp, "rm_get_device_name",
					     dev_name_entry, dev_name_ret,
					     sizeof(struct dev_name_ctx));
		added |= proc_attached;
	}
	if (added)
		pr_info("gpu_unlock: CMP 170HX branding override active (full=%d short=%d proc=%d)\n",
			full_attached, short_attached, proc_attached);
	mutex_unlock(&name_lock);
}

static void detach_probes(void)
{
	mutex_lock(&name_lock);
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

void gu_name_init(void)
{
	if (!notifier_registered) {
		if (!register_module_notifier(&nvidia_mod_nb))
			notifier_registered = true;
		else
			pr_warn("gpu_unlock: CMP 170HX name notifier registration failed\n");
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
}

#endif
