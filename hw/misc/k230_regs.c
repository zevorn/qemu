/*
 * K230 scratch register block
 *
 * Copyright (c) 2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/k230_regs.h"
#include "migration/vmstate.h"

static uint64_t k230_regs_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230RegsState *s = K230_REGS(opaque);
    uint64_t val = 0;

    if (addr >= K230_REGS_STORAGE_SIZE ||
        size > K230_REGS_STORAGE_SIZE - addr) {
        return 0;
    }

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)s->regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_regs_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned int size)
{
    K230RegsState *s = K230_REGS(opaque);

    if (addr >= K230_REGS_STORAGE_SIZE ||
        size > K230_REGS_STORAGE_SIZE - addr) {
        return;
    }

    for (int i = 0; i < size; i++) {
        s->regs[addr + i] = val >> (i * 8);
    }
}

static const MemoryRegionOps k230_regs_ops = {
    .read = k230_regs_read,
    .write = k230_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void k230_regs_reset(DeviceState *dev)
{
    K230RegsState *s = K230_REGS(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static const VMStateDescription vmstate_k230_regs = {
    .name = TYPE_K230_REGS,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230RegsState, K230_REGS_STORAGE_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_regs_realize(DeviceState *dev, Error **errp)
{
    K230RegsState *s = K230_REGS(dev);

    if (!s->size) {
        s->size = K230_REGS_DEFAULT_SIZE;
    }

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_regs_ops, s,
                          TYPE_K230_REGS, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static const Property k230_regs_properties[] = {
    DEFINE_PROP_UINT64("size", K230RegsState, size,
                       K230_REGS_DEFAULT_SIZE),
};

static void k230_regs_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_regs_realize;
    device_class_set_legacy_reset(dc, k230_regs_reset);
    device_class_set_props(dc, k230_regs_properties);
    dc->vmsd = &vmstate_k230_regs;
    dc->desc = "K230 scratch register block";
}

static const TypeInfo k230_regs_type_info = {
    .name = TYPE_K230_REGS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230RegsState),
    .class_init = k230_regs_class_init,
};

static void k230_regs_register_types(void)
{
    type_register_static(&k230_regs_type_info);
}

type_init(k230_regs_register_types)
