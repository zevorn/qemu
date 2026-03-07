/*
 * QTest for RISC-V Debug Module v1.0
 *
 * Copyright (c) 2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Virt machine memory map base address for the Debug Module */
#define DM_BASE                 0x0

/*
 * ROM layout offsets (byte offsets within DM ROM).
 * CPU ROM code writes to these addresses to signal state changes to the DM.
 */
#define ROM_HARTID      0x104
#define ROM_GOING       0x108
#define ROM_RESUME      0x10C
#define ROM_EXCP        0x110
#define ROM_DATA        0x3C0
#define ROM_FLAGS       0x400
#define ROM_ENTRY       0x800

/* DMI register byte offsets (word address × 4) */
#define A_DATA0         0x10
#define A_DATA1         0x14
#define A_DMCONTROL     0x40
#define A_DMSTATUS      0x44
#define A_HARTINFO      0x48
#define A_HALTSUM1      0x4C
#define A_HAWINDOWSEL   0x50
#define A_HAWINDOW      0x54
#define A_ABSTRACTCS    0x58
#define A_COMMAND       0x5C
#define A_ABSTRACTAUTO  0x60
#define A_PROGBUF0      0x80
#define A_PROGBUF1      0x84
#define A_AUTHDATA      0xC0
#define A_DMCS2         0xC8
#define A_SBCS          0xE0
#define A_SBADDRESS0    0xE4
#define A_SBADDRESS1    0xE8
#define A_SBADDRESS2    0xEC
#define A_SBDATA0       0xF0
#define A_SBDATA1       0xF4
#define A_SBDATA2       0xF8
#define A_SBDATA3       0xFC
#define A_HALTSUM0      0x100

/* DMCONTROL fields */
#define DMCONTROL_DMACTIVE      (1u << 0)
#define DMCONTROL_NDMRESET      (1u << 1)
#define DMCONTROL_CLRRESETHALTREQ (1u << 2)
#define DMCONTROL_SETRESETHALTREQ (1u << 3)
#define DMCONTROL_HARTRESET     (1u << 29)
#define DMCONTROL_HALTREQ       (1u << 31)
#define DMCONTROL_RESUMEREQ     (1u << 30)
#define DMCONTROL_ACKHAVERESET  (1u << 28)
#define DMCONTROL_HASEL         (1u << 26)
#define DMCONTROL_HARTSELHI_SHIFT   16
#define DMCONTROL_HARTSELLO_SHIFT   6
#define DMCONTROL_HARTSELLO_MASK    (0x3FFu << 6)

/* DMSTATUS fields */
#define DMSTATUS_VERSION_MASK   0xF
#define DMSTATUS_AUTHENTICATED  (1u << 7)
#define DMSTATUS_ANYHALTED      (1u << 8)
#define DMSTATUS_ALLHALTED      (1u << 9)
#define DMSTATUS_ANYRUNNING     (1u << 10)
#define DMSTATUS_ALLRUNNING     (1u << 11)
#define DMSTATUS_ANYUNAVAIL     (1u << 12)
#define DMSTATUS_ALLUNAVAIL     (1u << 13)
#define DMSTATUS_ANYNONEXISTENT (1u << 14)
#define DMSTATUS_ALLNONEXISTENT (1u << 15)
#define DMSTATUS_ANYRESUMEACK   (1u << 16)
#define DMSTATUS_ALLRESUMEACK   (1u << 17)
#define DMSTATUS_ANYHAVERESET   (1u << 18)
#define DMSTATUS_ALLHAVERESET   (1u << 19)
#define DMSTATUS_IMPEBREAK      (1u << 22)
#define DMSTATUS_NDMRESETPENDING (1u << 24)

/* HARTINFO fields */
#define HARTINFO_DATAADDR_MASK      0xFFFu
#define HARTINFO_DATASIZE_SHIFT     12
#define HARTINFO_DATASIZE_MASK      (0xFu << HARTINFO_DATASIZE_SHIFT)
#define HARTINFO_DATAACCESS         (1u << 16)
#define HARTINFO_NSCRATCH_SHIFT     20
#define HARTINFO_NSCRATCH_MASK      (0xFu << HARTINFO_NSCRATCH_SHIFT)

