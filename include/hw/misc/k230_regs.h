/*
 * K230 scratch register block
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_REGS_H
#define HW_MISC_K230_REGS_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_REGS "riscv.k230.regs"
OBJECT_DECLARE_SIMPLE_TYPE(K230RegsState, K230_REGS)

#define K230_REGS_DEFAULT_SIZE 0x1000
#define K230_REGS_STORAGE_SIZE 0x1000

struct K230RegsState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint64_t size;
    uint8_t regs[K230_REGS_STORAGE_SIZE];
};

#endif /* HW_MISC_K230_REGS_H */
