/*
 * RISC-V Debug Module v1.0
 *
 * Copyright (c) 2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * Based on the RISC-V Debug Specification v1.0 (ratified 2025-02-21)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/riscv/dm.h"
#include "exec/cpu-common.h"
#include "migration/vmstate.h"
#include "system/tcg.h"
#include "target/riscv/cpu.h"
#include "trace.h"

REG32(DMCONTROL, 0x40)
    FIELD(DMCONTROL, DMACTIVE, 0, 1)
    FIELD(DMCONTROL, NDMRESET, 1, 1)
    FIELD(DMCONTROL, CLRRESETHALTREQ, 2, 1)
    FIELD(DMCONTROL, SETRESETHALTREQ, 3, 1)
    FIELD(DMCONTROL, HARTSELLO, 6, 10)
    FIELD(DMCONTROL, HARTSELHI, 16, 10)
    FIELD(DMCONTROL, HASEL, 26, 1)
    FIELD(DMCONTROL, ACKHAVERESET, 28, 1)
    FIELD(DMCONTROL, HARTRESET, 29, 1)
    FIELD(DMCONTROL, RESUMEREQ, 30, 1)
    FIELD(DMCONTROL, HALTREQ, 31, 1)

REG32(DMSTATUS, 0x44)
    FIELD(DMSTATUS, VERSION, 0, 4)
    FIELD(DMSTATUS, HASRESETHALTREQ, 5, 1)
    FIELD(DMSTATUS, AUTHENTICATED, 7, 1)
    FIELD(DMSTATUS, ANYHALTED, 8, 1)
    FIELD(DMSTATUS, ALLHALTED, 9, 1)
    FIELD(DMSTATUS, ANYRUNNING, 10, 1)
    FIELD(DMSTATUS, ALLRUNNING, 11, 1)
    FIELD(DMSTATUS, ANYUNAVAIL, 12, 1)
    FIELD(DMSTATUS, ALLUNAVAIL, 13, 1)
    FIELD(DMSTATUS, ANYNONEXISTENT, 14, 1)
    FIELD(DMSTATUS, ALLNONEXISTENT, 15, 1)
    FIELD(DMSTATUS, ANYRESUMEACK, 16, 1)
    FIELD(DMSTATUS, ALLRESUMEACK, 17, 1)
    FIELD(DMSTATUS, ANYHAVERESET, 18, 1)
    FIELD(DMSTATUS, ALLHAVERESET, 19, 1)
    FIELD(DMSTATUS, IMPEBREAK, 22, 1)
    FIELD(DMSTATUS, NDMRESETPENDING, 24, 1)

REG32(HARTINFO, 0x48)
    FIELD(HARTINFO, DATAADDR, 0, 12)
    FIELD(HARTINFO, DATASIZE, 12, 4)
    FIELD(HARTINFO, DATAACCESS, 16, 1)
    FIELD(HARTINFO, NSCRATCH, 20, 4)

REG32(HALTSUM1, 0x4c)

REG32(HAWINDOWSEL, 0x50)
    FIELD(HAWINDOWSEL, HAWINDOWSEL, 0, 15)

REG32(HAWINDOW, 0x54)

REG32(ABSTRACTCS, 0x58)
    FIELD(ABSTRACTCS, DATACOUNT, 0, 4)
    FIELD(ABSTRACTCS, CMDERR, 8, 3)
    FIELD(ABSTRACTCS, BUSY, 12, 1)
    FIELD(ABSTRACTCS, PROGBUFSIZE, 24, 5)

REG32(HALTSUM0, 0x100)

typedef struct DMHartSelection {
    int all[RISCV_DM_HAWINDOW_SIZE + 1];
    int all_count;
    int harts[RISCV_DM_HAWINDOW_SIZE + 1];
    int valid_count;
} DMHartSelection;

static inline uint32_t dm_get_hartsel(RISCVDMState *s)
{
    uint32_t hi = ARRAY_FIELD_EX32(s->regs, DMCONTROL, HARTSELHI);
    uint32_t lo = ARRAY_FIELD_EX32(s->regs, DMCONTROL, HARTSELLO);

    return (hi << 10) | lo;
}

static inline bool dm_hart_valid(RISCVDMState *s, uint32_t hartsel)
{
    return hartsel < s->num_harts;
}

static inline uint32_t dm_rom_read32(RISCVDMState *s, uint32_t offset)
{
    return ldl_le_p(s->rom_ptr + offset);
}

static inline void dm_rom_write32(RISCVDMState *s, uint32_t offset,
                                  uint32_t val)
{
    stl_le_p(s->rom_ptr + offset, val);
}

static inline void dm_rom_write8(RISCVDMState *s, uint32_t offset, uint8_t val)
{
    s->rom_ptr[offset] = val;
}

static void dm_selection_add(RISCVDMState *s, DMHartSelection *sel, int hartsel)
{
    for (int i = 0; i < sel->all_count; i++) {
        if (sel->all[i] == hartsel) {
            return;
        }
    }

    sel->all[sel->all_count++] = hartsel;
    if (dm_hart_valid(s, hartsel)) {
        sel->harts[sel->valid_count++] = hartsel;
    }
}

static void dm_collect_selected_harts(RISCVDMState *s, DMHartSelection *sel)
{
    uint32_t hartsel = dm_get_hartsel(s);
    uint32_t wsel;
    uint32_t window;
    uint32_t base;

    memset(sel, 0, sizeof(*sel));
    dm_selection_add(s, sel, hartsel);

    if (!ARRAY_FIELD_EX32(s->regs, DMCONTROL, HASEL)) {
        return;
    }

    wsel = ARRAY_FIELD_EX32(s->regs, HAWINDOWSEL, HAWINDOWSEL);
    if (wsel >= RISCV_DM_MAX_HARTS / RISCV_DM_HAWINDOW_SIZE) {
        return;
    }

    window = s->hawindow[wsel];
    base = wsel * RISCV_DM_HAWINDOW_SIZE;
    for (int i = 0; i < RISCV_DM_HAWINDOW_SIZE; i++) {
        if ((window >> i) & 1) {
            dm_selection_add(s, sel, base + i);
        }
    }
}

static inline bool dm_ndmreset_active(RISCVDMState *s)
{
    return ARRAY_FIELD_EX32(s->regs, DMCONTROL, NDMRESET);
}

static bool dm_reg_present(RISCVDMState *s, hwaddr addr)
{
    switch (addr) {
    case A_HALTSUM1:
        return s->num_harts >= 33;
    default:
        return true;
    }
}

static void dm_status_refresh(RISCVDMState *s)
{
    DMHartSelection sel;
    bool anyhalted = false;
    bool allhalted = true;
    bool anyrunning = false;
    bool allrunning = true;
    bool anyunavail = false;
    bool allunavail = true;
    bool anyresumeack = false;
    bool allresumeack = true;
    bool anyhavereset = false;
    bool allhavereset = true;
    bool anynonexistent;
    bool allnonexistent;
    bool reset_unavail = dm_ndmreset_active(s) ||
                         ARRAY_FIELD_EX32(s->regs, DMCONTROL, HARTRESET);

    dm_collect_selected_harts(s, &sel);

    anynonexistent = sel.all_count > sel.valid_count;
    allnonexistent = sel.valid_count == 0 && sel.all_count > 0;

    if (sel.valid_count == 0) {
        allhalted = false;
        allrunning = false;
        allunavail = false;
        allresumeack = false;
        allhavereset = false;
    }

    for (int i = 0; i < sel.valid_count; i++) {
        int h = sel.harts[i];
        bool halted = s->hart_halted[h];
        bool resumeack = s->hart_resumeack[h];
        bool havereset = s->hart_havereset[h];

        if (reset_unavail) {
            anyunavail = true;
            allhalted = false;
            allrunning = false;
        } else {
            anyhalted |= halted;
            allhalted &= halted;
            anyrunning |= !halted;
            allrunning &= !halted;
        }

        allunavail &= reset_unavail;
        anyresumeack |= resumeack;
        allresumeack &= resumeack;
        anyhavereset |= havereset;
        allhavereset &= havereset;
    }

    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ANYHALTED, anyhalted);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ALLHALTED, allhalted);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ANYRUNNING, anyrunning);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ALLRUNNING, allrunning);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ANYUNAVAIL, anyunavail);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ALLUNAVAIL, allunavail);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ANYNONEXISTENT, anynonexistent);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ALLNONEXISTENT, allnonexistent);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ANYRESUMEACK, anyresumeack);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ALLRESUMEACK, allresumeack);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ANYHAVERESET, anyhavereset);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ALLHAVERESET, allhavereset);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, HASRESETHALTREQ, 1);
}

static void dm_debug_reset(RISCVDMState *s);

static void dm_cpu_reset_on_cpu(CPUState *cpu, run_on_cpu_data data)
{
    bool request_reset_halt = data.host_int;

    cpu_reset(cpu);
    if (request_reset_halt) {
        riscv_cpu_request_dm_halt(RISCV_CPU(cpu), DCSR_CAUSE_RESET);
    }
}

static void dm_note_hart_reset(RISCVDMState *s, uint32_t hartsel)
{
    s->hart_halted[hartsel] = false;
    s->hart_resumeack[hartsel] = false;
    s->hart_havereset[hartsel] = true;
    dm_rom_write8(s, RISCV_DM_ROM_FLAGS + hartsel, RISCV_DM_FLAG_CLEAR);
}

static void dm_reset_hart(RISCVDMState *s, uint32_t hartsel)
{
    CPUState *cs;

    if (!dm_hart_valid(s, hartsel)) {
        return;
    }

    cs = qemu_get_cpu(hartsel);
    if (cs && tcg_enabled()) {
        run_on_cpu(cs, dm_cpu_reset_on_cpu,
                   RUN_ON_CPU_HOST_INT(s->hart_resethaltreq[hartsel]));
    }

    dm_note_hart_reset(s, hartsel);

    if (cs && !tcg_enabled() && s->hart_resethaltreq[hartsel]) {
        riscv_cpu_request_dm_halt(RISCV_CPU(cs), DCSR_CAUSE_RESET);
    }
}

static void dm_reset_selected_harts(RISCVDMState *s, DMHartSelection *sel)
{
    for (int i = 0; i < sel->valid_count; i++) {
        dm_reset_hart(s, sel->harts[i]);
    }
}

static void dm_reset_all_harts(RISCVDMState *s)
{
    for (uint32_t hartsel = 0; hartsel < s->num_harts; hartsel++) {
        dm_reset_hart(s, hartsel);
    }
}

static uint64_t dm_dmcontrol_pre_write(RegisterInfo *reg, uint64_t val64)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    uint32_t val = val64;
    uint32_t cur_ctl = s->regs[R_DMCONTROL];
    bool busy = ARRAY_FIELD_EX32(s->regs, ABSTRACTCS, BUSY);
    bool ndmreset_was = FIELD_EX32(cur_ctl, DMCONTROL, NDMRESET);
    bool hartreset_was = FIELD_EX32(cur_ctl, DMCONTROL, HARTRESET);
    bool dmactive = FIELD_EX32(val, DMCONTROL, DMACTIVE);
    DMHartSelection sel;
    uint32_t stored = 0;

    trace_riscv_dm_control_write(s->regs[R_DMCONTROL], val, busy);

    if (busy) {
        val = FIELD_DP32(val, DMCONTROL, HARTSELHI,
                         ARRAY_FIELD_EX32(s->regs, DMCONTROL, HARTSELHI));
        val = FIELD_DP32(val, DMCONTROL, HARTSELLO,
                         ARRAY_FIELD_EX32(s->regs, DMCONTROL, HARTSELLO));
        val = FIELD_DP32(val, DMCONTROL, HASEL,
                         ARRAY_FIELD_EX32(s->regs, DMCONTROL, HASEL));
        val = FIELD_DP32(val, DMCONTROL, HALTREQ, 0);
        val = FIELD_DP32(val, DMCONTROL, RESUMEREQ, 0);
        val = FIELD_DP32(val, DMCONTROL, ACKHAVERESET, 0);
    }

    if (!dmactive) {
        dm_debug_reset(s);
        return 0;
    }

    s->regs[R_DMCONTROL] = val;
    dm_collect_selected_harts(s, &sel);

    if (FIELD_EX32(val, DMCONTROL, NDMRESET) && !ndmreset_was) {
        dm_reset_all_harts(s);
    }

    if (FIELD_EX32(val, DMCONTROL, HARTRESET) && !hartreset_was) {
        dm_reset_selected_harts(s, &sel);
    }

    ARRAY_FIELD_DP32(s->regs, DMSTATUS, NDMRESETPENDING,
                     FIELD_EX32(val, DMCONTROL, NDMRESET));

    if (!busy && FIELD_EX32(val, DMCONTROL, ACKHAVERESET)) {
        for (int i = 0; i < sel.valid_count; i++) {
            s->hart_havereset[sel.harts[i]] = false;
        }
    }

    if (!busy && FIELD_EX32(val, DMCONTROL, SETRESETHALTREQ)) {
        for (int i = 0; i < sel.valid_count; i++) {
            s->hart_resethaltreq[sel.harts[i]] = true;
        }
    }

    if (!busy && FIELD_EX32(val, DMCONTROL, CLRRESETHALTREQ)) {
        for (int i = 0; i < sel.valid_count; i++) {
            s->hart_resethaltreq[sel.harts[i]] = false;
        }
    }

    if (!busy && FIELD_EX32(val, DMCONTROL, RESUMEREQ) &&
        !FIELD_EX32(val, DMCONTROL, HALTREQ)) {
        for (int i = 0; i < sel.valid_count; i++) {
            int h = sel.harts[i];

            s->hart_resumeack[h] = false;
            dm_rom_write8(s, RISCV_DM_ROM_FLAGS + h, RISCV_DM_FLAG_RESUME);
            trace_riscv_dm_control_resume(h);
        }
    }

    if (!busy) {
        if (FIELD_EX32(val, DMCONTROL, HALTREQ)) {
            for (int i = 0; i < sel.valid_count; i++) {
                int h = sel.harts[i];

                dm_rom_write8(s, RISCV_DM_ROM_FLAGS + h, RISCV_DM_FLAG_CLEAR);
                qemu_set_irq(s->halt_irqs[h], 1);
                trace_riscv_dm_control_halt(h);
            }
        } else {
            for (int i = 0; i < sel.valid_count; i++) {
                qemu_set_irq(s->halt_irqs[sel.harts[i]], 0);
            }
        }
    }

    stored = FIELD_DP32(stored, DMCONTROL, DMACTIVE, 1);
    stored = FIELD_DP32(stored, DMCONTROL, NDMRESET,
                        FIELD_EX32(val, DMCONTROL, NDMRESET));
    stored = FIELD_DP32(stored, DMCONTROL, HARTSELLO,
                        FIELD_EX32(val, DMCONTROL, HARTSELLO));
    stored = FIELD_DP32(stored, DMCONTROL, HARTSELHI,
                        FIELD_EX32(val, DMCONTROL, HARTSELHI));
    stored = FIELD_DP32(stored, DMCONTROL, HASEL,
                        FIELD_EX32(val, DMCONTROL, HASEL));
    stored = FIELD_DP32(stored, DMCONTROL, HARTRESET,
                        FIELD_EX32(val, DMCONTROL, HARTRESET));

    s->dm_active = true;
    dm_status_refresh(s);
    return stored;
}

static uint64_t dm_dmstatus_post_read(RegisterInfo *reg, uint64_t val)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);

    dm_status_refresh(s);
    return s->regs[R_DMSTATUS];
}

static uint64_t dm_hartinfo_post_read(RegisterInfo *reg, uint64_t val)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    uint32_t v = val;

    v = FIELD_DP32(v, HARTINFO, DATAADDR, RISCV_DM_ROM_DATA);
    v = FIELD_DP32(v, HARTINFO, DATASIZE, s->num_abstract_data);
    v = FIELD_DP32(v, HARTINFO, DATAACCESS, 1);
    v = FIELD_DP32(v, HARTINFO, NSCRATCH, s->nscratch);
    return v;
}

static uint64_t dm_hawindowsel_pre_write(RegisterInfo *reg, uint64_t val64)
{
    uint32_t max_sel = RISCV_DM_MAX_HARTS / RISCV_DM_HAWINDOW_SIZE;
    uint32_t val = val64 & R_HAWINDOWSEL_HAWINDOWSEL_MASK;

    (void)reg;
    if (val >= max_sel) {
        val = max_sel - 1;
    }
    return val;
}

static uint64_t dm_hawindow_pre_write(RegisterInfo *reg, uint64_t val64)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    uint32_t wsel = ARRAY_FIELD_EX32(s->regs, HAWINDOWSEL, HAWINDOWSEL);

    if (wsel < RISCV_DM_MAX_HARTS / RISCV_DM_HAWINDOW_SIZE) {
        s->hawindow[wsel] = val64;
    }
    return (uint32_t)val64;
}

static uint64_t dm_hawindow_post_read(RegisterInfo *reg, uint64_t val)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    uint32_t wsel = ARRAY_FIELD_EX32(s->regs, HAWINDOWSEL, HAWINDOWSEL);

    (void)val;
    if (wsel < RISCV_DM_MAX_HARTS / RISCV_DM_HAWINDOW_SIZE) {
        return s->hawindow[wsel];
    }
    return 0;
}

static uint64_t dm_haltsum0_post_read(RegisterInfo *reg, uint64_t val)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    uint32_t sum = 0;
    uint32_t base = 0;
    uint32_t limit = MIN(s->num_harts, 32u);

    (void)val;
    for (uint32_t h = base; h < limit; h++) {
        if (s->hart_halted[h]) {
            sum |= 1u << (h - base);
        }
    }
    return sum;
}

static uint64_t dm_haltsum1_post_read(RegisterInfo *reg, uint64_t val)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    uint32_t sum = 0;

    (void)reg;
    (void)val;
    for (uint32_t h = 0; h < s->num_harts; h++) {
        if (s->hart_halted[h]) {
            sum |= 1u << (h / 32);
        }
    }
    return sum;
}

static RegisterAccessInfo riscv_dm_regs_info[] = {
    { .name = "DMCONTROL", .addr = A_DMCONTROL,
      .pre_write = dm_dmcontrol_pre_write, },
    { .name = "DMSTATUS", .addr = A_DMSTATUS,
      .ro = 0xffffffff,
      .post_read = dm_dmstatus_post_read, },
    { .name = "HARTINFO", .addr = A_HARTINFO,
      .ro = 0xffffffff,
      .post_read = dm_hartinfo_post_read, },
    { .name = "HALTSUM1", .addr = A_HALTSUM1,
      .ro = 0xffffffff,
      .post_read = dm_haltsum1_post_read, },
    { .name = "HAWINDOWSEL", .addr = A_HAWINDOWSEL,
      .pre_write = dm_hawindowsel_pre_write, },
    { .name = "HAWINDOW", .addr = A_HAWINDOW,
      .pre_write = dm_hawindow_pre_write,
      .post_read = dm_hawindow_post_read, },
    { .name = "ABSTRACTCS", .addr = A_ABSTRACTCS,
      .ro = 0xffffffff, },
    { .name = "HALTSUM0", .addr = A_HALTSUM0,
      .ro = 0xffffffff,
      .post_read = dm_haltsum0_post_read, },
};

static uint64_t riscv_dm_read(void *opaque, hwaddr addr, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RISCVDMState *s = RISCV_DM(reg_array->r[0]->opaque);

    if (!s->dm_active && addr != A_DMCONTROL) {
        return 0;
    }

    if (dm_ndmreset_active(s)) {
        if (addr == A_DMSTATUS) {
            return s->regs[R_DMSTATUS] & R_DMSTATUS_NDMRESETPENDING_MASK;
        }
        if (addr != A_DMCONTROL) {
            return 0;
        }
    }

    if (!dm_reg_present(s, addr)) {
        return 0;
    }

    return register_read_memory(opaque, addr, size);
}

static void riscv_dm_write(void *opaque, hwaddr addr,
                           uint64_t value, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RISCVDMState *s = RISCV_DM(reg_array->r[0]->opaque);

    if (!s->dm_active && addr != A_DMCONTROL) {
        return;
    }

    if (dm_ndmreset_active(s) && addr != A_DMCONTROL) {
        return;
    }

    if (!dm_reg_present(s, addr)) {
        return;
    }

    register_write_memory(opaque, addr, value, size);
}

static const MemoryRegionOps riscv_dm_ops = {
    .read = riscv_dm_read,
    .write = riscv_dm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/*
 * ROM device read callback.
 *
 * In ROMD mode (the default) reads go directly to the RAM backing store and
 * this callback is never invoked.  It is kept as a fallback for correctness
 * should ROMD mode ever be disabled at runtime.
 */