/* ABSTRACTCS fields */
#define ABSTRACTCS_DATACOUNT_MASK       0xF
#define ABSTRACTCS_CMDERR_MASK          (0x7u << 8)
#define ABSTRACTCS_CMDERR_SHIFT         8
#define ABSTRACTCS_BUSY                 (1u << 12)
#define ABSTRACTCS_PROGBUFSIZE_MASK     (0x1Fu << 24)
#define ABSTRACTCS_PROGBUFSIZE_SHIFT    24

/* SBCS fields */
#define SBCS_SBERROR_MASK       (0x7u << 12)
#define SBCS_SBBUSYERROR        (1u << 22)
#define SBCS_SBVERSION_MASK     (0x7u << 29)
#define SBCS_SBVERSION_SHIFT    29
#define SBCS_SBASIZE_SHIFT      5
#define SBCS_SBASIZE_MASK       (0x7Fu << SBCS_SBASIZE_SHIFT)
#define SBCS_SBACCESS32         (1u << 2)

/* Abstract command register number space */
#define REGNO_GPR(n)    (0x1000 + (n))  /* x0..x31 */
#define REGNO_FPR(n)    (0x1020 + (n))  /* f0..f31 */
#define REGNO_CSR(n)    (n)             /* CSR direct */
#define REGNO_DPC       0x7b1
#define REGNO_DCSR      0x7b0

/* DCSR fields */
#define DCSR_STEP           (1u << 2)
#define DCSR_CAUSE_SHIFT    6
#define DCSR_CAUSE_MASK     (0x7u << DCSR_CAUSE_SHIFT)
#define DCSR_CAUSE_EBREAK   1
#define DCSR_CAUSE_TRIGGER  2
#define DCSR_CAUSE_HALTREQ  3
#define DCSR_CAUSE_STEP     4
#define DCSR_CAUSE_RESET    5

/* Trigger CSRs (abstract command register numbers) */
#define REGNO_TSELECT       REGNO_CSR(0x7a0)
#define REGNO_TDATA1        REGNO_CSR(0x7a1)
#define REGNO_TDATA2        REGNO_CSR(0x7a2)

/* mcontrol (type 2) tdata1 field helpers */
#define TDATA1_TYPE2_TYPE       (2u << 28)
#define TDATA1_TYPE2_DMODE      (1u << 27)
#define TDATA1_TYPE2_ACTION_DBG (1u << 12)
#define TDATA1_TYPE2_M          (1u << 6)
#define TDATA1_TYPE2_S          (1u << 4)
#define TDATA1_TYPE2_U          (1u << 3)
#define TDATA1_TYPE2_EXEC       (1u << 2)
#define TDATA1_TYPE2_STORE      (1u << 1)
#define TDATA1_TYPE2_LOAD       (1u << 0)

/* itrigger (type 3) tdata1 field helpers */
#define TDATA1_ITRIGGER_TYPE        (3u << 28)
#define TDATA1_ITRIGGER_ACTION_DBG  1u
#define TDATA1_ITRIGGER_U           (1u << 6)
#define TDATA1_ITRIGGER_S           (1u << 7)
#define TDATA1_ITRIGGER_M           (1u << 9)
#define TDATA1_ITRIGGER_COUNT(n)    (((n) & 0x3fffu) << 10)
#define TDATA1_ITRIGGER_COUNT_MASK  TDATA1_ITRIGGER_COUNT(0x3fff)

/* TCG test timing (wall-clock microseconds, CPU runs via MTTCG thread) */
#define TCG_POLL_STEP_US    10000       /* 10ms per poll step */
#define TCG_POLL_TIMEOUT_US 5000000     /* 5s max */
#define TCG_BOOT_US         200000      /* 200ms boot */

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

/* Write to DM ROM area (simulates CPU writing from ROM code) */
static void rom_write32(QTestState *qts, uint32_t offset, uint32_t val)
{
    qtest_writel(qts, DM_BASE + offset, val);
}

