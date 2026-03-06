/*
 * RISC-V Debug Module v1.0
 *
 * Copyright (c) 2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * Based on the RISC-V Debug Specification v1.0 (ratified 2025-02-21)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_RISCV_DM_H
#define HW_RISCV_DM_H

#include "hw/core/sysbus.h"
#include "hw/core/register.h"

/* Debug memory layout constants (byte offsets within DM backing store) */
#define RISCV_DM_SIZE       0x1000

/*
 * The DMI register window occupies 0x0..0x103.
 * Place CPU<->DM mailbox words right above it.
 */
#define RISCV_DM_ROM_HARTID     0x104
#define RISCV_DM_ROM_GOING      0x108
#define RISCV_DM_ROM_RESUME     0x10C
#define RISCV_DM_ROM_EXCP       0x110
#define RISCV_DM_ROM_WHERETO    0x300
#define RISCV_DM_ROM_CMD        0x338
#define RISCV_DM_ROM_PROGBUF    0x360
#define RISCV_DM_ROM_DATA       0x3C0
#define RISCV_DM_ROM_FLAGS      0x400
#define RISCV_DM_ROM_ENTRY      0x800
#define RISCV_DM_ROM_WORK_BASE  RISCV_DM_ROM_HARTID
#define RISCV_DM_ROM_WORK_SIZE  (RISCV_DM_ROM_ENTRY - RISCV_DM_ROM_WORK_BASE)
#define RISCV_DM_ROM_ENTRY_SIZE (RISCV_DM_SIZE - RISCV_DM_ROM_ENTRY)

/*
 * Maximum harts addressable by the ROM entry loop.
 * The ROM uses `lbu s0, 0x400(s0)` with a 7-bit masked mhartid,
 * giving 128 directly-addressable harts per DM instance.
 */
#define RISCV_DM_MAX_HARTS      128
#define RISCV_DM_HAWINDOW_SIZE  32

/* Register space: 0x00 - 0x100, word-addressed */
#define RISCV_DM_REG_SIZE       0x104
#define RISCV_DM_R_MAX          (RISCV_DM_REG_SIZE / 4)

/* Hart flag values written into ROM FLAGS area */
enum RISCVDMHartFlag {
    RISCV_DM_FLAG_CLEAR  = 0,
    RISCV_DM_FLAG_GOING  = 1,
    RISCV_DM_FLAG_RESUME = 2,
};

/* Abstract command error codes (CMDERR field) */
enum RISCVDMCmdErr {
    RISCV_DM_CMDERR_NONE       = 0,
    RISCV_DM_CMDERR_BUSY       = 1,
    RISCV_DM_CMDERR_NOTSUP     = 2,
    RISCV_DM_CMDERR_EXCEPTION  = 3,
    RISCV_DM_CMDERR_HALTRESUME = 4,
    RISCV_DM_CMDERR_BUS        = 5,
    RISCV_DM_CMDERR_OTHER      = 7,
};

/* Abstract command types */
enum RISCVDMCmdType {
    RISCV_DM_CMD_ACCESS_REG = 0,
    RISCV_DM_CMD_QUICK_ACCESS = 1,
    RISCV_DM_CMD_ACCESS_MEM = 2,
};

/* Abstract register number ranges */
#define RISCV_DM_REGNO_CSR_START 0x0000
#define RISCV_DM_REGNO_CSR_END   0x0FFF
#define RISCV_DM_REGNO_GPR_START 0x1000
#define RISCV_DM_REGNO_GPR_END   0x101F
#define RISCV_DM_REGNO_FPR_START 0x1020
#define RISCV_DM_REGNO_FPR_END   0x103F

#define TYPE_RISCV_DM "riscv-dm"
OBJECT_DECLARE_SIMPLE_TYPE(RISCVDMState, RISCV_DM)

struct RISCVDMState {
    SysBusDevice parent_obj;

    /* register.h framework */
    RegisterInfoArray *reg_array;
    uint32_t regs[RISCV_DM_R_MAX];
    RegisterInfo regs_info[RISCV_DM_R_MAX];

    /* DM backing store (rom_device) and exported aliases */
    MemoryRegion rom_mr;
    MemoryRegion rom_work_alias_mr;
    MemoryRegion rom_entry_alias_mr;
    uint8_t *rom_ptr;

    /* Per-hart state */
    uint32_t num_harts;
    qemu_irq *halt_irqs;
    bool *hart_halted;
    bool *hart_resumeack;
    bool *hart_havereset;
    bool *hart_resethaltreq;

    /* DM active state (from DMCONTROL.dmactive) */
    bool dm_active;

    /* Hart array mask window */
    uint32_t hawindow[RISCV_DM_MAX_HARTS / RISCV_DM_HAWINDOW_SIZE];

    /* Last executed command (stored for autoexec) */
    uint32_t last_cmd;

    /* QOM properties */
    uint32_t num_abstract_data;  /* datacount: default 2 */
    uint32_t progbuf_size;       /* progbufsize: default 8 */
    bool impebreak;              /* implicit ebreak */
    uint32_t nscratch;           /* dscratch count: default 1 */
    uint32_t sba_addr_width;     /* SBA address bits: default 0 (disabled) */
};

/*
 * CPU-side callbacks: called from ROM write handler or CPU debug logic.
 * These update per-hart state in the DM.
 */
void riscv_dm_hart_halted(RISCVDMState *s, uint32_t hartsel);
void riscv_dm_hart_resumed(RISCVDMState *s, uint32_t hartsel);
void riscv_dm_abstracts_done(RISCVDMState *s, uint32_t hartsel);
void riscv_dm_abstracts_exception(RISCVDMState *s, uint32_t hartsel);

/* Convenience: create, configure, realize, and map the DM device.
 * @base is the start of the DM address space (size RISCV_DM_SIZE). */
DeviceState *riscv_dm_create(MemoryRegion *sys_mem, hwaddr base,
                             uint32_t num_harts);

#endif /* HW_RISCV_DM_H */
