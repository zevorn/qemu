/*
 * AMD MI300X gem5 Co-simulation PCIe Device
 *
 * Implements an MI300X-compatible PCIe endpoint that bridges QEMU's host
 * simulation to a gem5 GPU simulation. MMIO reads/writes are forwarded
 * to gem5 over a Unix domain socket, and VRAM is shared via mmap'd
 * shared memory. gem5 can issue DMA reads/writes back into QEMU guest
 * memory and raise MSI-X interrupts.
 *
 * Uses two separate socket connections to gem5:
 *   - MMIO socket: synchronous request/response for BAR read/write
 *   - Event socket: async gem5->QEMU notifications (IRQ, DMA)
 *
 * Usage:
 *   -device mi300x-gem5,gem5-socket=/tmp/gem5.sock,shmem-path=/dev/shm/mi300x-vram
 *
 * Copyright (c) 2025
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pcie.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/core/qdev-properties.h"
#include "migration/blocker.h"
#include "hw/misc/mi300x_gem5.h"
#include "trace.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

/* ======================================================================
 * gem5 Socket Communication
 * ====================================================================== */

static int mi300x_socket_connect(MI300XGem5State *s)
{
    struct sockaddr_un addr;
    int fd;

    if (!s->gem5_socket_path || s->gem5_socket_path[0] == '\0') {
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error_report("MI300X: failed to create socket: %s", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, s->gem5_socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_report("MI300X: failed to connect to gem5 at %s: %s",
                     s->gem5_socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static void mi300x_disconnect_gem5(MI300XGem5State *s)
{
    trace_mi300x_gem5_disconnect();

    if (s->gem5_mmio_fd >= 0) {
        MI300XGem5MsgHeader msg = {
            .type = cpu_to_le32(MI300X_MSG_SHUTDOWN),
            .size = 0,
        };
        ssize_t r = write(s->gem5_mmio_fd, &msg, sizeof(msg));
        (void)r;
        close(s->gem5_mmio_fd);
        s->gem5_mmio_fd = -1;
    }
    if (s->gem5_event_fd >= 0) {
        close(s->gem5_event_fd);
        s->gem5_event_fd = -1;
    }
}

static int mi300x_mmio_send(MI300XGem5State *s,
                            MI300XGem5MsgHeader *request,
                            MI300XGem5MsgHeader *response)
{
    ssize_t ret;

    if (s->gem5_mmio_fd < 0) {
        return -1;
    }

    qemu_mutex_lock(&s->mmio_mutex);

    request->id = cpu_to_le32(s->next_txn_id++);

    ret = write(s->gem5_mmio_fd, request, MI300X_MSG_HDR_SIZE);
    if (ret != MI300X_MSG_HDR_SIZE) {
        if (!s->stopping) {
            error_report("MI300X: lost connection to gem5 (write failed)");
        }
        close(s->gem5_mmio_fd);
        s->gem5_mmio_fd = -1;
        qemu_mutex_unlock(&s->mmio_mutex);
        return -1;
    }

    if (response) {
        ret = read(s->gem5_mmio_fd, response, MI300X_MSG_HDR_SIZE);
        if (ret != MI300X_MSG_HDR_SIZE) {
            if (!s->stopping) {
                error_report("MI300X: lost connection to gem5 (read failed)");
            }
            close(s->gem5_mmio_fd);
            s->gem5_mmio_fd = -1;
            qemu_mutex_unlock(&s->mmio_mutex);
            return -1;
        }
    }

    qemu_mutex_unlock(&s->mmio_mutex);
    return 0;
}

/* ======================================================================
 * Shared Memory (VRAM) Setup
 * ====================================================================== */

static int mi300x_setup_shmem(MI300XGem5State *s, Error **errp)
{
    int fd;
    void *ptr;

    if (!s->shmem_path || s->shmem_path[0] == '\0') {
        return 0;
    }

    fd = open(s->shmem_path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        error_setg_errno(errp, errno, "MI300X: failed to open shmem %s",
                         s->shmem_path);
        return -1;
    }

    if (ftruncate(fd, s->vram_size) < 0) {
        error_setg_errno(errp, errno, "MI300X: failed to resize shmem");
        close(fd);
        return -1;
    }

    ptr = mmap(NULL, s->vram_size, PROT_READ | PROT_WRITE,
               MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        error_setg_errno(errp, errno, "MI300X: failed to mmap shmem");
        close(fd);
        return -1;
    }

    s->shmem_fd = fd;
    s->shmem_ptr = ptr;
    trace_mi300x_gem5_shmem(s->shmem_path, s->vram_size);
    return 0;
}

static void mi300x_cleanup_shmem(MI300XGem5State *s)
{
    if (s->shmem_ptr) {
        munmap(s->shmem_ptr, s->vram_size);
        s->shmem_ptr = NULL;
    }
    if (s->shmem_fd >= 0) {
        close(s->shmem_fd);
        s->shmem_fd = -1;
    }
}

/* ======================================================================
 * gem5 Event Thread - receives async events on the EVENT socket
 *
 * This thread reads ONLY from gem5_event_fd (the second connection),
 * so there is no race with the MMIO path which uses gem5_mmio_fd.
 * ====================================================================== */

static void mi300x_handle_irq_raise(MI300XGem5State *s,
                                    MI300XGem5MsgHeader *msg)
{
    uint32_t vector = le64_to_cpu(msg->data) & 0xFFFF;

    trace_mi300x_gem5_irq_raise(vector);

    bql_lock();
    if (msix_enabled(PCI_DEVICE(s))) {
        if (vector < s->msix_vectors) {
            msix_notify(PCI_DEVICE(s), vector);
        }
    } else if (msi_enabled(PCI_DEVICE(s))) {
        msi_notify(PCI_DEVICE(s), vector);
    } else {
        s->irq_status |= (1u << vector);
        pci_set_irq(PCI_DEVICE(s), 1);
    }
    bql_unlock();
}

static void mi300x_handle_irq_lower(MI300XGem5State *s,
                                    MI300XGem5MsgHeader *msg)
{
    uint32_t vector = le64_to_cpu(msg->data) & 0xFFFF;

    trace_mi300x_gem5_irq_lower(vector);

    bql_lock();
    s->irq_status &= ~(1u << vector);
    if (!s->irq_status) {
        pci_set_irq(PCI_DEVICE(s), 0);
    }
    bql_unlock();
}

static void mi300x_handle_dma_read(MI300XGem5State *s,
                                   MI300XGem5MsgHeader *msg)
{
    uint64_t addr = le64_to_cpu(msg->addr);
    uint64_t len = le64_to_cpu(msg->data);
    MI300XGem5MsgHeader resp;
    ssize_t ret;

    trace_mi300x_gem5_dma_read(addr, len);

    if (len > MI300X_DMA_BUF_SIZE) {
        error_report("MI300X: DMA read too large: %" PRIu64, len);
        len = MI300X_DMA_BUF_SIZE;
    }

    bql_lock();
    pci_dma_read(PCI_DEVICE(s), addr, s->dma_buf, len);
    bql_unlock();

    /* Send DMA data back to gem5 on the event socket */
    resp = (MI300XGem5MsgHeader){
        .type = cpu_to_le32(MI300X_MSG_MMIO_RESP),
        .size = cpu_to_le32(len),
        .addr = msg->addr,
        .data = msg->data,
        .id = msg->id,
    };

    /* Event fd is only accessed by the event thread, no mutex needed */
    ret = write(s->gem5_event_fd, &resp, MI300X_MSG_HDR_SIZE);
    if (ret == MI300X_MSG_HDR_SIZE) {
        ret = write(s->gem5_event_fd, s->dma_buf, len);
    }

    if (ret < 0) {
        error_report("MI300X: failed to send DMA read response");
    }
}

static void mi300x_handle_dma_write(MI300XGem5State *s,
                                    MI300XGem5MsgHeader *msg)
{
    uint64_t addr = le64_to_cpu(msg->addr);
    uint64_t len = le64_to_cpu(msg->data);
    ssize_t ret;

    trace_mi300x_gem5_dma_write(addr, len);

    if (len > MI300X_DMA_BUF_SIZE) {
        error_report("MI300X: DMA write too large: %" PRIu64, len);
        return;
    }

    /* Read the payload data from the event socket */
    ret = read(s->gem5_event_fd, s->dma_buf, len);
    if (ret != (ssize_t)len) {
        error_report("MI300X: short DMA write payload read");
        return;
    }

    bql_lock();
    pci_dma_write(PCI_DEVICE(s), addr, s->dma_buf, len);
    bql_unlock();
}

static void *mi300x_event_thread(void *opaque)
{
    MI300XGem5State *s = opaque;
    MI300XGem5MsgHeader msg;
    ssize_t ret;

    while (!s->stopping && s->gem5_event_fd >= 0) {
        ret = read(s->gem5_event_fd, &msg, MI300X_MSG_HDR_SIZE);
        if (ret <= 0) {
            if (s->stopping) {
                break;
            }
            if (ret == 0) {
                error_report("MI300X: gem5 event connection closed");
            } else {
                error_report("MI300X: event read error: %s", strerror(errno));
            }
            break;
        }
        if (ret != MI300X_MSG_HDR_SIZE) {
            error_report("MI300X: short event header read");
            continue;
        }

        trace_mi300x_gem5_event(le32_to_cpu(msg.type),
                                le64_to_cpu(msg.addr),
                                le64_to_cpu(msg.data));

        switch (le32_to_cpu(msg.type)) {
        case MI300X_MSG_IRQ_RAISE:
            mi300x_handle_irq_raise(s, &msg);
            break;
        case MI300X_MSG_IRQ_LOWER:
            mi300x_handle_irq_lower(s, &msg);
            break;
        case MI300X_MSG_DMA_READ:
            mi300x_handle_dma_read(s, &msg);
            break;
        case MI300X_MSG_DMA_WRITE:
            mi300x_handle_dma_write(s, &msg);
            break;
        default:
            error_report("MI300X: unknown event type 0x%x",
                         le32_to_cpu(msg.type));
            break;
        }
    }

    return NULL;
}

/* ======================================================================
 * BAR0: GPU MMIO Register Space (forwarded to gem5)
 * ====================================================================== */

static uint64_t mi300x_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    MI300XGem5State *s = opaque;
    MI300XGem5MsgHeader req, resp;
    uint64_t val = 0;

    if (s->gem5_mmio_fd < 0) {
        /* Standalone mode: return identification registers */
        switch (addr) {
        case 0x00:
            return PCI_DEVICE_ID_MI300X;
        case 0x04:
            return 0;
        case 0xD000: /* GRBM_STATUS */
            return 0;
        case 0xD004: /* GRBM_STATUS2 */
            return 0;
        default:
            return 0;
        }
    }

    req = (MI300XGem5MsgHeader){
        .type = cpu_to_le32(MI300X_MSG_MMIO_READ),
        .size = 0,
        .addr = cpu_to_le64(addr),
        .data = 0,
        .access_size = cpu_to_le32(size),
    };

    if (mi300x_mmio_send(s, &req, &resp) == 0) {
        val = le64_to_cpu(resp.data);
    }

    trace_mi300x_gem5_mmio_read(addr, size, val);

    return val;
}

static void mi300x_mmio_write(void *opaque, hwaddr addr,
                              uint64_t val, unsigned size)
{
    MI300XGem5State *s = opaque;
    MI300XGem5MsgHeader req;

    trace_mi300x_gem5_mmio_write(addr, val, size);

    if (s->gem5_mmio_fd < 0) {
        return;
    }

    req = (MI300XGem5MsgHeader){
        .type = cpu_to_le32(MI300X_MSG_MMIO_WRITE),
        .size = 0,
        .addr = cpu_to_le64(addr),
        .data = cpu_to_le64(val),
        .access_size = cpu_to_le32(size),
    };

    mi300x_mmio_send(s, &req, NULL);
}

static const MemoryRegionOps mi300x_mmio_ops = {
    .read = mi300x_mmio_read,
    .write = mi300x_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

/* ======================================================================
 * BAR4: Doorbell / Signal Page
 * ====================================================================== */

static uint64_t mi300x_doorbell_read(void *opaque, hwaddr addr, unsigned size)
{
    MI300XGem5State *s = opaque;
    MI300XGem5MsgHeader req, resp;
    uint64_t val = 0;

    if (s->gem5_mmio_fd < 0) {
        return 0;
    }

    req = (MI300XGem5MsgHeader){
        .type = cpu_to_le32(MI300X_MSG_DB_READ),
        .size = 0,
        .addr = cpu_to_le64(addr),
        .data = 0,
        .access_size = cpu_to_le32(size),
    };

    if (mi300x_mmio_send(s, &req, &resp) == 0) {
        val = le64_to_cpu(resp.data);
    }

    trace_mi300x_gem5_doorbell_read(addr, size, val);

    return val;
}

static void mi300x_doorbell_write(void *opaque, hwaddr addr,
                                  uint64_t val, unsigned size)
{
    MI300XGem5State *s = opaque;
    MI300XGem5MsgHeader req;

    trace_mi300x_gem5_doorbell_write(addr, val, size);

    if (s->gem5_mmio_fd < 0) {
        return;
    }

    req = (MI300XGem5MsgHeader){
        .type = cpu_to_le32(MI300X_MSG_DB_WRITE),
        .size = 0,
        .addr = cpu_to_le64(addr),
        .data = cpu_to_le64(val),
        .access_size = cpu_to_le32(size),
    };

    /* Doorbell writes are fire-and-forget for performance */
    mi300x_mmio_send(s, &req, NULL);
}

static const MemoryRegionOps mi300x_doorbell_ops = {
    .read = mi300x_doorbell_read,
    .write = mi300x_doorbell_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

/* ======================================================================
 * PCI Config Space - MI300X specific setup
 * ====================================================================== */

static void mi300x_setup_pci_config(PCIDevice *pdev)
{
    uint8_t *pci_conf = pdev->config;

    /* Programming interface: VGA compatible controller */
    pci_config_set_prog_interface(pci_conf, 0x00);

    /* Capabilities: support 64-bit, prefetchable for BARs */
    pci_config_set_interrupt_pin(pci_conf, 1);

    /*
     * Set PCI subsystem IDs to match MI300X.
     * Some drivers (amdgpu) use subsystem IDs for device identification.
     */
    pci_set_word(pci_conf + PCI_SUBSYSTEM_VENDOR_ID,
                 PCI_SUBSYSTEM_VENDOR_ID_MI300X);
    pci_set_word(pci_conf + PCI_SUBSYSTEM_ID,
                 PCI_SUBSYSTEM_ID_MI300X);
}

/* ======================================================================
 * Device Lifecycle
 * ====================================================================== */

static void mi300x_gem5_realize(PCIDevice *pdev, Error **errp)
{
    MI300XGem5State *s = MI300X_GEM5(pdev);
    int ret;

    /* Setup PCI config space */
    mi300x_setup_pci_config(pdev);

    /* Initialize PCIe capabilities */
    ret = pcie_endpoint_cap_init(pdev, 0x80);
    if (ret < 0) {
        error_setg(errp, "MI300X: failed to init PCIe cap: %d", ret);
        return;
    }

    /*
     * BAR layout must match amdgpu driver expectations:
     *   BAR0+1: VRAM (64-bit, prefetchable)
     *   BAR2+3: Doorbell (64-bit)
     *   BAR4:   MSI-X (exclusive)
     *   BAR5:   MMIO registers (32-bit)
     */

    /* BAR0+1: VRAM (shared memory with gem5, 64-bit prefetchable) */
    if (s->shmem_path && s->shmem_path[0] != '\0') {
        if (mi300x_setup_shmem(s, errp) < 0) {
            return;
        }
        if (!memory_region_init_ram_from_fd(&s->vram_bar, OBJECT(s),
                                            "mi300x-vram", s->vram_size,
                                            RAM_SHARED, s->shmem_fd, 0,
                                            errp)) {
            mi300x_cleanup_shmem(s);
            return;
        }
    } else {
        if (!memory_region_init_ram(&s->vram_bar, OBJECT(s),
                                    "mi300x-vram", s->vram_size, errp)) {
            return;
        }
    }
    pci_register_bar(pdev, MI300X_VRAM_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->vram_bar);

    /* BAR2+3: Doorbell / signal pages (64-bit) */
    memory_region_init_io(&s->doorbell_bar, OBJECT(s), &mi300x_doorbell_ops, s,
                          "mi300x-doorbell", MI300X_DOORBELL_SIZE);
    pci_register_bar(pdev, MI300X_DOORBELL_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->doorbell_bar);

    /* BAR4: MSI-X (exclusive bar) */
    ret = msix_init_exclusive_bar(pdev, s->msix_vectors, MI300X_MSIX_BAR, errp);
    if (ret < 0) {
        error_prepend(errp, "MI300X: failed to init MSI-X: ");
        return;
    }

    /* Also support legacy MSI as fallback */
    ret = msi_init(pdev, 0x60, 32, true, false, errp);
    if (ret < 0) {
        error_prepend(errp, "MI300X: failed to init MSI: ");
        msix_uninit_exclusive_bar(pdev);
        return;
    }

    /* BAR5: MMIO register space (32-bit, forwarded to gem5) */
    memory_region_init_io(&s->mmio_bar, OBJECT(s), &mi300x_mmio_ops, s,
                          "mi300x-mmio", MI300X_MMIO_SIZE);
    pci_register_bar(pdev, MI300X_MMIO_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->mmio_bar);

    /* Allocate DMA staging buffer */
    s->dma_buf = g_malloc0(MI300X_DMA_BUF_SIZE);

    /* Initialize MMIO mutex */
    qemu_mutex_init(&s->mmio_mutex);

    /* Block migration */
    error_setg(&s->migration_blocker,
               "MI300X gem5 co-simulation device does not support migration");
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        g_free(s->dma_buf);
        return;
    }

    /*
     * Connect to gem5 if auto-connect enabled.
     * Two connections: one for MMIO (sync), one for events (async).
     */
    if (s->auto_connect && s->gem5_socket_path) {
        /* First connection: MMIO */
        s->gem5_mmio_fd = mi300x_socket_connect(s);
        if (s->gem5_mmio_fd >= 0) {
            trace_mi300x_gem5_connect(s->gem5_mmio_fd, "mmio");

            /* Send init message on MMIO socket */
            MI300XGem5MsgHeader init_msg = {
                .type = cpu_to_le32(MI300X_MSG_INIT),
                .size = 0,
                .data = cpu_to_le64(s->vram_size),
            };
            MI300XGem5MsgHeader init_resp;
            mi300x_mmio_send(s, &init_msg, &init_resp);

            trace_mi300x_gem5_init(s->vram_size,
                                   le64_to_cpu(init_resp.data));

            /* Second connection: Events (gem5->QEMU async) */
            s->gem5_event_fd = mi300x_socket_connect(s);
            if (s->gem5_event_fd >= 0) {
                trace_mi300x_gem5_connect(s->gem5_event_fd, "event");

                /* Start event thread only if event connection succeeded */
                s->stopping = false;
                s->event_thread_running = true;
                qemu_thread_create(&s->event_thread, "mi300x-gem5-events",
                                   mi300x_event_thread, s,
                                   QEMU_THREAD_JOINABLE);
            } else {
                error_report("MI300X: event connection failed, "
                            "DMA/IRQ from gem5 disabled");
            }
        } else {
            error_report("MI300X: running in standalone mode "
                        "(gem5 not connected)");
        }
    }

    trace_mi300x_gem5_realize(s->vram_size / (1024 * 1024));
}

static void mi300x_gem5_exit(PCIDevice *pdev)
{
    MI300XGem5State *s = MI300X_GEM5(pdev);

    /* Stop event thread */
    if (s->event_thread_running) {
        s->stopping = true;
        /* Close event socket to unblock the read in event thread */
        if (s->gem5_event_fd >= 0) {
            close(s->gem5_event_fd);
            s->gem5_event_fd = -1;
        }
        qemu_thread_join(&s->event_thread);
        s->event_thread_running = false;
    }

    mi300x_disconnect_gem5(s);

    migrate_del_blocker(&s->migration_blocker);

    qemu_mutex_destroy(&s->mmio_mutex);

    g_free(s->dma_buf);
    s->dma_buf = NULL;

    mi300x_cleanup_shmem(s);

    msix_uninit_exclusive_bar(pdev);
    msi_uninit(pdev);
}

static void mi300x_gem5_reset(DeviceState *dev)
{
    MI300XGem5State *s = MI300X_GEM5(dev);

    s->irq_status = 0;
    s->next_txn_id = 0;
}

static void mi300x_gem5_instance_init(Object *obj)
{
    MI300XGem5State *s = MI300X_GEM5(obj);

    s->gem5_mmio_fd = -1;
    s->gem5_event_fd = -1;
    s->shmem_fd = -1;
    s->shmem_ptr = NULL;
    s->event_thread_running = false;
    s->stopping = false;
}

/* ======================================================================
 * QOM Properties
 * ====================================================================== */

static const Property mi300x_gem5_properties[] = {
    DEFINE_PROP_STRING("gem5-socket", MI300XGem5State, gem5_socket_path),
    DEFINE_PROP_STRING("shmem-path", MI300XGem5State, shmem_path),
    DEFINE_PROP_UINT64("vram-size", MI300XGem5State, vram_size,
                       MI300X_VRAM_DEFAULT_SIZE),
    DEFINE_PROP_BOOL("auto-connect", MI300XGem5State, auto_connect, true),
    DEFINE_PROP_UINT32("msix-vectors", MI300XGem5State, msix_vectors,
                       MI300X_MSIX_VECTORS),
};

/* ======================================================================
 * Type Registration
 * ====================================================================== */

static void mi300x_gem5_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = mi300x_gem5_realize;
    k->exit = mi300x_gem5_exit;
    k->vendor_id = PCI_VENDOR_ID_ATI;
    k->device_id = PCI_DEVICE_ID_MI300X;
    k->revision = 0xc8;  /* MI300X revision */
    /* VGA-compatible: driver checks legacy ROM at 0xC0000 */
    k->class_id = PCI_CLASS_DISPLAY_VGA;

    device_class_set_legacy_reset(dc, mi300x_gem5_reset);
    device_class_set_props(dc, mi300x_gem5_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "AMD MI300X GPU (gem5 co-simulation bridge)";
}

static const TypeInfo mi300x_gem5_type_info = {
    .name          = TYPE_MI300X_GEM5,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(MI300XGem5State),
    .instance_init = mi300x_gem5_instance_init,
    .class_init    = mi300x_gem5_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void mi300x_gem5_register_types(void)
{
    type_register_static(&mi300x_gem5_type_info);
}

type_init(mi300x_gem5_register_types)
