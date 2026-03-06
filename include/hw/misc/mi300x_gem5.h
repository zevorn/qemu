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
 * MI300X BAR layout:
 *   BAR0: MMIO registers (256MB) - GPU register space forwarded to gem5
 *   BAR2: VRAM (shared memory, configurable size) - GPU framebuffer/HBM
 *   BAR4: Doorbell (8MB) - GPU doorbell/signal page
 */
#define MI300X_MMIO_BAR         0
#define MI300X_VRAM_BAR         2
#define MI300X_DOORBELL_BAR     4

#define MI300X_MMIO_SIZE        (256 * 1024 * 1024)   /* 256 MB */
#define MI300X_DOORBELL_SIZE    (8 * 1024 * 1024)     /* 8 MB */
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
 */
struct MI300XGem5State {
    /* Private */
    PCIDevice parent_obj;

    /* MMIO BARs */
    MemoryRegion mmio_bar;      /* BAR0: GPU register space */
    MemoryRegion vram_bar;      /* BAR2: VRAM (shared memory) */
    MemoryRegion doorbell_bar;  /* BAR4: Doorbell/signal pages */

    /* gem5 connection */
    char *gem5_socket_path;     /* path to gem5 Unix domain socket */
    char *shmem_path;           /* path to shared memory file for VRAM */
    int gem5_sock_fd;           /* socket fd to gem5 (-1 if disconnected) */
    int shmem_fd;               /* shared memory fd for VRAM */
    void *shmem_ptr;            /* mmap'd VRAM shared memory */

    /* Configuration */
    uint64_t vram_size;         /* VRAM size in bytes */
    bool auto_connect;          /* connect to gem5 on realize */
    uint32_t msix_vectors;      /* number of MSI-X vectors */

    /* Transaction tracking */
    uint32_t next_txn_id;       /* monotonic transaction ID counter */
    QemuMutex txn_mutex;        /* protects socket I/O */

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
