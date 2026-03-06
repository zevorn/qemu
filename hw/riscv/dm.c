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

REG32(DMSTATUS, 0x44)
    FIELD(DMSTATUS, VERSION, 0, 4)
    FIELD(DMSTATUS, AUTHENTICATED, 7, 1)

REG32(HARTINFO, 0x48)

REG32(ABSTRACTCS, 0x58)
    FIELD(ABSTRACTCS, DATACOUNT, 0, 4)
    FIELD(ABSTRACTCS, PROGBUFSIZE, 24, 5)

static RegisterAccessInfo riscv_dm_regs_info[] = {
    { .name = "DMCONTROL", .addr = A_DMCONTROL, },
    { .name = "DMSTATUS", .addr = A_DMSTATUS, .ro = 0xffffffff, },
    { .name = "HARTINFO", .addr = A_HARTINFO, .ro = 0xffffffff, },
    { .name = "ABSTRACTCS", .addr = A_ABSTRACTCS, .ro = 0xffffffff, },
};

static uint64_t riscv_dm_read(void *opaque, hwaddr addr, unsigned size)
{
    return register_read_memory(opaque, addr, size);
}

static void riscv_dm_write(void *opaque, hwaddr addr,
                           uint64_t value, unsigned size)
{
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
    (void)s;
    (void)hartsel;
}

void riscv_dm_hart_resumed(RISCVDMState *s, uint32_t hartsel)
{
    (void)s;
    (void)hartsel;
}

void riscv_dm_abstracts_done(RISCVDMState *s, uint32_t hartsel)
{
    (void)s;
    (void)hartsel;
}

void riscv_dm_abstracts_exception(RISCVDMState *s, uint32_t hartsel)
{
    (void)s;
    (void)hartsel;
}

static void dm_debug_reset(RISCVDMState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, VERSION, 3);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, AUTHENTICATED, 1);
    ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, DATACOUNT, s->num_abstract_data);
    ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, PROGBUFSIZE, s->progbuf_size);

    if (s->rom_ptr) {
        memset(s->rom_ptr, 0, RISCV_DM_SIZE);
    }
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
        VMSTATE_END_OF_LIST(),
    }
};

static void riscv_dm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = riscv_dm_realize;
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
