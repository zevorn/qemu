/*
 * K230 system controller blocks
 *
 * Copyright (c) 2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/misc/k230_sysctl.h"

#define K230_SYSCTL_PLL_COUNT 4
#define K230_SYSCTL_PLL_STRIDE 0x10
#define K230_SYSCTL_PLL_CFG0(n) ((n) * K230_SYSCTL_PLL_STRIDE)
#define K230_SYSCTL_PLL_CTL(n)  (K230_SYSCTL_PLL_CFG0(n) + 0x08)
#define K230_SYSCTL_PLL_STAT(n) (K230_SYSCTL_PLL_CFG0(n) + 0x0c)
#define K230_SYSCTL_PLL_GATE_EN BIT(2)
#define K230_SYSCTL_PLL_LOCK    BIT(0)

#define K230_SYSCTL_PWR_ON      BIT(1)
#define K230_SYSCTL_PWR_ON_WEN  BIT(17)
#define K230_SYSCTL_PWR_OFF     BIT(0)
#define K230_SYSCTL_PWR_OFF_WEN BIT(16)
#define K230_SYSCTL_REPAIR_DONE 0x7
#define K230_SYSCTL_AI_REPAIR   BIT(4)
#define K230_SYSCTL_REPAIR_WEN  BIT(20)

typedef struct K230SysctlPowerDomain {
    hwaddr en;
    hwaddr stat;
} K230SysctlPowerDomain;

static const K230SysctlPowerDomain k230_power_domains[] = {
    { 0x018, 0x01c }, /* CPU1 */
    { 0x028, 0x02c }, /* AI */
    { 0x03c, 0x040 }, /* DISP */
    { 0x07c, 0x080 }, /* VPU */
    { 0x108, 0x10c }, /* DPU */
};

static uint64_t k230_sysctl_read_bytes(uint8_t *regs, hwaddr addr,
                                       unsigned int size)
{
    uint64_t val = 0;

    if (addr > K230_SYSCTL_SIZE || size > K230_SYSCTL_SIZE - addr) {
        return 0;
    }

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_sysctl_write_bytes(uint8_t *regs, hwaddr addr, uint64_t val,
                                    unsigned int size)
{
    if (addr > K230_SYSCTL_SIZE || size > K230_SYSCTL_SIZE - addr) {
        return;
    }

    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static uint32_t k230_sysctl_reg_read32(uint8_t *regs, hwaddr addr)
{
    return ldl_le_p(regs + addr);
}

static void k230_sysctl_reg_write32(uint8_t *regs, hwaddr addr, uint32_t val)
{
    stl_le_p(regs + addr, val);
}

static uint64_t k230_sysctl_boot_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    return k230_sysctl_read_bytes(K230_SYSCTL_BOOT(opaque)->regs, addr, size);
}

static void k230_sysctl_boot_refresh_locks(K230SysctlBootState *s)
{
    for (int i = 0; i < K230_SYSCTL_PLL_COUNT; i++) {
        hwaddr stat = K230_SYSCTL_PLL_STAT(i);

        k230_sysctl_reg_write32(s->regs, stat,
            k230_sysctl_reg_read32(s->regs, stat) | K230_SYSCTL_PLL_LOCK);
    }
}

static void k230_sysctl_boot_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned int size)
{
    K230SysctlBootState *s = K230_SYSCTL_BOOT(opaque);

    k230_sysctl_write_bytes(s->regs, addr, val, size);
    k230_sysctl_boot_refresh_locks(s);
}

