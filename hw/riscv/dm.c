/*
 * RISC-V Debug Module v1.0
 *
 * Copyright (c) 2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * Based on the RISC-V Debug Specification v1.0 (ratified 2025-02-21)
 * Uses the QEMU register.h framework for declarative register definitions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/riscv/dm.h"
#include "exec/cpu-common.h"
#include "migration/vmstate.h"
#include "exec/translation-block.h"
#include "system/tcg.h"
#include "target/riscv/cpu.h"
#include "trace.h"

/* Addresses = DMI word address × 4 (byte offsets). */

REG32(DATA0,  0x10)
REG32(DATA1,  0x14)
REG32(DATA2,  0x18)
REG32(DATA3,  0x1C)
REG32(DATA4,  0x20)
REG32(DATA5,  0x24)
REG32(DATA6,  0x28)
REG32(DATA7,  0x2C)
REG32(DATA8,  0x30)
REG32(DATA9,  0x34)
REG32(DATA10, 0x38)
REG32(DATA11, 0x3C)

REG32(DMCONTROL, 0x40)
    FIELD(DMCONTROL, DMACTIVE,        0,  1)
    FIELD(DMCONTROL, NDMRESET,        1,  1)
    FIELD(DMCONTROL, CLRRESETHALTREQ, 2,  1)
    FIELD(DMCONTROL, SETRESETHALTREQ, 3,  1)
    FIELD(DMCONTROL, CLRKEEPALIVE,    4,  1)
    FIELD(DMCONTROL, SETKEEPALIVE,    5,  1)
    FIELD(DMCONTROL, HARTSELLO,       6, 10)
    FIELD(DMCONTROL, HARTSELHI,      16, 10)
    FIELD(DMCONTROL, HASEL,          26,  1)
    FIELD(DMCONTROL, ACKUNAVAIL,     27,  1)
    FIELD(DMCONTROL, ACKHAVERESET,   28,  1)
    FIELD(DMCONTROL, HARTRESET,      29,  1)
    FIELD(DMCONTROL, RESUMEREQ,      30,  1)
    FIELD(DMCONTROL, HALTREQ,        31,  1)

REG32(DMSTATUS, 0x44)
    FIELD(DMSTATUS, VERSION,          0, 4)
    FIELD(DMSTATUS, CONFSTRPTRVALID,  4, 1)
    FIELD(DMSTATUS, HASRESETHALTREQ,  5, 1)
    FIELD(DMSTATUS, AUTHBUSY,         6, 1)
    FIELD(DMSTATUS, AUTHENTICATED,    7, 1)
    FIELD(DMSTATUS, ANYHALTED,        8, 1)
    FIELD(DMSTATUS, ALLHALTED,        9, 1)
    FIELD(DMSTATUS, ANYRUNNING,      10, 1)
    FIELD(DMSTATUS, ALLRUNNING,      11, 1)
    FIELD(DMSTATUS, ANYUNAVAIL,      12, 1)
    FIELD(DMSTATUS, ALLUNAVAIL,      13, 1)
    FIELD(DMSTATUS, ANYNONEXISTENT,  14, 1)
    FIELD(DMSTATUS, ALLNONEXISTENT,  15, 1)
    FIELD(DMSTATUS, ANYRESUMEACK,    16, 1)
    FIELD(DMSTATUS, ALLRESUMEACK,    17, 1)
    FIELD(DMSTATUS, ANYHAVERESET,    18, 1)
    FIELD(DMSTATUS, ALLHAVERESET,    19, 1)
    FIELD(DMSTATUS, IMPEBREAK,       22, 1)
    FIELD(DMSTATUS, STICKYUNAVAIL,   23, 1)
    FIELD(DMSTATUS, NDMRESETPENDING, 24, 1)

REG32(HARTINFO, 0x48)
    FIELD(HARTINFO, DATAADDR,    0, 12)
    FIELD(HARTINFO, DATASIZE,   12,  4)
    FIELD(HARTINFO, DATAACCESS, 16,  1)
    FIELD(HARTINFO, NSCRATCH,   20,  4)

REG32(HALTSUM1, 0x4C)

REG32(HAWINDOWSEL, 0x50)
    FIELD(HAWINDOWSEL, HAWINDOWSEL, 0, 15)

REG32(HAWINDOW, 0x54)

REG32(ABSTRACTCS, 0x58)
    FIELD(ABSTRACTCS, DATACOUNT,   0,  4)
    FIELD(ABSTRACTCS, CMDERR,      8,  3)
    FIELD(ABSTRACTCS, BUSY,       12,  1)
    FIELD(ABSTRACTCS, PROGBUFSIZE, 24, 5)

REG32(COMMAND, 0x5C)
    FIELD(COMMAND, REGNO,            0, 16)
    FIELD(COMMAND, WRITE,           16,  1)
    FIELD(COMMAND, TRANSFER,        17,  1)
    FIELD(COMMAND, POSTEXEC,        18,  1)
    FIELD(COMMAND, AARPOSTINCREMENT, 19, 1)
    FIELD(COMMAND, AARSIZE,         20,  3)
    FIELD(COMMAND, CMDTYPE,         24,  8)

REG32(ABSTRACTAUTO, 0x60)
    FIELD(ABSTRACTAUTO, AUTOEXECDATA,    0, 12)
    FIELD(ABSTRACTAUTO, AUTOEXECPROGBUF, 16, 16)

REG32(CONFSTRPTR0, 0x64)
REG32(CONFSTRPTR1, 0x68)
REG32(CONFSTRPTR2, 0x6C)
REG32(CONFSTRPTR3, 0x70)