/*
 * Simulate the CPU executing the DM ROM halt entry code.
 * In real hardware: CPU enters debug mode → jumps to ROM entry (0x800) →
 * ROM code writes mhartid to HARTID offset → DM recognizes hart as halted.
 */
static void sim_cpu_halt_ack(QTestState *qts, uint32_t hartid)
{
    rom_write32(qts, ROM_HARTID, hartid);
}

/*
 * Simulate the CPU executing the DM ROM resume handler.
 * In real hardware: ROM detects RESUME flag → writes hartid to RESUME offset
 * → DM recognizes hart as resumed → CPU executes dret.
 */
static void sim_cpu_resume_ack(QTestState *qts, uint32_t hartid)
{
    rom_write32(qts, ROM_RESUME, hartid);
}


/*
 * Test: dmactive gate.
 * When dmactive=0 (after reset), all non-DMCONTROL reads should return 0.
 */
static void test_dmactive_gate(void)
{
    QTestState *qts = qtest_init("-machine virt");

    /* DM starts inactive after reset */
    g_assert_cmpuint(dm_read(qts, A_DMCONTROL), ==, 0);

    /* All other registers should return 0 when dmactive=0 */
    g_assert_cmpuint(dm_read(qts, A_DMSTATUS), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_ABSTRACTCS), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_HARTINFO), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_DATA0), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_PROGBUF0), ==, 0);

    /* Writes to non-DMCONTROL registers should be ignored */
    dm_write(qts, A_DATA0, 0xDEADBEEF);
    /* Still inactive, so read should still return 0 */
    g_assert_cmpuint(dm_read(qts, A_DATA0), ==, 0);

    /* Activate the DM */
    dm_set_active(qts);
    g_assert_cmpuint(dm_read(qts, A_DMCONTROL) & DMCONTROL_DMACTIVE, ==,
                     DMCONTROL_DMACTIVE);

    /* Now DMSTATUS should be non-zero (version, authenticated, etc.) */
    g_assert_cmpuint(dm_read(qts, A_DMSTATUS), !=, 0);

    qtest_quit(qts);
}

/*
 * Test: DMSTATUS register fields after activation.
 */
static void test_dmstatus(void)
{
    QTestState *qts = qtest_init("-machine virt");
    dm_set_active(qts);

    uint32_t status = dm_read(qts, A_DMSTATUS);

    /* Version should be 3 (v1.0 spec) */
    g_assert_cmpuint(status & DMSTATUS_VERSION_MASK, ==, 3);

    /* Should be authenticated */
    g_assert_cmpuint(status & DMSTATUS_AUTHENTICATED, ==,
                     DMSTATUS_AUTHENTICATED);

    /* impebreak should be set (default property) */
    g_assert_cmpuint(status & DMSTATUS_IMPEBREAK, ==, DMSTATUS_IMPEBREAK);

    /* Hart 0 should be running (not halted) */
    g_assert_cmpuint(status & DMSTATUS_ANYRUNNING, ==, DMSTATUS_ANYRUNNING);
    g_assert_cmpuint(status & DMSTATUS_ALLRUNNING, ==, DMSTATUS_ALLRUNNING);
    g_assert_cmpuint(status & DMSTATUS_ANYHALTED, ==, 0);
    g_assert_cmpuint(status & DMSTATUS_ALLHALTED, ==, 0);

    /* Should have havereset after initial reset */
    g_assert_cmpuint(status & DMSTATUS_ANYHAVERESET, ==,
                     DMSTATUS_ANYHAVERESET);
    g_assert_cmpuint(status & DMSTATUS_ALLHAVERESET, ==,
                     DMSTATUS_ALLHAVERESET);

    /* Hart 0 should not be nonexistent or unavailable */
    g_assert_cmpuint(status & DMSTATUS_ANYNONEXISTENT, ==, 0);
    g_assert_cmpuint(status & DMSTATUS_ANYUNAVAIL, ==, 0);

    qtest_quit(qts);
}

/*
 * Test: HARTINFO fields describe memory-mapped DATA registers.
 */
