/*
 * RVT2 Ternary MatMul Accelerator - QEMU Device Model
 *
 * Virtual PCI device for driver stack development. Emulates a compute
 * accelerator with descriptor-based command submission, DMA, MSI-X,
 * and a mailbox interface for firmware emulation.
 *
 * Copyright (c) 2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "hw/misc/rvt2.h"

/* ---- Interrupt helpers ---- */

static void rvt2_raise_irq(Rvt2State *s, uint32_t irq_bit, int vector)
{
    s->irq_status |= irq_bit;
    if (s->irq_mask & irq_bit) {
        return; /* masked */
    }
    if (msix_enabled(&s->pdev)) {
        msix_notify(&s->pdev, vector);
    }
}

static void rvt2_write_completion(Rvt2State *s, const Rvt2Completion *cpl)
{
    uint64_t cpl_addr;

    if (!s->cplq_base || s->cplq_size == 0) {
        return;
    }
    if (s->cplq_head >= s->cplq_size || s->cplq_tail >= s->cplq_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rvt2: invalid cplq state head=%u tail=%u size=%u\n",
                      s->cplq_head, s->cplq_tail, s->cplq_size);
        return;
    }

    cpl_addr = s->cplq_base + (uint64_t)s->cplq_tail * RVT2_CPL_SIZE;
    pci_dma_write(&s->pdev, cpl_addr, cpl, sizeof(*cpl));
    s->cplq_tail = (s->cplq_tail + 1) % s->cplq_size;
}

static void rvt2_latch_fault(Rvt2State *s, uint64_t seqno, uint32_t status)
{
    Rvt2Completion cpl = {
        .fence_seqno = seqno,
        .status = status,
    };

    s->status |= RVT2_STATUS_ERROR;
    rvt2_write_completion(s, &cpl);
    rvt2_raise_irq(s, RVT2_IRQ_FAULT, RVT2_MSIX_VEC_FAULT);
}

/* ---- Mailbox emulation ---- */

static void rvt2_mbox_process(Rvt2State *s)
{
    switch (s->mbox_cmd) {
    case RVT2_MBOX_CMD_NOP:
        s->mbox_status = RVT2_MBOX_STATUS_DONE;
        break;

    case RVT2_MBOX_CMD_INIT:
        /* Firmware init: mark device ready */
        s->status |= RVT2_STATUS_FW_LOADED | RVT2_STATUS_READY;
        s->mbox_data[0] = 0; /* success */
        s->mbox_status = RVT2_MBOX_STATUS_DONE;
        break;

    case RVT2_MBOX_CMD_QUERY_CAP:
        s->mbox_data[0] = RVT2_CAP_ENGINE_COUNT;
        s->mbox_data[1] = RVT2_CAP_MAX_DESC_SIZE;
        s->mbox_data[2] = RVT2_CAP_FW_VERSION;
        s->mbox_data[3] = RVT2_CAP_SUPPORTED_OPS;
        s->mbox_status = RVT2_MBOX_STATUS_DONE;
        break;

    case RVT2_MBOX_CMD_HEARTBEAT:
        s->mbox_data[0] = 1; /* alive */
        s->mbox_status = RVT2_MBOX_STATUS_DONE;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rvt2: unknown mailbox command 0x%x\n", s->mbox_cmd);
        s->mbox_status = RVT2_MBOX_STATUS_ERROR;
        break;
    }
    s->mbox_cmd = RVT2_MBOX_CMD_NOP;
}

/* ---- Compute emulation (D = A * B + C) ---- */