static uint64_t dm_rom_read(void *opaque, hwaddr offset, unsigned size)
{
    RISCVDMState *s = opaque;
    uint64_t ret;

    ret = ldn_le_p(s->rom_ptr + offset, size);
    trace_riscv_dm_rom_access(offset, ret, size, false);
    return ret;
}

static void dm_rom_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
    RISCVDMState *s = opaque;
    uint32_t hartsel;

    stn_le_p(s->rom_ptr + offset, size, value);

    trace_riscv_dm_rom_access(offset, value, size, true);

    if (offset == RISCV_DM_ROM_HARTID && size == 4) {
        hartsel = value;
        if (dm_hart_valid(s, hartsel)) {
            riscv_dm_hart_halted(s, hartsel);
        }
        return;
    }

    if (offset == RISCV_DM_ROM_GOING && size == 4) {
        hartsel = dm_rom_read32(s, RISCV_DM_ROM_HARTID);
        if (dm_hart_valid(s, hartsel)) {
            dm_rom_write8(s, RISCV_DM_ROM_FLAGS + hartsel,
                          RISCV_DM_FLAG_CLEAR);
            trace_riscv_dm_going(hartsel);
        }
        return;
    }

    if (offset == RISCV_DM_ROM_RESUME && size == 4) {
        hartsel = value;
        if (dm_hart_valid(s, hartsel)) {
            riscv_dm_hart_resumed(s, hartsel);
        }
        return;
    }

    if (offset == RISCV_DM_ROM_EXCP && size == 4) {
        hartsel = dm_rom_read32(s, RISCV_DM_ROM_HARTID);
        if (dm_hart_valid(s, hartsel)) {
            riscv_dm_abstracts_exception(s, hartsel);
        }
    }
}