REG32(NEXTDM, 0x74)

REG32(PROGBUF0,  0x80)
REG32(PROGBUF1,  0x84)
REG32(PROGBUF2,  0x88)
REG32(PROGBUF3,  0x8C)
REG32(PROGBUF4,  0x90)
REG32(PROGBUF5,  0x94)
REG32(PROGBUF6,  0x98)
REG32(PROGBUF7,  0x9C)
REG32(PROGBUF8,  0xA0)
REG32(PROGBUF9,  0xA4)
REG32(PROGBUF10, 0xA8)
REG32(PROGBUF11, 0xAC)
REG32(PROGBUF12, 0xB0)
REG32(PROGBUF13, 0xB4)
REG32(PROGBUF14, 0xB8)
REG32(PROGBUF15, 0xBC)

REG32(AUTHDATA, 0xC0)

REG32(DMCS2, 0xC8)
    FIELD(DMCS2, HGSELECT,    0, 1)
    FIELD(DMCS2, HGWRITE,     1, 1)
    FIELD(DMCS2, GROUP,        2, 5)
    FIELD(DMCS2, DMEXTTRIGGER, 7, 4)
    FIELD(DMCS2, GROUPTYPE,   11, 1)

REG32(HALTSUM2, 0xD0)
REG32(HALTSUM3, 0xD4)

REG32(SBADDRESS3, 0xDC)

REG32(SBCS, 0xE0)
    FIELD(SBCS, SBACCESS8,     0, 1)
    FIELD(SBCS, SBACCESS16,    1, 1)
    FIELD(SBCS, SBACCESS32,    2, 1)
    FIELD(SBCS, SBACCESS64,    3, 1)
    FIELD(SBCS, SBACCESS128,   4, 1)
    FIELD(SBCS, SBASIZE,       5, 7)
    FIELD(SBCS, SBERROR,      12, 3)
    FIELD(SBCS, SBREADONDATA, 15, 1)
    FIELD(SBCS, SBAUTOINCREMENT, 16, 1)
    FIELD(SBCS, SBACCESS,     17, 3)
    FIELD(SBCS, SBREADONADDR, 20, 1)
    FIELD(SBCS, SBBUSY,       21, 1)
    FIELD(SBCS, SBBUSYERROR,  22, 1)
    FIELD(SBCS, SBVERSION,    29, 3)

REG32(SBADDRESS0, 0xE4)
REG32(SBADDRESS1, 0xE8)
REG32(SBADDRESS2, 0xEC)

REG32(SBDATA0, 0xF0)
REG32(SBDATA1, 0xF4)
REG32(SBDATA2, 0xF8)
REG32(SBDATA3, 0xFC)

REG32(HALTSUM0, 0x100)


/* Minimal instruction builders used by abstract commands and ROM mailboxes. */
#define DM_I(opcode, funct3, rd, rs1, imm) \
    (((((uint32_t)(imm)) & 0xfffu) << 20) | \
     (((uint32_t)(rs1)) << 15) | (((uint32_t)(funct3)) << 12) | \
     (((uint32_t)(rd)) << 7) | ((uint32_t)(opcode)))

#define DM_S(opcode, funct3, rs1, rs2, imm) \
    (((((((uint32_t)(imm)) >> 5) & 0x7fu) << 25) | \
      (((uint32_t)(rs2)) << 20) | (((uint32_t)(rs1)) << 15) | \
      (((uint32_t)(funct3)) << 12) | ((((uint32_t)(imm)) & 0x1fu) << 7) | \
      ((uint32_t)(opcode))))

#define DM_LOAD(rd, rs1, offset, size) \
    DM_I(0x03u, size, rd, rs1, offset)

#define DM_STORE(rs2, rs1, offset, size) \
    DM_S(0x23u, size, rs1, rs2, offset)

#define DM_FP_LOAD(rd, rs1, offset, size) \
    DM_I(0x07u, size, rd, rs1, offset)

#define DM_FP_STORE(rs2, rs1, offset, size) \
    DM_S(0x27u, size, rs1, rs2, offset)

#define DM_LBU(rd, rs1, offset) \
    DM_LOAD(rd, rs1, offset, 4u)

#define DM_DATA_LOAD(rd, size) \
    DM_LOAD(rd, 0u, RISCV_DM_ROM_DATA, size)

#define DM_DATA_STORE(rs2, size) \
    DM_STORE(rs2, 0u, RISCV_DM_ROM_DATA, size)

#define DM_DATA_FP_LOAD(rd, size) \
    DM_FP_LOAD(rd, 0u, RISCV_DM_ROM_DATA, size)

#define DM_DATA_FP_STORE(rs2, size) \
    DM_FP_STORE(rs2, 0u, RISCV_DM_ROM_DATA, size)

/* csrr rd=s0, csr=regno */
#define DM_CSRR(regno) \
    (0x2473u | ((uint32_t)(regno) << 20))

/* csrw csr=regno, rs1=s0 */
#define DM_CSRW(regno) \
    (0x41073u | ((uint32_t)(regno) << 20))

/* jal x0, imm (imm is in units of 2 bytes, encoded in J-format) */
#define DM_JAL(imm) \
    (0x6fu | ((uint32_t)(imm) << 21))

#define DM_NOP    0x13u
#define DM_EBREAK 0x100073u


typedef struct DMHartSelection {
    int all[RISCV_DM_HAWINDOW_SIZE + 1];
    int all_count;
    int harts[RISCV_DM_HAWINDOW_SIZE + 1];
    int valid_count;
} DMHartSelection;

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