static bool rvt2_process_one_descriptor(Rvt2State *s, Rvt2Descriptor *desc)
{
    uint32_t m = desc->m, n = desc->n, k = desc->k;
    uint32_t elements_a, elements_b, elements_c, elements_d;
    float *a = NULL, *b = NULL, *c = NULL, *d = NULL;
    Rvt2Completion cpl = { 0 };
    uint32_t i, j, p;

    if (desc->opcode != RVT2_OP_TERNARY_MATMUL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rvt2: unsupported opcode 0x%x\n", desc->opcode);
        rvt2_latch_fault(s, desc->fence_seqno, 1);
        return false;
    }

    if (desc->dtype != RVT2_DTYPE_FLOAT32) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rvt2: unsupported dtype %u (only float32)\n",
                      desc->dtype);
        rvt2_latch_fault(s, desc->fence_seqno, 2);
        return false;
    }

    if (m == 0 || n == 0 || k == 0 || m > 4096 || n > 4096 || k > 4096) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rvt2: invalid dimensions m=%u n=%u k=%u\n", m, n, k);
        rvt2_latch_fault(s, desc->fence_seqno, 3);
        return false;
    }

    elements_a = m * k;
    elements_b = k * n;
    elements_c = m * n;
    elements_d = m * n;

    a = g_malloc(elements_a * sizeof(float));
    b = g_malloc(elements_b * sizeof(float));
    c = g_malloc(elements_c * sizeof(float));
    d = g_malloc(elements_d * sizeof(float));

    /* DMA read inputs */
    if (pci_dma_read(&s->pdev, desc->input_a_addr,
                     a, elements_a * sizeof(float)) != 0) {
        rvt2_latch_fault(s, desc->fence_seqno, 4);
        goto fault;
    }
    if (pci_dma_read(&s->pdev, desc->input_b_addr,
                     b, elements_b * sizeof(float)) != 0) {
        rvt2_latch_fault(s, desc->fence_seqno, 4);
        goto fault;
    }
    if (pci_dma_read(&s->pdev, desc->input_c_addr,
                     c, elements_c * sizeof(float)) != 0) {
        rvt2_latch_fault(s, desc->fence_seqno, 4);
        goto fault;
    }

    /* Compute D = A * B + C (naive matmul, row-major) */
    for (i = 0; i < m; i++) {
        for (j = 0; j < n; j++) {
            float sum = 0.0f;
            for (p = 0; p < k; p++) {
                sum += a[i * k + p] * b[p * n + j];
            }
            d[i * n + j] = sum + c[i * n + j];
        }
    }

    /* DMA write output */
    if (pci_dma_write(&s->pdev, desc->output_d_addr,
                      d, elements_d * sizeof(float)) != 0) {
        rvt2_latch_fault(s, desc->fence_seqno, 4);
        goto fault;
    }

    cpl.fence_seqno = desc->fence_seqno;
    cpl.status = 0; /* success */
    s->last_completed_seqno = desc->fence_seqno;
    rvt2_write_completion(s, &cpl);

fault:
    g_free(a);
    g_free(b);
    g_free(c);
    g_free(d);
    return cpl.status == 0;
}

static void rvt2_process_cmdq(Rvt2State *s)
{
    uint32_t tail_snapshot;
    uint32_t processed = 0;
    bool faulted = false;

    /* Validate queue configuration */
    if (s->cmdq_size == 0 || s->cmdq_base == 0) {
        if (s->cmdq_head != s->cmdq_tail) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "rvt2: cmdq not configured (base=0x%" PRIx64
                          " size=%u) but has pending work\n",
                          s->cmdq_base, s->cmdq_size);
            rvt2_latch_fault(s, 0, 4);
        }
        return;
    }
    if (s->cmdq_head >= s->cmdq_size || s->cmdq_tail >= s->cmdq_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rvt2: invalid cmdq state head=%u tail=%u size=%u\n",
                      s->cmdq_head, s->cmdq_tail, s->cmdq_size);
        rvt2_latch_fault(s, 0, 4);
        return;
    }
    if (!s->cplq_base || s->cplq_size == 0 ||
        s->cplq_head >= s->cplq_size || s->cplq_tail >= s->cplq_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rvt2: invalid cplq state base=0x%" PRIx64
                      " head=%u tail=%u size=%u\n",
                      s->cplq_base, s->cplq_head, s->cplq_tail, s->cplq_size);
        rvt2_latch_fault(s, 0, 4);
        return;
    }

    tail_snapshot = s->cmdq_tail;

    while (s->cmdq_head != tail_snapshot) {
        Rvt2Descriptor desc;
        uint64_t desc_addr = s->cmdq_base +
                             (uint64_t)s->cmdq_head *
                             RVT2_DESC_SIZE;

        if (pci_dma_read(&s->pdev, desc_addr, &desc, sizeof(desc)) != 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "rvt2: failed to read descriptor at 0x%" PRIx64 "\n",
                          desc_addr);
            rvt2_latch_fault(s, 0, 4);
            faulted = true;
            break;
        }

        s->status |= RVT2_STATUS_BUSY;
        if (!rvt2_process_one_descriptor(s, &desc)) {
            faulted = true;
            break;
        }
        s->cmdq_head = (s->cmdq_head + 1) % s->cmdq_size;
        processed++;
    }

    s->status &= ~RVT2_STATUS_BUSY;

    /* Only raise completion interrupt if we actually processed descriptors */
    if (processed > 0) {
        rvt2_raise_irq(s, RVT2_IRQ_COMPLETION, RVT2_MSIX_VEC_COMPLETION);
    }
    if (faulted) {
        s->cmdq_tail = s->cmdq_head;
    }
}

