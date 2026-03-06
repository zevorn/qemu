/*
 * QTest for RISC-V Debug Module v1.0
 *
 * Copyright (c) 2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define DM_BASE                 0x0

#define ROM_HARTID              0x104
#define ROM_RESUME              0x10c

#define A_DATA0                 0x10
#define A_DMCONTROL             0x40
#define A_DMSTATUS              0x44
#define A_HARTINFO              0x48
#define A_ABSTRACTCS            0x58

#define DMCONTROL_DMACTIVE      (1u << 0)
#define DMCONTROL_HALTREQ       (1u << 31)
#define DMCONTROL_RESUMEREQ     (1u << 30)

#define DMSTATUS_VERSION_MASK   0xf
#define DMSTATUS_AUTHENTICATED  (1u << 7)
#define DMSTATUS_ANYHALTED      (1u << 8)
#define DMSTATUS_ALLHALTED      (1u << 9)
#define DMSTATUS_ANYRUNNING     (1u << 10)
#define DMSTATUS_ALLRUNNING     (1u << 11)
#define DMSTATUS_ANYRESUMEACK   (1u << 16)
#define DMSTATUS_ALLRESUMEACK   (1u << 17)
#define DMSTATUS_ANYHAVERESET   (1u << 18)
#define DMSTATUS_IMPEBREAK      (1u << 22)

#define HARTINFO_DATAADDR_MASK      0xfffu
#define HARTINFO_DATASIZE_SHIFT     12
#define HARTINFO_DATASIZE_MASK      (0xfu << HARTINFO_DATASIZE_SHIFT)
#define HARTINFO_DATAACCESS         (1u << 16)
#define HARTINFO_NSCRATCH_SHIFT     20
#define HARTINFO_NSCRATCH_MASK      (0xfu << HARTINFO_NSCRATCH_SHIFT)

#define ABSTRACTCS_DATACOUNT_MASK       0xf
#define ABSTRACTCS_BUSY                 (1u << 12)
#define ABSTRACTCS_PROGBUFSIZE_SHIFT    24
#define ABSTRACTCS_PROGBUFSIZE_MASK     (0x1fu << ABSTRACTCS_PROGBUFSIZE_SHIFT)

static uint32_t dm_read(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, DM_BASE + reg);
}

static void dm_write(QTestState *qts, uint32_t reg, uint32_t val)
{
    qtest_writel(qts, DM_BASE + reg, val);
}

static void dm_set_active(QTestState *qts)
{
    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE);
}

static void rom_write32(QTestState *qts, uint32_t offset, uint32_t val)
{
    qtest_writel(qts, DM_BASE + offset, val);
}

static void sim_cpu_halt_ack(QTestState *qts, uint32_t hartid)
{
    rom_write32(qts, ROM_HARTID, hartid);
}

static void sim_cpu_resume_ack(QTestState *qts, uint32_t hartid)
{
    rom_write32(qts, ROM_RESUME, hartid);
}

static void test_dmactive_gate(void)
{
    QTestState *qts = qtest_init("-machine virt");

    g_assert_cmpuint(dm_read(qts, A_DMCONTROL), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_DMSTATUS), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_HARTINFO), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_ABSTRACTCS), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_DATA0), ==, 0);

    dm_write(qts, A_DATA0, 0xdeadbeef);
    g_assert_cmpuint(dm_read(qts, A_DATA0), ==, 0);

    dm_set_active(qts);
    g_assert_cmpuint(dm_read(qts, A_DMCONTROL) & DMCONTROL_DMACTIVE, ==,
                     DMCONTROL_DMACTIVE);
    g_assert_cmpuint(dm_read(qts, A_DMSTATUS), !=, 0);

    qtest_quit(qts);
}

static void test_dmstatus(void)
{
    QTestState *qts = qtest_init("-machine virt");
    uint32_t status;

    dm_set_active(qts);
    status = dm_read(qts, A_DMSTATUS);

    g_assert_cmpuint(status & DMSTATUS_VERSION_MASK, ==, 3);
    g_assert_cmpuint(status & DMSTATUS_AUTHENTICATED, ==,
                     DMSTATUS_AUTHENTICATED);
    g_assert_cmpuint(status & DMSTATUS_ANYRUNNING, ==,
                     DMSTATUS_ANYRUNNING);
    g_assert_cmpuint(status & DMSTATUS_ALLRUNNING, ==,
                     DMSTATUS_ALLRUNNING);
    g_assert_cmpuint(status & DMSTATUS_ANYHALTED, ==, 0);
    g_assert_cmpuint(status & DMSTATUS_ANYHAVERESET, ==,
                     DMSTATUS_ANYHAVERESET);
    g_assert_cmpuint(status & DMSTATUS_IMPEBREAK, ==, DMSTATUS_IMPEBREAK);

    qtest_quit(qts);
}

static void test_hartinfo(void)
{
    QTestState *qts = qtest_init("-machine virt");
    uint32_t info;

    dm_set_active(qts);
    info = dm_read(qts, A_HARTINFO);

    g_assert_cmpuint(info & HARTINFO_DATAADDR_MASK, ==, 0x3c0);
    g_assert_cmpuint(info & HARTINFO_DATAACCESS, ==, HARTINFO_DATAACCESS);
    g_assert_cmpuint((info & HARTINFO_DATASIZE_MASK) >>
                     HARTINFO_DATASIZE_SHIFT, ==, 2);
    g_assert_cmpuint((info & HARTINFO_NSCRATCH_MASK) >>
                     HARTINFO_NSCRATCH_SHIFT, ==, 1);

    qtest_quit(qts);
}

static void test_abstractcs_config(void)
{
    QTestState *qts = qtest_init("-machine virt");
    uint32_t acs;

    dm_set_active(qts);
    acs = dm_read(qts, A_ABSTRACTCS);

    g_assert_cmpuint(acs & ABSTRACTCS_DATACOUNT_MASK, ==, 2);
    g_assert_cmpuint((acs & ABSTRACTCS_PROGBUFSIZE_MASK) >>
                     ABSTRACTCS_PROGBUFSIZE_SHIFT, ==, 8);
    g_assert_cmpuint(acs & ABSTRACTCS_BUSY, ==, 0);

    qtest_quit(qts);
}

static void test_halt_resume(void)
{
    QTestState *qts = qtest_init("-machine virt");
    uint32_t status;

    dm_set_active(qts);
    status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_ANYRUNNING, ==,
                     DMSTATUS_ANYRUNNING);
    g_assert_cmpuint(status & DMSTATUS_ANYHALTED, ==, 0);

    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_HALTREQ);
    sim_cpu_halt_ack(qts, 0);

    status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_ANYHALTED, ==,
                     DMSTATUS_ANYHALTED);
    g_assert_cmpuint(status & DMSTATUS_ALLHALTED, ==,
                     DMSTATUS_ALLHALTED);
    g_assert_cmpuint(status & DMSTATUS_ANYRUNNING, ==, 0);
    g_assert_cmpuint(status & DMSTATUS_ALLRUNNING, ==, 0);

    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_RESUMEREQ);
    sim_cpu_resume_ack(qts, 0);

    status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_ANYRESUMEACK, ==,
                     DMSTATUS_ANYRESUMEACK);
    g_assert_cmpuint(status & DMSTATUS_ALLRESUMEACK, ==,
                     DMSTATUS_ALLRESUMEACK);
    g_assert_cmpuint(status & DMSTATUS_ANYRUNNING, ==,
                     DMSTATUS_ANYRUNNING);
    g_assert_cmpuint(status & DMSTATUS_ALLRUNNING, ==,
                     DMSTATUS_ALLRUNNING);
    g_assert_cmpuint(status & DMSTATUS_ANYHALTED, ==, 0);

    qtest_quit(qts);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/riscv-dm/dmactive-gate", test_dmactive_gate);
    qtest_add_func("/riscv-dm/dmstatus", test_dmstatus);
    qtest_add_func("/riscv-dm/hartinfo", test_hartinfo);
    qtest_add_func("/riscv-dm/abstractcs-config", test_abstractcs_config);
    qtest_add_func("/riscv-dm/halt-resume", test_halt_resume);

    return g_test_run();
}