static inline uint8_t dm_rom_read8(RISCVDMState *s, uint32_t offset)
{
    return s->rom_ptr[offset];
}

static void dm_sync_data_to_rom(RISCVDMState *s, int data_index)
{
    uint32_t val = s->regs[R_DATA0 + data_index];
    dm_rom_write32(s, RISCV_DM_ROM_DATA + data_index * 4, val);
}

static void dm_sync_progbuf_to_rom(RISCVDMState *s, int progbuf_index)
{
    uint32_t val = s->regs[R_PROGBUF0 + progbuf_index];
    dm_rom_write32(s, RISCV_DM_ROM_PROGBUF + progbuf_index * 4, val);
}

static void dm_flush_cmd_space(RISCVDMState *s)
{
    for (int i = 0; i < 8; i++) {
        dm_rom_write32(s, RISCV_DM_ROM_CMD + i * 4, DM_NOP);
    }
    /* Restore s0 from dscratch0 */
    dm_rom_write32(s, RISCV_DM_ROM_CMD + 8 * 4, DM_CSRR(0x7b2));
    /* ebreak */
    dm_rom_write32(s, RISCV_DM_ROM_CMD + 9 * 4, DM_EBREAK);
    /* Default whereto: jump to CMD space from the ROM dispatcher. */
    dm_rom_write32(s, RISCV_DM_ROM_WHERETO, DM_JAL(0x1C));
}

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

    memset(sel, 0, sizeof(*sel));
    dm_selection_add(s, sel, hartsel);

    if (!ARRAY_FIELD_EX32(s->regs, DMCONTROL, HASEL)) {
        return;
    }

    uint32_t wsel = ARRAY_FIELD_EX32(s->regs, HAWINDOWSEL, HAWINDOWSEL);
    if (wsel >= RISCV_DM_MAX_HARTS / RISCV_DM_HAWINDOW_SIZE) {
        return;
    }
    uint32_t window = s->hawindow[wsel];
    uint32_t base = wsel * RISCV_DM_HAWINDOW_SIZE;
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

static bool dm_abstract_cmd_completed(RISCVDMState *s, uint32_t hartsel)
{
    uint32_t selected = dm_get_hartsel(s);
    uint8_t flags;

    if (!ARRAY_FIELD_EX32(s->regs, ABSTRACTCS, BUSY) || selected != hartsel) {
        return false;
    }

    /*
     * The hart only completes an execution-based abstract command after it has
     * consumed GO and returned to the park loop.
     */
    flags = dm_rom_read8(s, RISCV_DM_ROM_FLAGS + hartsel);
    return !(flags & RISCV_DM_FLAG_GOING);
}

static void dm_invalidate_dynamic_code(RISCVDMState *s)
{
    if (tcg_enabled()) {
        ram_addr_t base = memory_region_get_ram_addr(&s->rom_mr);

        tb_invalidate_phys_range(NULL, base + RISCV_DM_ROM_WHERETO,
                                 base + RISCV_DM_ROM_DATA - 1);
    }
}

static bool dm_data_reg_present(RISCVDMState *s, hwaddr addr)
{
    unsigned int index = (addr - A_DATA0) / 4;

    return index < s->num_abstract_data;
}

static bool dm_progbuf_reg_present(RISCVDMState *s, hwaddr addr)
{
    unsigned int index = (addr - A_PROGBUF0) / 4;

    return index < s->progbuf_size;
}

static bool dm_sba_addr_reg_present(RISCVDMState *s, hwaddr addr)
{
    switch (addr) {
    case A_SBADDRESS0:
        return s->sba_addr_width > 0;
    case A_SBADDRESS1:
        return s->sba_addr_width > 32;
    case A_SBADDRESS2:
        return s->sba_addr_width > 64;
    case A_SBADDRESS3:
        return s->sba_addr_width > 96;
    default:
        return false;
    }
}

static bool dm_sba_data_reg_present(RISCVDMState *s, hwaddr addr)
{
    bool sbdata0 = ARRAY_FIELD_EX32(s->regs, SBCS, SBACCESS8) ||
                   ARRAY_FIELD_EX32(s->regs, SBCS, SBACCESS16) ||
                   ARRAY_FIELD_EX32(s->regs, SBCS, SBACCESS32) ||
                   ARRAY_FIELD_EX32(s->regs, SBCS, SBACCESS64) ||
                   ARRAY_FIELD_EX32(s->regs, SBCS, SBACCESS128);
    bool sbdata1 = ARRAY_FIELD_EX32(s->regs, SBCS, SBACCESS64) ||
                   ARRAY_FIELD_EX32(s->regs, SBCS, SBACCESS128);
    bool sbdata23 = ARRAY_FIELD_EX32(s->regs, SBCS, SBACCESS128);

    switch (addr) {
    case A_SBDATA0:
        return sbdata0;
    case A_SBDATA1:
        return sbdata1;
    case A_SBDATA2:
    case A_SBDATA3:
        return sbdata23;
    default:
        return false;
    }
}