static const MemoryRegionOps dm_rom_ops = {
    .read = dm_rom_read,
    .write = dm_rom_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static bool dm_rom_realize(RISCVDMState *s, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(s);

    if (!memory_region_init_rom_device(&s->rom_mr, OBJECT(s), &dm_rom_ops, s,
                                       "riscv-dm.rom", RISCV_DM_SIZE,
                                       errp)) {
        return false;
    }

    s->rom_ptr = memory_region_get_ram_ptr(&s->rom_mr);

    memory_region_init_alias(&s->rom_work_alias_mr, OBJECT(s),
                             "riscv-dm.rom-work", &s->rom_mr,
                             RISCV_DM_ROM_WORK_BASE, RISCV_DM_ROM_WORK_SIZE);
    memory_region_init_alias(&s->rom_entry_alias_mr, OBJECT(s),
                             "riscv-dm.rom-entry", &s->rom_mr,
                             RISCV_DM_ROM_ENTRY, RISCV_DM_ROM_ENTRY_SIZE);

    sysbus_init_mmio(sbd, &s->rom_work_alias_mr);
    sysbus_init_mmio(sbd, &s->rom_entry_alias_mr);
    return true;
}

void riscv_dm_hart_halted(RISCVDMState *s, uint32_t hartsel)
{
    if (!dm_hart_valid(s, hartsel)) {
        return;
    }

    s->hart_halted[hartsel] = true;
    riscv_dm_abstracts_done(s, hartsel);
    dm_status_refresh(s);
    trace_riscv_dm_hart_halted(hartsel);
}

void riscv_dm_hart_resumed(RISCVDMState *s, uint32_t hartsel)
{
    if (!dm_hart_valid(s, hartsel)) {
        return;
    }

    s->hart_halted[hartsel] = false;
    s->hart_resumeack[hartsel] = true;
    dm_rom_write8(s, RISCV_DM_ROM_FLAGS + hartsel, RISCV_DM_FLAG_CLEAR);
    dm_status_refresh(s);
    trace_riscv_dm_hart_resumed(hartsel);
}

void riscv_dm_abstracts_done(RISCVDMState *s, uint32_t hartsel)
{
    ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, BUSY, 0);
    trace_riscv_dm_abstract_cmd_complete(hartsel);
}