static const MemoryRegionOps k230_sysctl_boot_ops = {
    .read = k230_sysctl_boot_read,
    .write = k230_sysctl_boot_write,
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

static void k230_sysctl_boot_set_pll(K230SysctlBootState *s, int pll,
                                     uint32_t fb_div, uint32_t ref_div,
                                     uint32_t out_div)
{
    uint32_t cfg0 = ((out_div - 1) << 24) |
                   ((ref_div - 1) << 16) |
                   (fb_div - 1);

    k230_sysctl_reg_write32(s->regs, K230_SYSCTL_PLL_CFG0(pll), cfg0);
    k230_sysctl_reg_write32(s->regs, K230_SYSCTL_PLL_CTL(pll),
                            K230_SYSCTL_PLL_GATE_EN);
    k230_sysctl_reg_write32(s->regs, K230_SYSCTL_PLL_STAT(pll),
                            K230_SYSCTL_PLL_LOCK);
}

static void k230_sysctl_boot_reset(DeviceState *dev)
{
    K230SysctlBootState *s = K230_SYSCTL_BOOT(dev);

    memset(s->regs, 0, sizeof(s->regs));

    k230_sysctl_boot_set_pll(s, 0, 200, 3, 1);
    k230_sysctl_boot_set_pll(s, 1, 99, 1, 1);
    k230_sysctl_boot_set_pll(s, 2, 111, 1, 1);
    k230_sysctl_boot_set_pll(s, 3, 200, 3, 1);
}

static const VMStateDescription vmstate_k230_sysctl_boot = {
    .name = TYPE_K230_SYSCTL_BOOT,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230SysctlBootState, K230_SYSCTL_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_sysctl_boot_realize(DeviceState *dev, Error **errp)
{
    K230SysctlBootState *s = K230_SYSCTL_BOOT(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_sysctl_boot_ops, s,
                          TYPE_K230_SYSCTL_BOOT, K230_SYSCTL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_sysctl_boot_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_sysctl_boot_realize;
    device_class_set_legacy_reset(dc, k230_sysctl_boot_reset);
    dc->vmsd = &vmstate_k230_sysctl_boot;
    dc->desc = "K230 sysctl boot registers";
}

static const TypeInfo k230_sysctl_boot_type_info = {
    .name = TYPE_K230_SYSCTL_BOOT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230SysctlBootState),
    .class_init = k230_sysctl_boot_class_init,
};

static uint64_t k230_sysctl_power_read(void *opaque, hwaddr addr,
                                       unsigned int size)
{
    return k230_sysctl_read_bytes(K230_SYSCTL_POWER(opaque)->regs, addr, size);
}

static void k230_sysctl_power_update_domain(K230SysctlPowerState *s,
                                            hwaddr addr, uint32_t val)
{
    for (int i = 0; i < ARRAY_SIZE(k230_power_domains); i++) {
        const K230SysctlPowerDomain *domain = &k230_power_domains[i];

        if (addr != domain->en) {
            continue;
        }

        if ((val & K230_SYSCTL_PWR_ON_WEN) && (val & K230_SYSCTL_PWR_ON)) {
            k230_sysctl_reg_write32(s->regs, domain->stat,
                                    K230_SYSCTL_PWR_ON);
        } else if ((val & K230_SYSCTL_PWR_OFF_WEN) &&
                   (val & K230_SYSCTL_PWR_OFF)) {
            k230_sysctl_reg_write32(s->regs, domain->stat,
                                    K230_SYSCTL_PWR_OFF);
        }

        if (addr == k230_power_domains[1].en &&
            (val & K230_SYSCTL_REPAIR_WEN) && (val & K230_SYSCTL_AI_REPAIR)) {
            k230_sysctl_reg_write32(s->regs, 0x160, K230_SYSCTL_REPAIR_DONE);
        }
    }
}

static void k230_sysctl_power_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned int size)
{
    K230SysctlPowerState *s = K230_SYSCTL_POWER(opaque);

    k230_sysctl_write_bytes(s->regs, addr, val, size);

    if (size == 4) {
        k230_sysctl_power_update_domain(s, addr, val);
    }
}

static const MemoryRegionOps k230_sysctl_power_ops = {
    .read = k230_sysctl_power_read,
    .write = k230_sysctl_power_write,
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

static void k230_sysctl_power_reset(DeviceState *dev)
{
    K230SysctlPowerState *s = K230_SYSCTL_POWER(dev);

    memset(s->regs, 0, sizeof(s->regs));

    for (int i = 0; i < ARRAY_SIZE(k230_power_domains); i++) {
        k230_sysctl_reg_write32(s->regs, k230_power_domains[i].stat,
                                K230_SYSCTL_PWR_OFF);
    }
}

static const VMStateDescription vmstate_k230_sysctl_power = {
    .name = TYPE_K230_SYSCTL_POWER,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230SysctlPowerState, K230_SYSCTL_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_sysctl_power_realize(DeviceState *dev, Error **errp)
{
    K230SysctlPowerState *s = K230_SYSCTL_POWER(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_sysctl_power_ops, s,
                          TYPE_K230_SYSCTL_POWER, K230_SYSCTL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_sysctl_power_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_sysctl_power_realize;
    device_class_set_legacy_reset(dc, k230_sysctl_power_reset);
    dc->vmsd = &vmstate_k230_sysctl_power;
    dc->desc = "K230 sysctl power registers";
}

static const TypeInfo k230_sysctl_power_type_info = {
    .name = TYPE_K230_SYSCTL_POWER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230SysctlPowerState),
    .class_init = k230_sysctl_power_class_init,
};

static void k230_sysctl_register_types(void)
{
    type_register_static(&k230_sysctl_boot_type_info);
    type_register_static(&k230_sysctl_power_type_info);
}

type_init(k230_sysctl_register_types)