static bool dm_reg_present(RISCVDMState *s, hwaddr addr)
{
    if (addr >= A_DATA0 && addr <= A_DATA11) {
        return dm_data_reg_present(s, addr);
    }

    if (addr >= A_PROGBUF0 && addr <= A_PROGBUF15) {
        return dm_progbuf_reg_present(s, addr);
    }

    switch (addr) {
    case A_AUTHDATA:
    case A_DMCS2:
        return false;
    case A_HALTSUM1:
        return s->num_harts >= 33;
    case A_HALTSUM2:
        return s->num_harts >= 1025;
    case A_HALTSUM3:
        return s->num_harts >= 32769;
    case A_SBADDRESS0:
    case A_SBADDRESS1:
    case A_SBADDRESS2:
    case A_SBADDRESS3:
        return dm_sba_addr_reg_present(s, addr);
    case A_SBDATA0:
    case A_SBDATA1:
    case A_SBDATA2:
    case A_SBDATA3:
        return dm_sba_data_reg_present(s, addr);
    default:
        return true;
    }
}

static void dm_update_impebreak(RISCVDMState *s)
{
    hwaddr ebreak_addr = RISCV_DM_ROM_PROGBUF + s->progbuf_size * 4;

    if (ebreak_addr + 4 > RISCV_DM_ROM_DATA) {
        return;
    }

    dm_rom_write32(s, ebreak_addr, s->impebreak ? DM_EBREAK : DM_NOP);
}

static void dm_reset_rom_state(RISCVDMState *s)
{
    if (!s->rom_ptr) {
        return;
    }

    memset(s->rom_ptr + RISCV_DM_ROM_WORK_BASE, 0, RISCV_DM_ROM_WORK_SIZE);

    dm_flush_cmd_space(s);

    for (uint32_t i = 0; i < s->num_abstract_data; i++) {
        dm_sync_data_to_rom(s, i);
    }
    for (uint32_t i = 0; i < s->progbuf_size; i++) {
        dm_sync_progbuf_to_rom(s, i);
    }

    dm_update_impebreak(s);
    dm_invalidate_dynamic_code(s);
}

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


static inline void dm_set_cmderr(RISCVDMState *s, uint32_t err)
{
    uint32_t cur = ARRAY_FIELD_EX32(s->regs, ABSTRACTCS, CMDERR);
    if (cur == RISCV_DM_CMDERR_NONE) {
        ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, CMDERR, err);
    }
}

static void dm_status_refresh(RISCVDMState *s)
{
    DMHartSelection sel;
    bool anyhalted = false, allhalted = true;
    bool anyrunning = false, allrunning = true;
    bool anyunavail = false, allunavail = true;
    bool anyresumeack = false, allresumeack = true;
    bool anyhavereset = false, allhavereset = true;
    bool anynonexistent = false, allnonexistent = false;
    bool reset_unavail = dm_ndmreset_active(s) ||
                         ARRAY_FIELD_EX32(s->regs, DMCONTROL, HARTRESET);

    dm_collect_selected_harts(s, &sel);

    anynonexistent = (sel.all_count > sel.valid_count);
    allnonexistent = (sel.valid_count == 0 && sel.all_count > 0);

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
static uint64_t dm_dmcontrol_pre_write(RegisterInfo *reg, uint64_t val64)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    uint32_t val = (uint32_t)val64;
    uint32_t cur_ctl = s->regs[R_DMCONTROL];
    bool busy = ARRAY_FIELD_EX32(s->regs, ABSTRACTCS, BUSY);
    bool ndmreset_was = FIELD_EX32(cur_ctl, DMCONTROL, NDMRESET);
    bool hartreset_was = FIELD_EX32(cur_ctl, DMCONTROL, HARTRESET);

    trace_riscv_dm_control_write(s->regs[R_DMCONTROL], val, busy);

    /* If busy, preserve hart selection and suppress run-control */
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

    bool dmactive = FIELD_EX32(val, DMCONTROL, DMACTIVE);

    if (!dmactive) {
        /* dmactive=0: reset the DM */
        dm_debug_reset(s);
        /* Return the reset value (only dmactive=0) */
        return 0;
    }

    /* Store first so helpers see updated fields */
    s->regs[R_DMCONTROL] = val;

    DMHartSelection sel;
    dm_collect_selected_harts(s, &sel);

    bool ndmreset = FIELD_EX32(val, DMCONTROL, NDMRESET);
    bool hartreset = FIELD_EX32(val, DMCONTROL, HARTRESET);

    if (ndmreset && !ndmreset_was) {
        dm_reset_all_harts(s);
    }
    if (hartreset && !hartreset_was) {
        dm_reset_selected_harts(s, &sel);
    }

    ARRAY_FIELD_DP32(s->regs, DMSTATUS, NDMRESETPENDING, ndmreset);

    /* ACKHAVERESET: clear havereset for selected harts */
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

    /* RESUMEREQ */
    bool resumereq = FIELD_EX32(val, DMCONTROL, RESUMEREQ);
    bool haltreq = FIELD_EX32(val, DMCONTROL, HALTREQ);

    if (!busy && resumereq && !haltreq) {
        for (int i = 0; i < sel.valid_count; i++) {
            int h = sel.harts[i];
            s->hart_resumeack[h] = false;
            dm_rom_write8(s, RISCV_DM_ROM_FLAGS + h, RISCV_DM_FLAG_RESUME);
            trace_riscv_dm_control_resume(h);
        }
    }

    /* HALTREQ */
    if (!busy) {
        if (haltreq) {
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

    /*
     * Build the stored value: only keep fields with readable semantics.
     * WARZ fields (haltreq, resumereq, ackhavereset, setresethaltreq,
     * clrresethaltreq, setkeepalive, clrkeepalive, ackunavail) read as 0.
     */
    uint32_t stored = 0;
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
                        hartreset);

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
    uint32_t v = 0;
    v = FIELD_DP32(v, HARTINFO, DATAADDR, RISCV_DM_ROM_DATA);
    v = FIELD_DP32(v, HARTINFO, DATASIZE, s->num_abstract_data);
    v = FIELD_DP32(v, HARTINFO, DATAACCESS, 1);
    v = FIELD_DP32(v, HARTINFO, NSCRATCH, s->nscratch);
    return v;
}

static uint64_t dm_hawindowsel_pre_write(RegisterInfo *reg, uint64_t val64)
{
    uint32_t max_sel = RISCV_DM_MAX_HARTS / RISCV_DM_HAWINDOW_SIZE;
    uint32_t val = (uint32_t)val64 & R_HAWINDOWSEL_HAWINDOWSEL_MASK;
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
        s->hawindow[wsel] = (uint32_t)val64;
    }
    return (uint32_t)val64;
}

