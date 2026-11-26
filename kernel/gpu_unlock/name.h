/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_NAME_H
#define GPU_UNLOCK_NAME_H

#include <linux/types.h>

#define GU_NV_NAME_MAX		64U
#define GU_CMP170HX_NAME	"NVIDIA CMP 170HX"
#define GU_CMP170HX_SHORT_NAME	"CMP 170HX"

bool gu_name_rewrite_full(char *buf, size_t len);
bool gu_name_rewrite_short(char *buf, size_t len);
const char *gu_name_device_override(u16 device, const char *current_name);

#if defined(CONFIG_KRETPROBES) && defined(CONFIG_X86_64)
void gu_name_init(void);
void gu_name_exit(void);
#else
static inline void gu_name_init(void) {}
static inline void gu_name_exit(void) {}
#endif

#endif
