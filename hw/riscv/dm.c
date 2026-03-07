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
#include "hw/core/qdev-properties.h"
#include "hw/riscv/dm.h"
#include "migration/vmstate.h"

REG32(DMCONTROL, 0x40)
    FIELD(DMCONTROL, DMACTIVE, 0, 1)
    FIELD(DMCONTROL, HARTSELLO, 6, 10)
    FIELD(DMCONTROL, HARTSELHI, 16, 10)
    FIELD(DMCONTROL, HASEL, 26, 1)

REG32(DMSTATUS, 0x44)
    FIELD(DMSTATUS, VERSION, 0, 4)
    FIELD(DMSTATUS, HASRESETHALTREQ, 5, 1)
    FIELD(DMSTATUS, AUTHENTICATED, 7, 1)
    FIELD(DMSTATUS, ANYHALTED, 8, 1)
    FIELD(DMSTATUS, ALLHALTED, 9, 1)
    FIELD(DMSTATUS, ANYRUNNING, 10, 1)
    FIELD(DMSTATUS, ALLRUNNING, 11, 1)
    FIELD(DMSTATUS, ANYNONEXISTENT, 14, 1)
    FIELD(DMSTATUS, ALLNONEXISTENT, 15, 1)
    FIELD(DMSTATUS, ANYRESUMEACK, 16, 1)
    FIELD(DMSTATUS, ALLRESUMEACK, 17, 1)
    FIELD(DMSTATUS, ANYHAVERESET, 18, 1)
    FIELD(DMSTATUS, ALLHAVERESET, 19, 1)
    FIELD(DMSTATUS, IMPEBREAK, 22, 1)

REG32(HARTINFO, 0x48)
    FIELD(HARTINFO, DATAADDR, 0, 12)
    FIELD(HARTINFO, DATASIZE, 12, 4)
    FIELD(HARTINFO, DATAACCESS, 16, 1)
    FIELD(HARTINFO, NSCRATCH, 20, 4)

REG32(ABSTRACTCS, 0x58)
    FIELD(ABSTRACTCS, DATACOUNT, 0, 4)
    FIELD(ABSTRACTCS, CMDERR, 8, 3)
    FIELD(ABSTRACTCS, BUSY, 12, 1)
    FIELD(ABSTRACTCS, PROGBUFSIZE, 24, 5)

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

static void dm_status_refresh(RISCVDMState *s)
{
    uint32_t hartsel = dm_get_hartsel(s);
    bool valid = dm_hart_valid(s, hartsel);
    bool halted = valid && s->hart_halted[hartsel];
    bool running = valid && !s->hart_halted[hartsel];
    bool resumeack = valid && s->hart_resumeack[hartsel];
    bool havereset = valid && s->hart_havereset[hartsel];

    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ANYNONEXISTENT, !valid);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ALLNONEXISTENT, !valid);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ANYHALTED, halted);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ALLHALTED, halted);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ANYRUNNING, running);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ALLRUNNING, running);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ANYRESUMEACK, resumeack);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ALLRESUMEACK, resumeack);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ANYHAVERESET, havereset);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, ALLHAVERESET, havereset);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, HASRESETHALTREQ, 1);
}

static void dm_debug_reset(RISCVDMState *s);

static uint64_t dm_dmcontrol_pre_write(RegisterInfo *reg, uint64_t val64)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    uint32_t val = val64;

    if (!FIELD_EX32(val, DMCONTROL, DMACTIVE)) {
        dm_debug_reset(s);
        return 0;
    }

    s->dm_active = true;

    val = FIELD_DP32(val, DMCONTROL, DMACTIVE, 1);
    s->regs[R_DMCONTROL] = val;
    dm_status_refresh(s);
    return val;
}

static uint64_t dm_dmstatus_post_read(RegisterInfo *reg, uint64_t val)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);

    dm_status_refresh(s);
    return val;
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

static RegisterAccessInfo riscv_dm_regs_info[] = {
    { .name = "DMCONTROL", .addr = A_DMCONTROL,
      .pre_write = dm_dmcontrol_pre_write, },
    { .name = "DMSTATUS", .addr = A_DMSTATUS,
      .ro = 0xffffffff,
      .post_read = dm_dmstatus_post_read, },
    { .name = "HARTINFO", .addr = A_HARTINFO,
      .ro = 0xffffffff,
      .post_read = dm_hartinfo_post_read, },
    { .name = "ABSTRACTCS", .addr = A_ABSTRACTCS,
      .ro = 0xffffffff, },
};

static uint64_t riscv_dm_read(void *opaque, hwaddr addr, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RISCVDMState *s = RISCV_DM(reg_array->r[0]->opaque);

    if (!s->dm_active && addr != A_DMCONTROL) {
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

    return ldn_le_p(s->rom_ptr + offset, size);
}

static void dm_rom_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
    RISCVDMState *s = opaque;

    stn_le_p(s->rom_ptr + offset, size, value);

    if (offset == RISCV_DM_ROM_HARTID && size == 4) {
        riscv_dm_hart_halted(s, value);
        return;
    }

    if (offset == RISCV_DM_ROM_RESUME && size == 4) {
        riscv_dm_hart_resumed(s, value);
        return;
    }

    if (offset == RISCV_DM_ROM_EXCP && size == 4) {
        riscv_dm_abstracts_exception(s, dm_get_hartsel(s));
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
}

void riscv_dm_hart_resumed(RISCVDMState *s, uint32_t hartsel)
{
    if (!dm_hart_valid(s, hartsel)) {
        return;
    }

    s->hart_halted[hartsel] = false;
    s->hart_resumeack[hartsel] = true;
    dm_status_refresh(s);
}

void riscv_dm_abstracts_done(RISCVDMState *s, uint32_t hartsel)
{
    (void)hartsel;
    ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, BUSY, 0);
}

void riscv_dm_abstracts_exception(RISCVDMState *s, uint32_t hartsel)
{
    (void)hartsel;
    ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, BUSY, 0);
    ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, CMDERR,
                     RISCV_DM_CMDERR_EXCEPTION);
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
        memset(s->hart_halted, 0, s->num_harts * sizeof(bool));
        memset(s->hart_resumeack, 0, s->num_harts * sizeof(bool));
        memset(s->hart_havereset, 1, s->num_harts * sizeof(bool));
    }

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

    s->hart_halted = g_new0(bool, s->num_harts);
    s->hart_resumeack = g_new0(bool, s->num_harts);
    s->hart_havereset = g_new0(bool, s->num_harts);

    if (!dm_rom_realize(s, errp)) {
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