static void test_hartinfo(void)
{
    QTestState *qts = qtest_init("-machine virt");
    dm_set_active(qts);

    uint32_t info = dm_read(qts, A_HARTINFO);
    uint32_t datasize =
        (info & HARTINFO_DATASIZE_MASK) >> HARTINFO_DATASIZE_SHIFT;
    uint32_t nscratch =
        (info & HARTINFO_NSCRATCH_MASK) >> HARTINFO_NSCRATCH_SHIFT;

    g_assert_cmpuint(info & HARTINFO_DATAACCESS, ==, HARTINFO_DATAACCESS);
    g_assert_cmpuint(info & HARTINFO_DATAADDR_MASK, ==, ROM_DATA);
    g_assert_cmpuint(datasize, ==, 2);
    g_assert_cmpuint(nscratch, ==, 1);

    qtest_quit(qts);
}

/*
 * Test: ABSTRACTCS config fields (datacount, progbufsize).
 */
static void test_abstractcs_config(void)
{
    QTestState *qts = qtest_init("-machine virt");
    dm_set_active(qts);

    uint32_t acs = dm_read(qts, A_ABSTRACTCS);

    /* Default datacount = 2 */
    g_assert_cmpuint(acs & ABSTRACTCS_DATACOUNT_MASK, ==, 2);

    /* Default progbufsize = 8 */
    uint32_t progbufsize =
        (acs & ABSTRACTCS_PROGBUFSIZE_MASK) >> ABSTRACTCS_PROGBUFSIZE_SHIFT;
    g_assert_cmpuint(progbufsize, ==, 8);

    /* BUSY should be 0 */
    g_assert_cmpuint(acs & ABSTRACTCS_BUSY, ==, 0);

    /* CMDERR should be 0 */
    g_assert_cmpuint(acs & ABSTRACTCS_CMDERR_MASK, ==, 0);

    qtest_quit(qts);
}

/*
 * Test: DATA register read/write.
 */
static void test_data_rw(void)
{
    QTestState *qts = qtest_init("-machine virt");
    dm_set_active(qts);

    /* Write to DATA0 and DATA1 */
    dm_write(qts, A_DATA0, 0xCAFEBABE);
    dm_write(qts, A_DATA1, 0x12345678);

    g_assert_cmpuint(dm_read(qts, A_DATA0), ==, 0xCAFEBABE);
    g_assert_cmpuint(dm_read(qts, A_DATA1), ==, 0x12345678);

    /* Overwrite */
    dm_write(qts, A_DATA0, 0x0);
    g_assert_cmpuint(dm_read(qts, A_DATA0), ==, 0x0);

    qtest_quit(qts);
}

/*
 * Test: PROGBUF register read/write.
 */
static void test_progbuf_rw(void)
{
    QTestState *qts = qtest_init("-machine virt");
    dm_set_active(qts);

    /* Write distinct values to progbuf0..3 */
    for (int i = 0; i < 4; i++) {
        dm_write(qts, A_PROGBUF0 + i * 4, 0xA0000000 | i);
    }

    for (int i = 0; i < 4; i++) {
        g_assert_cmpuint(dm_read(qts, A_PROGBUF0 + i * 4), ==,
                         0xA0000000 | (uint32_t)i);
    }

    qtest_quit(qts);
}

/*
 * Test: Read-only register protection (DMSTATUS, HARTINFO, HALTSUM).
 */
static void test_ro_registers(void)
{
    QTestState *qts = qtest_init("-machine virt");
    dm_set_active(qts);

    uint32_t orig_status = dm_read(qts, A_DMSTATUS);

    /* Try to write DMSTATUS - should be ignored (RO) */
    dm_write(qts, A_DMSTATUS, 0xFFFFFFFF);
    g_assert_cmpuint(dm_read(qts, A_DMSTATUS), ==, orig_status);

    /* Try to write HARTINFO - should be ignored (RO) */
    uint32_t orig_hartinfo = dm_read(qts, A_HARTINFO);
    dm_write(qts, A_HARTINFO, 0xFFFFFFFF);
    g_assert_cmpuint(dm_read(qts, A_HARTINFO), ==, orig_hartinfo);

    /* Try to write HALTSUM0 - should be ignored (RO) */
    uint32_t orig_haltsum0 = dm_read(qts, A_HALTSUM0);
    dm_write(qts, A_HALTSUM0, 0xFFFFFFFF);
    g_assert_cmpuint(dm_read(qts, A_HALTSUM0), ==, orig_haltsum0);

    qtest_quit(qts);
}