static uint64_t dm_hawindow_post_read(RegisterInfo *reg, uint64_t val)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    uint32_t wsel = ARRAY_FIELD_EX32(s->regs, HAWINDOWSEL, HAWINDOWSEL);
    if (wsel < RISCV_DM_MAX_HARTS / RISCV_DM_HAWINDOW_SIZE) {
        return s->hawindow[wsel];
    }
    return 0;
}

static uint64_t dm_abstractcs_pre_write(RegisterInfo *reg, uint64_t val64)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);

    if (ARRAY_FIELD_EX32(s->regs, ABSTRACTCS, BUSY)) {
        dm_set_cmderr(s, RISCV_DM_CMDERR_BUSY);
        return s->regs[R_ABSTRACTCS];
    }

    /* W1C on CMDERR */
    uint32_t w1c_bits = FIELD_EX32((uint32_t)val64, ABSTRACTCS, CMDERR);
    uint32_t cur = s->regs[R_ABSTRACTCS];
    uint32_t cmderr = FIELD_EX32(cur, ABSTRACTCS, CMDERR);
    cmderr &= ~w1c_bits;
    cur = FIELD_DP32(cur, ABSTRACTCS, CMDERR, cmderr);

    return cur;
}

static uint64_t dm_command_pre_write(RegisterInfo *reg, uint64_t val64)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);

    /* Stub: abstract command execution added in a follow-on patch. */
    s->last_cmd = (uint32_t)val64;
    return s->regs[R_COMMAND];
}

static uint64_t dm_command_post_read(RegisterInfo *reg, uint64_t val)
{
    return 0;
}

static uint64_t dm_abstractauto_pre_write(RegisterInfo *reg, uint64_t val64)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);

    if (ARRAY_FIELD_EX32(s->regs, ABSTRACTCS, BUSY)) {
        dm_set_cmderr(s, RISCV_DM_CMDERR_BUSY);
        return s->regs[R_ABSTRACTAUTO];
    }

    /* Stub: autoexec trigger logic added in a follow-on patch. */
    uint32_t data_count = MIN(s->num_abstract_data, 12);
    uint32_t pbuf_size = MIN(s->progbuf_size, 16);
    uint32_t data_mask = data_count ? ((1u << data_count) - 1u) : 0;
    uint32_t pbuf_mask = pbuf_size ? ((1u << pbuf_size) - 1u) : 0;
    uint32_t mask = data_mask | (pbuf_mask << 16);

    return (uint32_t)val64 & mask;
}

static uint64_t dm_data_pre_write(RegisterInfo *reg, uint64_t val64)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);

    if (ARRAY_FIELD_EX32(s->regs, ABSTRACTCS, BUSY)) {
        dm_set_cmderr(s, RISCV_DM_CMDERR_BUSY);
        return s->regs[reg->access->addr / 4];
    }
    return (uint32_t)val64;
}

static void dm_data_post_write(RegisterInfo *reg, uint64_t val64)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    int index = (reg->access->addr - A_DATA0) / 4;

    dm_sync_data_to_rom(s, index);
}

static uint64_t dm_data_post_read(RegisterInfo *reg, uint64_t val)
{
    return val;
}

static uint64_t dm_progbuf_pre_write(RegisterInfo *reg, uint64_t val64)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);

    if (ARRAY_FIELD_EX32(s->regs, ABSTRACTCS, BUSY)) {
        dm_set_cmderr(s, RISCV_DM_CMDERR_BUSY);
        return s->regs[reg->access->addr / 4];
    }
    return (uint32_t)val64;
}

static void dm_progbuf_post_write(RegisterInfo *reg, uint64_t val64)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    int index = (reg->access->addr - A_PROGBUF0) / 4;

    dm_sync_progbuf_to_rom(s, index);
}

static uint64_t dm_progbuf_post_read(RegisterInfo *reg, uint64_t val)
{
    return val;
}

static uint64_t dm_haltsum0_post_read(RegisterInfo *reg, uint64_t val)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    uint32_t result = 0;
    uint32_t base = dm_get_hartsel(s) & ~0x1fu;

    for (uint32_t i = 0; i < 32; i++) {
        uint32_t h = base + i;
        if (h < s->num_harts && s->hart_halted[h]) {
            result |= (1u << i);
        }
    }
    return result;
}

static uint64_t dm_haltsum1_post_read(RegisterInfo *reg, uint64_t val)
{
    RISCVDMState *s = RISCV_DM(reg->opaque);
    uint32_t result = 0;
    uint32_t base = dm_get_hartsel(s) & ~0x3ffu;

    for (uint32_t g = 0; g < 32; g++) {
        for (uint32_t i = 0; i < 32; i++) {
            uint32_t h = base + g * 32 + i;
            if (h < s->num_harts && s->hart_halted[h]) {
                result |= (1u << g);
                break;
            }
        }
    }
    return result;
}