static void rvt2_compute_timer_cb(void *opaque)
{
    Rvt2State *s = opaque;
    rvt2_process_cmdq(s);
}

/* ---- MMIO read/write ---- */

static uint64_t rvt2_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Rvt2State *s = opaque;
    uint32_t val = 0;

    switch (addr) {
    case RVT2_REG_ID:
        val = RVT2_ID_MAGIC;
        break;
    case RVT2_REG_VERSION:
        val = RVT2_HW_VERSION;
        break;
    case RVT2_REG_STATUS:
        val = s->status;
        break;
    case RVT2_REG_CONTROL:
        val = s->control;
        break;
    case RVT2_REG_IRQ_STATUS:
        val = s->irq_status;
        s->irq_status = 0; /* clear on read */
        break;
    case RVT2_REG_IRQ_MASK:
        val = s->irq_mask;
        break;
    case RVT2_REG_CMDQ_BASE_LO:
        val = (uint32_t)s->cmdq_base;
        break;
    case RVT2_REG_CMDQ_BASE_HI:
        val = (uint32_t)(s->cmdq_base >> 32);
        break;
    case RVT2_REG_CMDQ_SIZE:
        val = s->cmdq_size;
        break;
    case RVT2_REG_CMDQ_HEAD:
        val = s->cmdq_head;
        break;
    case RVT2_REG_CMDQ_TAIL:
        val = s->cmdq_tail;
        break;
    case RVT2_REG_CPLQ_BASE_LO:
        val = (uint32_t)s->cplq_base;
        break;
    case RVT2_REG_CPLQ_BASE_HI:
        val = (uint32_t)(s->cplq_base >> 32);
        break;
    case RVT2_REG_CPLQ_SIZE:
        val = s->cplq_size;
        break;
    case RVT2_REG_CPLQ_HEAD:
        val = s->cplq_head;
        break;
    case RVT2_REG_CPLQ_TAIL:
        val = s->cplq_tail;
        break;
    case RVT2_REG_ENGINE_COUNT:
        val = RVT2_DEFAULT_ENGINE_COUNT;
        break;
    case RVT2_REG_MAX_DESC_SIZE:
        val = RVT2_DESC_SIZE;
        break;
    case RVT2_REG_MBOX_CMD:
        val = s->mbox_cmd;
        break;
    case RVT2_REG_MBOX_STATUS:
        val = s->mbox_status;
        break;
    case RVT2_REG_MBOX_DATA0:
        val = s->mbox_data[0];
        break;
    case RVT2_REG_MBOX_DATA1:
        val = s->mbox_data[1];
        break;
    case RVT2_REG_MBOX_DATA2:
        val = s->mbox_data[2];
        break;
    case RVT2_REG_MBOX_DATA3:
        val = s->mbox_data[3];
        break;
    case RVT2_REG_HDM_SIZE:
        val = RVT2_HDM_SIZE;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rvt2: read from unknown register 0x%" HWADDR_PRIx "\n",
                      addr);
        val = 0;
        break;
    }

    return val;
}

