/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_NAME_H
#define GPU_UNLOCK_NAME_H

#include <linux/types.h>

struct pci_dev;

#define GU_NV_NAME_MAX		64U
#define GU_CMP170HX_NAME	"NVIDIA CMP 170HX"
#define GU_CMP170HX_SHORT_NAME	"CMP 170HX"

bool gu_name_rewrite_full(char *buf, size_t len);
bool gu_name_rewrite_short(char *buf, size_t len);
const char *gu_name_device_override(u16 device, const char *current_name);
u32 gu_pcie_gen_rewrite(u32 data, u8 hw_max, u8 hw_cur);

#if defined(CONFIG_KRETPROBES) && defined(CONFIG_X86_64)
void gu_name_register_card(struct pci_dev *pdev);
void gu_name_set_pcie_gen(struct pci_dev *pdev, u8 hw_max, u8 hw_cur);
void gu_name_reset_pcie_gen(void);
void gu_name_init(bool enable_name, bool enable_pcie_gen);
void gu_name_exit(void);
#else
static inline void gu_name_register_card(struct pci_dev *pdev) {}
static inline void gu_name_set_pcie_gen(struct pci_dev *pdev, u8 hw_max, u8 hw_cur) {}
static inline void gu_name_reset_pcie_gen(void) {}
static inline void gu_name_init(bool enable_name, bool enable_pcie_gen) {}
static inline void gu_name_exit(void) {}
#endif

#endif