static RegisterAccessInfo riscv_dm_regs_info[] = {
    { .name = "DATA0",  .addr = A_DATA0,  .pre_write = dm_data_pre_write,
      .post_write = dm_data_post_write, .post_read = dm_data_post_read, },
    { .name = "DATA1",  .addr = A_DATA1,  .pre_write = dm_data_pre_write,
      .post_write = dm_data_post_write, .post_read = dm_data_post_read, },
    { .name = "DATA2",  .addr = A_DATA2,  .pre_write = dm_data_pre_write,
      .post_write = dm_data_post_write, .post_read = dm_data_post_read, },
    { .name = "DATA3",  .addr = A_DATA3,  .pre_write = dm_data_pre_write,
      .post_write = dm_data_post_write, .post_read = dm_data_post_read, },
    { .name = "DATA4",  .addr = A_DATA4,  .pre_write = dm_data_pre_write,
      .post_write = dm_data_post_write, .post_read = dm_data_post_read, },
    { .name = "DATA5",  .addr = A_DATA5,  .pre_write = dm_data_pre_write,
      .post_write = dm_data_post_write, .post_read = dm_data_post_read, },
    { .name = "DATA6",  .addr = A_DATA6,  .pre_write = dm_data_pre_write,
      .post_write = dm_data_post_write, .post_read = dm_data_post_read, },
    { .name = "DATA7",  .addr = A_DATA7,  .pre_write = dm_data_pre_write,
      .post_write = dm_data_post_write, .post_read = dm_data_post_read, },
    { .name = "DATA8",  .addr = A_DATA8,  .pre_write = dm_data_pre_write,
      .post_write = dm_data_post_write, .post_read = dm_data_post_read, },
    { .name = "DATA9",  .addr = A_DATA9,  .pre_write = dm_data_pre_write,
      .post_write = dm_data_post_write, .post_read = dm_data_post_read, },
    { .name = "DATA10", .addr = A_DATA10, .pre_write = dm_data_pre_write,
      .post_write = dm_data_post_write, .post_read = dm_data_post_read, },
    { .name = "DATA11", .addr = A_DATA11, .pre_write = dm_data_pre_write,
      .post_write = dm_data_post_write, .post_read = dm_data_post_read, },

    { .name = "DMCONTROL", .addr = A_DMCONTROL,
      .pre_write = dm_dmcontrol_pre_write, },

    { .name = "DMSTATUS", .addr = A_DMSTATUS,
      .ro = 0xFFFFFFFF,
      .post_read = dm_dmstatus_post_read, },

    { .name = "HARTINFO", .addr = A_HARTINFO,
      .ro = 0xFFFFFFFF,
      .post_read = dm_hartinfo_post_read, },

    { .name = "HALTSUM1", .addr = A_HALTSUM1,
      .ro = 0xFFFFFFFF,
      .post_read = dm_haltsum1_post_read, },

    { .name = "HAWINDOWSEL", .addr = A_HAWINDOWSEL,
      .pre_write = dm_hawindowsel_pre_write, },

    { .name = "HAWINDOW", .addr = A_HAWINDOW,
      .pre_write = dm_hawindow_pre_write,
      .post_read = dm_hawindow_post_read, },

    { .name = "ABSTRACTCS", .addr = A_ABSTRACTCS,
      .ro = R_ABSTRACTCS_DATACOUNT_MASK | R_ABSTRACTCS_BUSY_MASK |
            R_ABSTRACTCS_PROGBUFSIZE_MASK,
      .pre_write = dm_abstractcs_pre_write, },

    { .name = "COMMAND", .addr = A_COMMAND,
      .pre_write = dm_command_pre_write,
      .post_read = dm_command_post_read, },

    { .name = "ABSTRACTAUTO", .addr = A_ABSTRACTAUTO,
      .pre_write = dm_abstractauto_pre_write, },

    { .name = "CONFSTRPTR0", .addr = A_CONFSTRPTR0, .ro = 0xFFFFFFFF, },
    { .name = "CONFSTRPTR1", .addr = A_CONFSTRPTR1, .ro = 0xFFFFFFFF, },
    { .name = "CONFSTRPTR2", .addr = A_CONFSTRPTR2, .ro = 0xFFFFFFFF, },
    { .name = "CONFSTRPTR3", .addr = A_CONFSTRPTR3, .ro = 0xFFFFFFFF, },

    { .name = "NEXTDM", .addr = A_NEXTDM, .ro = 0xFFFFFFFF, },

    { .name = "PROGBUF0", .addr = A_PROGBUF0,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF1", .addr = A_PROGBUF1,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF2", .addr = A_PROGBUF2,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF3", .addr = A_PROGBUF3,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF4", .addr = A_PROGBUF4,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF5", .addr = A_PROGBUF5,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF6", .addr = A_PROGBUF6,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF7", .addr = A_PROGBUF7,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF8", .addr = A_PROGBUF8,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF9", .addr = A_PROGBUF9,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF10", .addr = A_PROGBUF10,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF11", .addr = A_PROGBUF11,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF12", .addr = A_PROGBUF12,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF13", .addr = A_PROGBUF13,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF14", .addr = A_PROGBUF14,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },
    { .name = "PROGBUF15", .addr = A_PROGBUF15,
      .pre_write = dm_progbuf_pre_write,
      .post_write = dm_progbuf_post_write,
      .post_read = dm_progbuf_post_read, },

    { .name = "AUTHDATA", .addr = A_AUTHDATA, },

    { .name = "DMCS2", .addr = A_DMCS2, },

    { .name = "HALTSUM2", .addr = A_HALTSUM2, .ro = 0xFFFFFFFF, },
    { .name = "HALTSUM3", .addr = A_HALTSUM3, .ro = 0xFFFFFFFF, },

    { .name = "SBADDRESS3", .addr = A_SBADDRESS3, },

    { .name = "SBCS", .addr = A_SBCS,
      .ro = R_SBCS_SBACCESS8_MASK | R_SBCS_SBACCESS16_MASK |
            R_SBCS_SBACCESS32_MASK | R_SBCS_SBACCESS64_MASK |
            R_SBCS_SBACCESS128_MASK | R_SBCS_SBASIZE_MASK |
            R_SBCS_SBBUSY_MASK | R_SBCS_SBVERSION_MASK, },

    { .name = "SBADDRESS0", .addr = A_SBADDRESS0, },

    { .name = "SBADDRESS1", .addr = A_SBADDRESS1, },

    { .name = "SBADDRESS2", .addr = A_SBADDRESS2, },

    { .name = "SBDATA0", .addr = A_SBDATA0, },

    { .name = "SBDATA1", .addr = A_SBDATA1, },

    { .name = "SBDATA2", .addr = A_SBDATA2, },
    { .name = "SBDATA3", .addr = A_SBDATA3, },

    { .name = "HALTSUM0", .addr = A_HALTSUM0,
      .ro = 0xFFFFFFFF,
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

    /* DATA area remap: reads from ROM DATA area return register values */
    if (offset >= RISCV_DM_ROM_DATA &&
        offset < RISCV_DM_ROM_DATA + s->num_abstract_data * 4) {
        int idx = (offset - RISCV_DM_ROM_DATA) / 4;
        if (size == 4) {
            ret = s->regs[R_DATA0 + idx];
        }
    }

    trace_riscv_dm_rom_access(offset, ret, size, false);
    return ret;
}

static void dm_rom_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
    RISCVDMState *s = opaque;

    stn_le_p(s->rom_ptr + offset, size, value);

    trace_riscv_dm_rom_access(offset, value, size, true);

    /* CPU wrote to HARTID register → hart has halted */
    if (offset == RISCV_DM_ROM_HARTID && size == 4) {
        uint32_t hartsel = (uint32_t)value;
        if (dm_hart_valid(s, hartsel)) {
            riscv_dm_hart_halted(s, hartsel);
        }
    }

    /* CPU wrote GOING acknowledgment */
    if (offset == RISCV_DM_ROM_GOING && size == 4) {
        uint32_t hartsel = dm_rom_read32(s, RISCV_DM_ROM_HARTID);
        if (dm_hart_valid(s, hartsel)) {
            dm_rom_write8(s, RISCV_DM_ROM_FLAGS + hartsel,
                          RISCV_DM_FLAG_CLEAR);
            trace_riscv_dm_going(hartsel);
        }
    }

    /* CPU wrote RESUME acknowledgment */
    if (offset == RISCV_DM_ROM_RESUME && size == 4) {
        uint32_t hartsel = (uint32_t)value;
        if (dm_hart_valid(s, hartsel)) {
            riscv_dm_hart_resumed(s, hartsel);
        }
    }

    /* CPU wrote EXCEPTION */
    if (offset == RISCV_DM_ROM_EXCP && size == 4) {
        uint32_t hartsel = dm_rom_read32(s, RISCV_DM_ROM_HARTID);
        if (dm_hart_valid(s, hartsel)) {
            riscv_dm_abstracts_exception(s, hartsel);
        }
    }

    /* DATA area remap: writes to ROM DATA area update registers */
    if (offset >= RISCV_DM_ROM_DATA &&
        offset < RISCV_DM_ROM_DATA + s->num_abstract_data * 4 && size == 4) {
        int idx = (offset - RISCV_DM_ROM_DATA) / 4;
        s->regs[R_DATA0 + idx] = (uint32_t)value;
    }
}