/*
 * Test: W1C (Write-1-to-Clear) behavior on ABSTRACTCS.CMDERR.
 */
static void test_abstractcs_w1c(void)
{
    QTestState *qts = qtest_init("-machine virt");
    dm_set_active(qts);

    /*
     * CMDERR is 0 initially. To test W1C, we need to trigger an error.
     * Issue an invalid command (cmdtype=0xFF) to a running hart.
     * This should set CMDERR to HALTRESUME (4) since hart is not halted.
     */
    dm_write(qts, A_COMMAND, 0xFF000000);

    uint32_t acs = dm_read(qts, A_ABSTRACTCS);
    uint32_t cmderr = (acs & ABSTRACTCS_CMDERR_MASK) >> ABSTRACTCS_CMDERR_SHIFT;
    g_assert_cmpuint(cmderr, !=, 0);

    /* Clear CMDERR by writing 1s to the CMDERR field */
    dm_write(qts, A_ABSTRACTCS, ABSTRACTCS_CMDERR_MASK);

    acs = dm_read(qts, A_ABSTRACTCS);
    cmderr = (acs & ABSTRACTCS_CMDERR_MASK) >> ABSTRACTCS_CMDERR_SHIFT;
    g_assert_cmpuint(cmderr, ==, 0);

    qtest_quit(qts);
}

/*
 * Test: Hart selection via DMCONTROL.hartsel.
 * Select a nonexistent hart and verify DMSTATUS reports it.
 */
static void test_hart_selection(void)
{
    QTestState *qts = qtest_init("-machine virt");
    dm_set_active(qts);

    /* Default hart 0 is selected and should be running */
    uint32_t status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_ANYRUNNING, ==, DMSTATUS_ANYRUNNING);
    g_assert_cmpuint(status & DMSTATUS_ANYNONEXISTENT, ==, 0);

    /* Select nonexistent hart 99 */
    uint32_t ctl = DMCONTROL_DMACTIVE |
                   (99u << DMCONTROL_HARTSELLO_SHIFT);
    dm_write(qts, A_DMCONTROL, ctl);

    status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_ANYNONEXISTENT, ==,
                     DMSTATUS_ANYNONEXISTENT);
    g_assert_cmpuint(status & DMSTATUS_ALLNONEXISTENT, ==,
                     DMSTATUS_ALLNONEXISTENT);
    g_assert_cmpuint(status & DMSTATUS_ANYRUNNING, ==, 0);

    /* Switch back to hart 0 */
    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE);
    status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_ANYRUNNING, ==, DMSTATUS_ANYRUNNING);
    g_assert_cmpuint(status & DMSTATUS_ANYNONEXISTENT, ==, 0);

    qtest_quit(qts);
}

/*
 * Test: DMCONTROL WARZ (Write-Any-Read-Zero) fields.
 * HALTREQ, RESUMEREQ, ACKHAVERESET etc. should not read back.
 */
static void test_dmcontrol_warz(void)
{
    QTestState *qts = qtest_init("-machine virt");
    dm_set_active(qts);

    /* Write HALTREQ | DMACTIVE */
    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_HALTREQ);

    /* HALTREQ should not read back */
    uint32_t ctl = dm_read(qts, A_DMCONTROL);
    g_assert_cmpuint(ctl & DMCONTROL_HALTREQ, ==, 0);

    /* DMACTIVE should still be set */
    g_assert_cmpuint(ctl & DMCONTROL_DMACTIVE, ==, DMCONTROL_DMACTIVE);

    /* Write RESUMEREQ | DMACTIVE */
    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_RESUMEREQ);
    ctl = dm_read(qts, A_DMCONTROL);
    g_assert_cmpuint(ctl & DMCONTROL_RESUMEREQ, ==, 0);

    /* Write ACKHAVERESET | DMACTIVE */
    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_ACKHAVERESET);
    ctl = dm_read(qts, A_DMCONTROL);
    g_assert_cmpuint(ctl & DMCONTROL_ACKHAVERESET, ==, 0);

    qtest_quit(qts);
}