void riscv_dm_abstracts_exception(RISCVDMState *s, uint32_t hartsel)
{
    ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, BUSY, 0);
    ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, CMDERR,
                     RISCV_DM_CMDERR_EXCEPTION);
    trace_riscv_dm_abstract_cmd_exception(hartsel);
}

static void dm_debug_reset(RISCVDMState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, VERSION, 3);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, AUTHENTICATED, 1);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, IMPEBREAK, s->impebreak);
    ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, DATACOUNT, s->num_abstract_data);
    ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, PROGBUFSIZE, s->progbuf_size);

    if (s->hart_halted) {
        for (uint32_t i = 0; i < s->num_harts; i++) {
            s->hart_halted[i] = false;
            s->hart_resumeack[i] = false;
            s->hart_havereset[i] = true;
            s->hart_resethaltreq[i] = false;
        }
    }

    memset(s->hawindow, 0, sizeof(s->hawindow));
    s->dm_active = false;

    if (s->rom_ptr) {
        memset(s->rom_ptr, 0, RISCV_DM_SIZE);
    }

    dm_status_refresh(s);
}

static void riscv_dm_init(Object *obj)
{
    RISCVDMState *s = RISCV_DM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->reg_array = register_init_block32(DEVICE(obj), riscv_dm_regs_info,
                                         ARRAY_SIZE(riscv_dm_regs_info),
                                         s->regs_info, s->regs, &riscv_dm_ops,
                                         false, RISCV_DM_REG_SIZE);

    sysbus_init_mmio(sbd, &s->reg_array->mem);
}