static void rvt2_mmio_write(void *opaque, hwaddr addr,
                            uint64_t val, unsigned size)
{
    Rvt2State *s = opaque;

    switch (addr) {
    case RVT2_REG_CONTROL:
        if (val & RVT2_CTRL_RESET) {
            /* Soft reset */
            s->status = 0;
            s->control = 0;
            s->irq_status = 0;
            s->cmdq_head = 0;
            s->cmdq_tail = 0;
            s->cplq_head = 0;
            s->cplq_tail = 0;
            s->mbox_cmd = RVT2_MBOX_CMD_NOP;
            s->mbox_status = RVT2_MBOX_STATUS_IDLE;
            s->last_completed_seqno = 0;
            timer_del(&s->compute_timer);
        } else {
            s->control = (uint32_t)val;
        }
        break;
    case RVT2_REG_IRQ_MASK:
        s->irq_mask = (uint32_t)val;
        break;
    case RVT2_REG_CMDQ_BASE_LO:
        s->cmdq_base = (s->cmdq_base & 0xFFFFFFFF00000000ULL) | (val & 0xFFFFFFFF);
        break;
    case RVT2_REG_CMDQ_BASE_HI:
        s->cmdq_base = (s->cmdq_base & 0x00000000FFFFFFFFULL) | (val << 32);
        break;
    case RVT2_REG_CMDQ_SIZE:
        s->cmdq_size = (uint32_t)val;
        break;
    case RVT2_REG_CMDQ_TAIL:
        s->cmdq_tail = (uint32_t)val;
        break;
    case RVT2_REG_DOORBELL:
        /* Trigger command processing with a small delay to simulate async */
        if ((s->control & RVT2_CTRL_ENABLE) &&
            (s->status & RVT2_STATUS_READY)) {
            timer_mod(&s->compute_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
        }
        break;
    case RVT2_REG_CPLQ_BASE_LO:
        s->cplq_base = (s->cplq_base & 0xFFFFFFFF00000000ULL) | (val & 0xFFFFFFFF);
        break;
    case RVT2_REG_CPLQ_BASE_HI:
        s->cplq_base = (s->cplq_base & 0x00000000FFFFFFFFULL) | (val << 32);
        break;
    case RVT2_REG_CPLQ_SIZE:
        s->cplq_size = (uint32_t)val;
        break;
    case RVT2_REG_CPLQ_HEAD:
        s->cplq_head = (uint32_t)val;
        break;
    case RVT2_REG_MBOX_CMD:
        s->mbox_cmd = (uint32_t)val;
        s->mbox_status = RVT2_MBOX_STATUS_BUSY;
        rvt2_mbox_process(s);
        break;
    case RVT2_REG_MBOX_DATA0:
        s->mbox_data[0] = (uint32_t)val;
        break;
    case RVT2_REG_MBOX_DATA1:
        s->mbox_data[1] = (uint32_t)val;
        break;
    case RVT2_REG_MBOX_DATA2:
        s->mbox_data[2] = (uint32_t)val;
        break;
    case RVT2_REG_MBOX_DATA3:
        s->mbox_data[3] = (uint32_t)val;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rvt2: write to unknown register 0x%" HWADDR_PRIx
                      " val=0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps rvt2_mmio_ops = {
    .read = rvt2_mmio_read,
    .write = rvt2_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ---- PCI lifecycle ---- */

static void rvt2_realize(PCIDevice *pdev, Error **errp)
{
    Rvt2State *s = RVT2(pdev);
    int rc;

    /* BAR0: MMIO registers */
    memory_region_init_io(&s->mmio, OBJECT(s), &rvt2_mmio_ops, s,
                          "rvt2-mmio", RVT2_BAR0_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    /* BAR2: HDM (device-managed memory, CXL Type-2 stub) */
    s->hdm_buf = g_malloc0(RVT2_HDM_SIZE);
    memory_region_init_ram_ptr(&s->hdm, OBJECT(s), "rvt2-hdm",
                               RVT2_HDM_SIZE, s->hdm_buf);
    pci_register_bar(pdev, RVT2_HDM_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->hdm);

    /* MSI-X on exclusive BAR4 */
    rc = msix_init_exclusive_bar(pdev, RVT2_MSIX_VEC_COUNT, RVT2_MSIX_BAR,
                                 errp);
    if (rc) {
        return;
    }
    msix_vector_use(pdev, RVT2_MSIX_VEC_COMPLETION);
    msix_vector_use(pdev, RVT2_MSIX_VEC_FAULT);

    /* Compute timer */
    timer_init_ns(&s->compute_timer, QEMU_CLOCK_VIRTUAL,
                  rvt2_compute_timer_cb, s);

    /* Initial state */
    s->status = 0;
    s->control = 0;
    s->mbox_status = RVT2_MBOX_STATUS_IDLE;
}

static void rvt2_exit(PCIDevice *pdev)
{
    Rvt2State *s = RVT2(pdev);

    timer_del(&s->compute_timer);
    msix_uninit_exclusive_bar(pdev);
    g_free(s->hdm_buf);
}

static void rvt2_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize = rvt2_realize;
    k->exit = rvt2_exit;
    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = RVT2_DEVICE_ID;
    k->revision = RVT2_REVISION;
    k->class_id = PCI_CLASS_PROCESSOR_CO;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "RVT2 Ternary MatMul Accelerator";
}

static const TypeInfo rvt2_types[] = {
    {
        .name          = TYPE_RVT2,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(Rvt2State),
        .class_init    = rvt2_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    }
};

DEFINE_TYPES(rvt2_types)