static const MemoryRegionOps dm_rom_ops = {
    .read = dm_rom_read,
    .write = dm_rom_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static bool dm_rom_realize(RISCVDMState *s, Error **errp)
{
    /*
     * ROM program at 0x800:
     * - entry: jump to _entry
     * - resume: jump to _resume
     * - exception: jump to _exception
     * - _entry: fence, save s0, poll FLAGS loop
     * - _exception: write EXCP, restore s0, ebreak
     * - going: write GOING, restore s0, jump to whereto
     * - _resume: write RESUME hartid, restore s0, dret
     */
    static const uint32_t rom_code_entry[] = {
        /* 0x800 <entry>: */
        0x0180006f,                               /* j   818 <_entry>      */
        0x00000013,                               /* nop                   */

        /* 0x808 <resume>: */
        0x0600006f,                               /* j   868 <_resume>     */
        0x00000013,                               /* nop                   */

        /* 0x810 <exception>: */
        0x0400006f,                               /* j   850 <_exception>  */
        0x00000013,                               /* nop                   */

        /* 0x818 <_entry>: */
        0x0ff0000f,                               /* fence                 */
        0x7b241073,                               /* csrw dscratch0, s0    */

        /* 0x820 <entry_loop>: */
        0xf1402473,                               /* csrr s0, mhartid      */
        0x07f47413,                               /* andi s0, s0, 127      */
        DM_STORE(8, 0u, RISCV_DM_ROM_HARTID, 2u), /* sw s0, HARTID(zero)   */
        DM_LBU(8, 8, RISCV_DM_ROM_FLAGS),         /* lbu s0, FLAGS(s0)     */
        0x00147413,                               /* andi s0, s0, 1        */
        0x02041463,                               /* bnez s0, 85c <going>  */
        0xf1402473,                               /* csrr s0, mhartid      */
        0x07f47413,                               /* andi s0, s0, 127      */
        DM_LBU(8, 8, RISCV_DM_ROM_FLAGS),         /* lbu s0, FLAGS(s0)     */
        0x00247413,                               /* andi s0, s0, 2        */
        0xfc0410e3,                               /* bnez s0, 808 <resume> */
        0xfd5ff06f,                               /* j    820 <entry_loop> */

        /* 0x850 <_exception>: */
        DM_STORE(0, 0u, RISCV_DM_ROM_EXCP, 2u),   /* sw zero, EXCP(zero)   */
        0x7b202473,                               /* csrr s0, dscratch0    */
        0x00100073,                               /* ebreak                */

        /* 0x85c <going>: */
        DM_STORE(0, 0u, RISCV_DM_ROM_GOING, 2u),  /* sw zero, GOING(zero)  */
        0x7b202473,                               /* csrr s0, dscratch0    */
        0xa9dff06f,                               /* j    300 <whereto>    */

        /* 0x868 <_resume>: */
        0xf1402473,                               /* csrr s0, mhartid      */
        0x07f47413,                               /* andi s0, s0, 127      */
        DM_STORE(8, 0u, RISCV_DM_ROM_RESUME, 2u), /* sw s0, RESUME(zero)   */
        0x7b202473,                               /* csrr s0, dscratch0    */
        0x7b200073,                               /* dret                  */
    };

    SysBusDevice *sbd = SYS_BUS_DEVICE(s);

    if (!memory_region_init_rom_device(&s->rom_mr, OBJECT(s), &dm_rom_ops, s,
                                       "riscv-dm.rom", RISCV_DM_SIZE,
                                       errp)) {
        return false;
    }
    s->rom_ptr = memory_region_get_ram_ptr(&s->rom_mr);

    memcpy(s->rom_ptr + RISCV_DM_ROM_ENTRY, rom_code_entry,
           sizeof(rom_code_entry));

    memory_region_init_alias(&s->rom_work_alias_mr, OBJECT(s),
                             "riscv-dm.rom-work", &s->rom_mr,
                             RISCV_DM_ROM_WORK_BASE, RISCV_DM_ROM_WORK_SIZE);
    memory_region_init_alias(&s->rom_entry_alias_mr, OBJECT(s),
                             "riscv-dm.rom-entry", &s->rom_mr,
                             RISCV_DM_ROM_ENTRY, RISCV_DM_ROM_ENTRY_SIZE);

    /*
     * Expose debug backing store in two non-overlapping physical windows:
     * - work area at low addresses (mailbox/data/progbuf/flags)
     * - ROM entry vector at VIRT_DM_ROM base
     */
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

    if (dm_abstract_cmd_completed(s, hartsel)) {
        riscv_dm_abstracts_done(s, hartsel);
    }

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
    dm_set_cmderr(s, RISCV_DM_CMDERR_EXCEPTION);
    trace_riscv_dm_abstract_cmd_exception(hartsel);
}


static void dm_debug_reset(RISCVDMState *s)
{
    s->dm_active = false;

    /* Reset all registers via framework */
    for (unsigned int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }

    /* Set config-dependent reset values */
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, VERSION, 3);       /* v1.0 */
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, AUTHENTICATED, 1);
    ARRAY_FIELD_DP32(s->regs, DMSTATUS, IMPEBREAK, s->impebreak ? 1 : 0);

    ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, DATACOUNT, s->num_abstract_data);
    ARRAY_FIELD_DP32(s->regs, ABSTRACTCS, PROGBUFSIZE, s->progbuf_size);

    /* Reset per-hart state */
    if (s->hart_resumeack && s->num_harts > 0) {
        for (uint32_t i = 0; i < s->num_harts; i++) {
            s->hart_halted[i] = false;
            s->hart_resumeack[i] = false;
            s->hart_havereset[i] = true;
            s->hart_resethaltreq[i] = false;
        }
    }

    memset(s->hawindow, 0, sizeof(s->hawindow));
    s->last_cmd = 0;

    dm_reset_rom_state(s);
    dm_status_refresh(s);
}