static void riscv_dm_realize(DeviceState *dev, Error **errp)
{
    RISCVDMState *s = RISCV_DM(dev);

    if (s->num_harts > RISCV_DM_MAX_HARTS) {
        error_setg(errp, "riscv-dm: num-harts %u exceeds maximum %d",
                   s->num_harts, RISCV_DM_MAX_HARTS);
        return;
    }

    if (s->num_harts > 0) {
        s->hart_halted = g_new0(bool, s->num_harts);
        s->hart_resumeack = g_new0(bool, s->num_harts);
        s->hart_havereset = g_new0(bool, s->num_harts);
        s->hart_resethaltreq = g_new0(bool, s->num_harts);
        s->halt_irqs = g_new(qemu_irq, s->num_harts);
        qdev_init_gpio_out(dev, s->halt_irqs, s->num_harts);
    }

    if (!dm_rom_realize(s, errp)) {
        g_free(s->hart_halted);
        g_free(s->hart_resumeack);
        g_free(s->hart_havereset);
        g_free(s->hart_resethaltreq);
        g_free(s->halt_irqs);
        return;
    }

    dm_debug_reset(s);
}

static void riscv_dm_reset_hold(Object *obj, ResetType type)
{
    (void)type;
    dm_debug_reset(RISCV_DM(obj));
}

