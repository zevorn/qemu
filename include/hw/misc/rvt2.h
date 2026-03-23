/*
 * RVT2 Ternary MatMul Accelerator - QEMU Device Model
 *
 * Virtual PCI device for developing and testing the RVT2 driver stack.
 * Emulates a compute accelerator with descriptor-based command submission,
 * DMA, MSI-X interrupts, and a mailbox interface for firmware emulation.
 *
 * Compute operation: D = A × B + C  (matrix FMA, float32)
 *
 * Copyright (c) 2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RVT2_H
#define HW_MISC_RVT2_H

#include "hw/pci/pci_device.h"
#include "qom/object.h"

#define TYPE_RVT2 "rvt2"

OBJECT_DECLARE_SIMPLE_TYPE(Rvt2State, RVT2)

/* PCI identification */
#define RVT2_DEVICE_ID          0x1de2
#define RVT2_REVISION           0x01

/* BAR0 register map (MMIO, 4KiB) */
#define RVT2_REG_ID             0x00    /* RO: device identification */
#define RVT2_REG_VERSION        0x04    /* RO: hardware version */
#define RVT2_REG_STATUS         0x08    /* RO: device status */
#define RVT2_REG_CONTROL        0x0C    /* RW: device control */
#define RVT2_REG_IRQ_STATUS     0x10    /* RO: interrupt status (clear on read) */
#define RVT2_REG_IRQ_MASK       0x14    /* RW: interrupt mask */
#define RVT2_REG_CMDQ_BASE_LO  0x20    /* RW: command queue base address [31:0] */
#define RVT2_REG_CMDQ_BASE_HI  0x24    /* RW: command queue base address [63:32] */
#define RVT2_REG_CMDQ_SIZE     0x28    /* RW: command queue size (entries) */
#define RVT2_REG_CMDQ_HEAD     0x2C    /* RO: device read pointer */
#define RVT2_REG_CMDQ_TAIL     0x30    /* RW: host write pointer */
#define RVT2_REG_DOORBELL       0x34    /* WO: doorbell (write any value to trigger) */
#define RVT2_REG_CPLQ_BASE_LO  0x40    /* RW: completion queue base address [31:0] */
#define RVT2_REG_CPLQ_BASE_HI  0x44    /* RW: completion queue base address [63:32] */
#define RVT2_REG_CPLQ_SIZE     0x48    /* RW: completion queue size (entries) */
#define RVT2_REG_CPLQ_HEAD     0x4C    /* RW: host read pointer */
#define RVT2_REG_CPLQ_TAIL     0x50    /* RO: device write pointer */
#define RVT2_REG_ENGINE_COUNT   0x60    /* RO: number of compute engines */
#define RVT2_REG_MAX_DESC_SIZE  0x64    /* RO: max descriptor size in bytes */
/* Mailbox registers */
#define RVT2_REG_MBOX_CMD      0x80    /* RW: mailbox command */
#define RVT2_REG_MBOX_STATUS   0x84    /* RO: mailbox status */
#define RVT2_REG_MBOX_DATA0    0x88    /* RW: mailbox data word 0 */
#define RVT2_REG_MBOX_DATA1    0x8C    /* RW: mailbox data word 1 */
#define RVT2_REG_MBOX_DATA2    0x90    /* RW: mailbox data word 2 */
#define RVT2_REG_MBOX_DATA3    0x94    /* RW: mailbox data word 3 */

#define RVT2_BAR0_SIZE          0x1000  /* 4KiB */

/* Device ID magic */
#define RVT2_ID_MAGIC           0x52565432  /* "RVT2" in ASCII */
#define RVT2_HW_VERSION         0x00010000  /* v1.0.0 */

/* Status register bits */
#define RVT2_STATUS_READY       (1 << 0)
#define RVT2_STATUS_BUSY        (1 << 1)
#define RVT2_STATUS_ERROR       (1 << 2)
#define RVT2_STATUS_FW_LOADED   (1 << 3)

/* Control register bits */
#define RVT2_CTRL_ENABLE        (1 << 0)
#define RVT2_CTRL_RESET         (1 << 1)

/* IRQ bits */
#define RVT2_IRQ_COMPLETION     (1 << 0)
#define RVT2_IRQ_FAULT          (1 << 1)