static void riscv_dm_init(Object *obj)
{
    RISCVDMState *s = RISCV_DM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->reg_array =
        register_init_block32(DEVICE(obj), riscv_dm_regs_info,
                              ARRAY_SIZE(riscv_dm_regs_info),
                              s->regs_info, s->regs,
                              &riscv_dm_ops, false,
                              RISCV_DM_REG_SIZE);

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

    if (s->num_abstract_data == 0 || s->num_abstract_data > 12) {
        error_setg(errp, "riscv-dm: datacount %u must be in range 1..12",
                   s->num_abstract_data);
        return;
    }

    if (s->progbuf_size > 16) {
        error_setg(errp, "riscv-dm: progbufsize %u exceeds maximum 16",
                   s->progbuf_size);
        return;
    }

    if (s->progbuf_size == 1 && !s->impebreak) {
        error_setg(errp,
                   "riscv-dm: progbufsize 1 requires impebreak to be enabled");
        return;
    }

    /* Allocate per-hart state */
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

    /* Apply initial reset */
    dm_debug_reset(s);
}

static void riscv_dm_reset_hold(Object *obj, ResetType type)
{
    RISCVDMState *s = RISCV_DM(obj);
    dm_debug_reset(s);
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
        VMSTATE_UINT32(last_cmd, RISCVDMState),
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