static void riscv_dm_unrealize(DeviceState *dev)
{
    RISCVDMState *s = RISCV_DM(dev);

    g_free(s->hart_halted);
    g_free(s->hart_resumeack);
    g_free(s->hart_havereset);
    g_free(s->hart_resethaltreq);
    g_free(s->halt_irqs);
}

static const Property riscv_dm_props[] = {
    DEFINE_PROP_UINT32("num-harts", RISCVDMState, num_harts, 1),
    DEFINE_PROP_UINT32("datacount", RISCVDMState, num_abstract_data, 2),
    DEFINE_PROP_UINT32("progbufsize", RISCVDMState, progbuf_size, 8),
    DEFINE_PROP_BOOL("impebreak", RISCVDMState, impebreak, true),
    DEFINE_PROP_UINT32("nscratch", RISCVDMState, nscratch, 1),
    DEFINE_PROP_UINT32("sba-addr-width", RISCVDMState, sba_addr_width, 0),
};

static const VMStateDescription vmstate_riscv_dm = {
    .name = TYPE_RISCV_DM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RISCVDMState, RISCV_DM_R_MAX),
        VMSTATE_BOOL(dm_active, RISCVDMState),
        VMSTATE_VARRAY_UINT32(hart_halted, RISCVDMState, num_harts,
                              0, vmstate_info_bool, bool),
        VMSTATE_VARRAY_UINT32(hart_resumeack, RISCVDMState, num_harts,
                              0, vmstate_info_bool, bool),
        VMSTATE_VARRAY_UINT32(hart_havereset, RISCVDMState, num_harts,
                              0, vmstate_info_bool, bool),
        VMSTATE_VARRAY_UINT32(hart_resethaltreq, RISCVDMState, num_harts,
                              0, vmstate_info_bool, bool),
        VMSTATE_END_OF_LIST(),
    }
};