/*
 * Test: HARTRESET is implemented and reads back while asserted.
 */
static void test_hartreset(void)
{
    QTestState *qts = qtest_init("-machine virt");
    uint32_t ctl, status;

    dm_set_active(qts);

    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_HARTRESET);
    ctl = dm_read(qts, A_DMCONTROL);
    g_assert_cmpuint(ctl & DMCONTROL_HARTRESET, ==, DMCONTROL_HARTRESET);

    status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_ANYHAVERESET, ==,
                     DMSTATUS_ANYHAVERESET);

    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE);
    ctl = dm_read(qts, A_DMCONTROL);
    g_assert_cmpuint(ctl & DMCONTROL_HARTRESET, ==, 0);

    qtest_quit(qts);
}

/*
 * Test: NDMRESET gates non-DMCONTROL accesses and exposes only pending state.
 */
static void test_ndmreset_gate(void)
{
    QTestState *qts = qtest_init("-machine virt");
    uint32_t status;

    dm_set_active(qts);
    dm_write(qts, A_DATA0, 0xDEADBEEF);
    g_assert_cmpuint(dm_read(qts, A_DATA0), ==, 0xDEADBEEF);

    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_NDMRESET);

    status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status, ==, DMSTATUS_NDMRESETPENDING);

    dm_write(qts, A_DATA0, 0x11223344);
    g_assert_cmpuint(dm_read(qts, A_DATA0), ==, 0);

    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE);
    status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_NDMRESETPENDING, ==, 0);
    g_assert_cmpuint(status & DMSTATUS_ANYHAVERESET, ==,
                     DMSTATUS_ANYHAVERESET);

    qtest_quit(qts);
}

/*
 * Test: Optional registers that are not implemented read as 0 and ignore
 * writes.
 */
static void test_optional_regs_absent(void)
{
    QTestState *qts = qtest_init("-machine virt");

    dm_set_active(qts);

    dm_write(qts, A_AUTHDATA, 0xFFFFFFFF);
    g_assert_cmpuint(dm_read(qts, A_AUTHDATA), ==, 0);

    dm_write(qts, A_DMCS2, 0xFFFFFFFF);
    g_assert_cmpuint(dm_read(qts, A_DMCS2), ==, 0);

    dm_write(qts, A_SBADDRESS0, 0xFFFFFFFF);
    dm_write(qts, A_SBADDRESS1, 0xFFFFFFFF);
    dm_write(qts, A_SBADDRESS2, 0xFFFFFFFF);
    dm_write(qts, A_SBDATA0, 0xAAAAAAAA);
    dm_write(qts, A_SBDATA1, 0xBBBBBBBB);
    dm_write(qts, A_SBDATA2, 0xCCCCCCCC);
    dm_write(qts, A_SBDATA3, 0xDDDDDDDD);

    g_assert_cmpuint(dm_read(qts, A_SBADDRESS0), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_SBADDRESS1), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_SBADDRESS2), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_SBDATA0), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_SBDATA1), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_SBDATA2), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_SBDATA3), ==, 0);

    qtest_quit(qts);
}

/*
 * Test: Deactivation resets all state.
 */
static void test_deactivate(void)
{
    QTestState *qts = qtest_init("-machine virt");

    /* Activate and write some data */
    dm_set_active(qts);
    dm_write(qts, A_DATA0, 0xDEADBEEF);
    g_assert_cmpuint(dm_read(qts, A_DATA0), ==, 0xDEADBEEF);

    /* Deactivate: clear dmactive */
    dm_write(qts, A_DMCONTROL, 0);

    /* All registers should return 0 again */
    g_assert_cmpuint(dm_read(qts, A_DMSTATUS), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_DATA0), ==, 0);
    g_assert_cmpuint(dm_read(qts, A_ABSTRACTCS), ==, 0);

    /* Re-activate: verify state was reset */
    dm_set_active(qts);
    g_assert_cmpuint(dm_read(qts, A_DATA0), ==, 0);

    uint32_t status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_VERSION_MASK, ==, 3);

    qtest_quit(qts);
}