/* MSI-X vectors */
#define RVT2_MSIX_VEC_COMPLETION    0
#define RVT2_MSIX_VEC_FAULT         1
#define RVT2_MSIX_VEC_COUNT         2
#define RVT2_MSIX_BAR               4   /* exclusive BAR for MSI-X table */

/* Mailbox commands */
#define RVT2_MBOX_CMD_NOP           0x00
#define RVT2_MBOX_CMD_INIT          0x01
#define RVT2_MBOX_CMD_QUERY_CAP     0x02
#define RVT2_MBOX_CMD_HEARTBEAT     0x03

/* Mailbox status */
#define RVT2_MBOX_STATUS_IDLE       0x00
#define RVT2_MBOX_STATUS_BUSY       0x01
#define RVT2_MBOX_STATUS_DONE       0x02
#define RVT2_MBOX_STATUS_ERROR      0x03

/* Capability query response layout (in mbox data words) */
#define RVT2_CAP_ENGINE_COUNT       1       /* data0: engine count */
#define RVT2_CAP_MAX_DESC_SIZE      64      /* data1: max descriptor bytes */
#define RVT2_CAP_FW_VERSION         0x0100  /* data2: firmware version */
#define RVT2_CAP_SUPPORTED_OPS      0x01    /* data3: bitmask, bit0 = ternary_matmul */

/*
 * Descriptor format (64 bytes, cache-line aligned)
 *
 * The host writes descriptors to the command queue. The device fetches
 * them via DMA and processes each one.
 */
#define RVT2_DESC_SIZE              64

#define RVT2_OP_TERNARY_MATMUL     0x01

#define RVT2_DTYPE_FLOAT32         0x00
#define RVT2_DTYPE_FLOAT16         0x01
#define RVT2_DTYPE_INT8            0x02

typedef struct QEMU_PACKED Rvt2Descriptor {
    uint32_t opcode;            /* 0x00: operation code */
    uint32_t flags;             /* 0x04: flags */
    uint64_t input_a_addr;      /* 0x08: DMA address of matrix A */
    uint64_t input_b_addr;      /* 0x10: DMA address of matrix B */
    uint64_t input_c_addr;      /* 0x18: DMA address of matrix C */
    uint64_t output_d_addr;     /* 0x20: DMA address of result D */
    uint32_t m;                 /* 0x28: rows of A and D */
    uint32_t n;                 /* 0x2C: cols of B and D */
    uint32_t k;                 /* 0x30: cols of A / rows of B */
    uint32_t dtype;             /* 0x34: data type */
    uint64_t fence_seqno;       /* 0x38: fence sequence number */
} Rvt2Descriptor;               /* total: 64 bytes */

QEMU_BUILD_BUG_ON(sizeof(Rvt2Descriptor) != RVT2_DESC_SIZE);

/* Completion entry written by device to completion queue */
typedef struct QEMU_PACKED Rvt2Completion {
    uint64_t fence_seqno;       /* matching fence seqno */
    uint32_t status;            /* 0 = success, non-zero = error code */
    uint32_t reserved;
} Rvt2Completion;

#define RVT2_CPL_SIZE               16

/* Default capabilities */
#define RVT2_DEFAULT_ENGINE_COUNT   1
#define RVT2_DEFAULT_CMDQ_MAX      256

/*
 * Device state
 */
struct Rvt2State {
    PCIDevice pdev;

    /* BAR0: MMIO registers */
    MemoryRegion mmio;

    /* Register state */
    uint32_t status;
    uint32_t control;
    uint32_t irq_status;
    uint32_t irq_mask;

    /* Command queue */
    uint64_t cmdq_base;
    uint32_t cmdq_size;
    uint32_t cmdq_head;     /* device read pointer */
    uint32_t cmdq_tail;     /* host write pointer */

    /* Completion queue */
    uint64_t cplq_base;
    uint32_t cplq_size;
    uint32_t cplq_head;     /* host read pointer */
    uint32_t cplq_tail;     /* device write pointer */

    /* Mailbox */
    uint32_t mbox_cmd;
    uint32_t mbox_status;
    uint32_t mbox_data[4];

    /* DMA processing timer (simulates async compute) */
    QEMUTimer compute_timer;

    /* Fence tracking */
    uint64_t last_completed_seqno;
};

#endif /* HW_MISC_RVT2_H */
