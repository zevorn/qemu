/*
 * AMD MI300X gem5 Co-simulation PCIe Device
 *
 * This device acts as a PCIe bridge between QEMU (host simulation) and
 * gem5 (MI300X GPU simulation). It exposes MI300X-compatible PCIe config
 * space and BARs, forwarding MMIO accesses to gem5 via Unix socket and
 * sharing VRAM/doorbell memory via shared memory regions.
 *
 * Copyright (c) 2025
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MI300X_GEM5_H
#define HW_MI300X_GEM5_H

#include "hw/pci/pci_device.h"
#include "qom/object.h"

#define TYPE_MI300X_GEM5 "mi300x-gem5"
OBJECT_DECLARE_SIMPLE_TYPE(MI300XGem5State, MI300X_GEM5)

/*
 * AMD GPU PCI IDs
 * Vendor: 0x1002 (AMD/ATI)
 * Device: 0x74a0 (MI300X - gfx942)
 */
#define PCI_VENDOR_ID_ATI               0x1002
#define PCI_DEVICE_ID_MI300X            0x74a0
#define PCI_SUBSYSTEM_VENDOR_ID_MI300X  0x1002
#define PCI_SUBSYSTEM_ID_MI300X         0x74a0

/*
 * MI300X BAR layout (must match amdgpu driver expectations):
 *   BAR0+1: VRAM (16GB, 64-bit, prefetchable) - GPU framebuffer/HBM
 *   BAR2+3: Doorbell (2MB, 64-bit) - GPU doorbell/signal pages
 *   BAR4:   MSI-X (exclusive bar)
 *   BAR5:   MMIO registers (512KB, 32-bit) - forwarded to gem5
 *
 * The amdgpu driver hardcodes: VRAM=BAR0, Doorbell=BAR2, MMIO=BAR5.
 */
#define MI300X_VRAM_BAR         0
#define MI300X_DOORBELL_BAR     2
#define MI300X_MSIX_BAR         4
#define MI300X_MMIO_BAR         5

#define MI300X_MMIO_SIZE        (512 * 1024)           /* 512 KB */
#define MI300X_DOORBELL_SIZE    (2 * 1024 * 1024)      /* 2 MB */
#define MI300X_VRAM_DEFAULT_SIZE (16ULL * 1024 * 1024 * 1024) /* 16 GB default */

#define MI300X_MSIX_VECTORS     256

/*
 * gem5 co-simulation protocol message types.
 * Communication is over a Unix domain socket with a simple TLV protocol.
 */
enum MI300XGem5MsgType {
    /* QEMU -> gem5 */
    MI300X_MSG_MMIO_READ    = 0x01,
    MI300X_MSG_MMIO_WRITE   = 0x02,
    MI300X_MSG_DB_READ      = 0x03,
    MI300X_MSG_DB_WRITE     = 0x04,
    MI300X_MSG_DMA_REQ      = 0x05,
    MI300X_MSG_INIT         = 0x06,
    MI300X_MSG_SHUTDOWN     = 0x07,

    /* gem5 -> QEMU */
    MI300X_MSG_MMIO_RESP    = 0x81,
    MI300X_MSG_IRQ_RAISE    = 0x82,
    MI300X_MSG_IRQ_LOWER    = 0x83,
    MI300X_MSG_DMA_READ     = 0x84,
    MI300X_MSG_DMA_WRITE    = 0x85,
    MI300X_MSG_INIT_RESP    = 0x86,
};

/*
 * Wire format for messages between QEMU and gem5.
 * All fields are little-endian.
 */
typedef struct MI300XGem5MsgHeader {
    uint32_t type;          /* MI300XGem5MsgType */
    uint32_t size;          /* payload size in bytes */
    uint64_t addr;          /* address for MMIO/DMA operations */
    uint64_t data;          /* data value or DMA length */
    uint32_t access_size;   /* 1/2/4/8 byte access width */
    uint32_t id;            /* transaction ID for request/response matching */
} __attribute__((packed)) MI300XGem5MsgHeader;

#define MI300X_MSG_HDR_SIZE sizeof(MI300XGem5MsgHeader)
#define MI300X_DMA_BUF_SIZE (4 * 1024 * 1024) /* 4MB DMA staging buffer */

/*
 * Device state
 *
 * Uses two socket connections to gem5 to avoid race conditions:
 *   - gem5_mmio_fd: synchronous MMIO request/response (QEMU main thread)
 *   - gem5_event_fd: async events from gem5 (IRQ, DMA) (event thread)
 */
struct MI300XGem5State {
    /* Private */
    PCIDevice parent_obj;

    /* PCI BARs */
    MemoryRegion vram_bar;      /* BAR0: VRAM (shared memory, 64-bit) */
    MemoryRegion doorbell_bar;  /* BAR2: Doorbell/signal pages (64-bit) */
    MemoryRegion mmio_bar;      /* BAR5: GPU register space (32-bit) */

    /* gem5 connection - two sockets for clean separation */
    char *gem5_socket_path;     /* path to gem5 Unix domain socket */
    char *shmem_path;           /* path to shared memory file for VRAM */
    int gem5_mmio_fd;           /* socket fd for MMIO (sync req/resp) */
    int gem5_event_fd;          /* socket fd for events (async gem5->QEMU) */
    int shmem_fd;               /* shared memory fd for VRAM */
    void *shmem_ptr;            /* mmap'd VRAM shared memory */

    /* Configuration */
    uint64_t vram_size;         /* VRAM size in bytes */
    bool auto_connect;          /* connect to gem5 on realize */
    uint32_t msix_vectors;      /* number of MSI-X vectors */

    /* Transaction tracking */
    uint32_t next_txn_id;       /* monotonic transaction ID counter */
    QemuMutex mmio_mutex;       /* protects MMIO socket I/O */

    /* gem5 async event handling */
    QemuThread event_thread;    /* thread for receiving gem5 events */
    bool event_thread_running;
    bool stopping;

    /* Interrupt state */
    uint32_t irq_status;

    /* DMA staging buffer */
    uint8_t *dma_buf;

    /* Migration blocker */
    Error *migration_blocker;
};

#endif /* HW_MI300X_GEM5_H */