/*
 * Test: ACKHAVERESET clears havereset in DMSTATUS.
 */
static void test_ackhavereset(void)
{
    QTestState *qts = qtest_init("-machine virt");
    dm_set_active(qts);

    /* After reset, havereset should be set */
    uint32_t status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_ANYHAVERESET, ==,
                     DMSTATUS_ANYHAVERESET);

    /* Acknowledge havereset */
    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_ACKHAVERESET);

    status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_ANYHAVERESET, ==, 0);
    g_assert_cmpuint(status & DMSTATUS_ALLHAVERESET, ==, 0);

    qtest_quit(qts);
}

/*
 * Test: Halt and resume cycle via ROM simulation.
 *
 * Simulates the full halt/resume flow:
 * 1. Debugger writes HALTREQ → DM signals halt IRQ
 * 2. CPU enters debug mode → ROM entry code writes mhartid to HARTID offset
 * 3. DM recognizes hart as halted (anyhalted/allhalted set)
 * 4. Debugger writes RESUMEREQ → DM sets FLAGS to RESUME
 * 5. CPU ROM resume handler writes hartid to RESUME offset → dret
 * 6. DM recognizes hart as resumed (resumeack set, running)
 */
static void test_halt_resume(void)
{
    QTestState *qts = qtest_init("-machine virt");
    dm_set_active(qts);

    uint32_t status;

    status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_ANYRUNNING, ==, DMSTATUS_ANYRUNNING);
    g_assert_cmpuint(status & DMSTATUS_ANYHALTED, ==, 0);

    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_HALTREQ);
    sim_cpu_halt_ack(qts, 0);

    status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_ANYHALTED, ==, DMSTATUS_ANYHALTED);
    g_assert_cmpuint(status & DMSTATUS_ALLHALTED, ==, DMSTATUS_ALLHALTED);
    g_assert_cmpuint(status & DMSTATUS_ANYRUNNING, ==, 0);
    g_assert_cmpuint(status & DMSTATUS_ALLRUNNING, ==, 0);

    dm_write(qts, A_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_RESUMEREQ);
    sim_cpu_resume_ack(qts, 0);

    status = dm_read(qts, A_DMSTATUS);
    g_assert_cmpuint(status & DMSTATUS_ANYRESUMEACK, ==,
                     DMSTATUS_ANYRESUMEACK);
    g_assert_cmpuint(status & DMSTATUS_ALLRESUMEACK, ==,
                     DMSTATUS_ALLRESUMEACK);
    g_assert_cmpuint(status & DMSTATUS_ANYRUNNING, ==, DMSTATUS_ANYRUNNING);
    g_assert_cmpuint(status & DMSTATUS_ALLRUNNING, ==, DMSTATUS_ALLRUNNING);
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
    qtest_add_func("/riscv-dm/data-rw", test_data_rw);
    qtest_add_func("/riscv-dm/progbuf-rw", test_progbuf_rw);
    qtest_add_func("/riscv-dm/ro-registers", test_ro_registers);
    qtest_add_func("/riscv-dm/abstractcs-w1c", test_abstractcs_w1c);
    qtest_add_func("/riscv-dm/hart-selection", test_hart_selection);
    qtest_add_func("/riscv-dm/dmcontrol-warz", test_dmcontrol_warz);
    qtest_add_func("/riscv-dm/hartreset", test_hartreset);
    qtest_add_func("/riscv-dm/ndmreset-gate", test_ndmreset_gate);
    qtest_add_func("/riscv-dm/optional-regs-absent", test_optional_regs_absent);
    qtest_add_func("/riscv-dm/deactivate", test_deactivate);
    qtest_add_func("/riscv-dm/ackhavereset", test_ackhavereset);
    qtest_add_func("/riscv-dm/halt-resume", test_halt_resume);

    return g_test_run();
}