static void riscv_dm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = riscv_dm_realize;
    dc->unrealize = riscv_dm_unrealize;
    dc->vmsd = &vmstate_riscv_dm;
    device_class_set_props(dc, riscv_dm_props);
    rc->phases.hold = riscv_dm_reset_hold;
}

static const TypeInfo riscv_dm_info = {
    .name          = TYPE_RISCV_DM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RISCVDMState),
    .instance_init = riscv_dm_init,
    .class_init    = riscv_dm_class_init,
};

static void riscv_dm_register_types(void)
{
    type_register_static(&riscv_dm_info);
}

type_init(riscv_dm_register_types)

DeviceState *riscv_dm_create(MemoryRegion *sys_mem, hwaddr base,
                             uint32_t num_harts)
{
    DeviceState *dev = qdev_new(TYPE_RISCV_DM);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    qdev_prop_set_uint32(dev, "num-harts", num_harts);
    sysbus_realize_and_unref(sbd, &error_fatal);

    /* MMIO region 0: DMI register file */
    memory_region_add_subregion(sys_mem, base,
                                sysbus_mmio_get_region(sbd, 0));
    /* MMIO region 1: low debug work area (mailbox, cmd, data, flags) */
    memory_region_add_subregion(sys_mem, base + RISCV_DM_ROM_WORK_BASE,
                                sysbus_mmio_get_region(sbd, 1));
    /* MMIO region 2: debug ROM entry vector */
    memory_region_add_subregion(sys_mem, base + RISCV_DM_ROM_ENTRY,
                                sysbus_mmio_get_region(sbd, 2));
    return dev;
}
