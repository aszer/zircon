// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hypervisor/vcpu.h>
#include <hypervisor/virtio.h>
#include <magenta/syscalls/port.h>
#include <mxtl/unique_ptr.h>

#include <virtio/virtio.h>
#include <virtio/virtio_ring.h>

// clang-format off

/* PCI macros. */
#define PCI_ALIGN(n)                    ((((uintptr_t)n) + 4095) & ~4095)

#define PCI_VENDOR_ID_VIRTIO            0x1af4u

static constexpr uint16_t virtio_pci_legacy_id(uint16_t virtio_id) {
      return static_cast<uint16_t>(virtio_id + 0xfffu);
}

// clang-format on

// Convert guest-physical addresses to usable virtual addresses.
#define guest_paddr_to_host_vaddr(device, addr) \
    (static_cast<mx_vaddr_t>(((device)->guest_physmem_addr) + (addr)))

// Returns a circular index into a Virtio ring.
static uint32_t ring_index(virtio_queue_t* queue, uint32_t index) {
    return index % queue->size;
}

static int ring_avail_count(virtio_queue_t* queue) {
    if (queue->avail == NULL)
        return 0;
    return queue->avail->idx - queue->index;
}

static virtio_device_t* pci_device_to_virtio(const pci_device_t* device) {
    return (virtio_device_t*)device->impl;
}

static virtio_queue_t* selected_queue(const virtio_device_t* device) {
    return device->queue_sel < device->num_queues ? &device->queues[device->queue_sel] : NULL;
}

static mx_status_t virtio_pci_legacy_read(const pci_device_t* pci_device, uint8_t bar,
                                          uint16_t port, mx_vcpu_io_t* vcpu_io) {
    if (bar != 0)
        return MX_ERR_NOT_SUPPORTED;

    virtio_device_t* device = pci_device_to_virtio(pci_device);
    const virtio_queue_t* queue = selected_queue(device);
    switch (port) {
    case VIRTIO_PCI_DEVICE_FEATURES:
        vcpu_io->access_size = 4;
        vcpu_io->u32 = device->features;
        return MX_OK;
    case VIRTIO_PCI_QUEUE_PFN:
        if (!queue)
            return MX_ERR_NOT_SUPPORTED;
        vcpu_io->access_size = 4;
        vcpu_io->u32 = queue->pfn;
        return MX_OK;
    case VIRTIO_PCI_QUEUE_SIZE:
        if (!queue)
            return MX_ERR_NOT_SUPPORTED;
        vcpu_io->access_size = 2;
        vcpu_io->u16 = queue->size;
        return MX_OK;
    case VIRTIO_PCI_DEVICE_STATUS:
        vcpu_io->access_size = 1;
        vcpu_io->u8 = device->status;
        return MX_OK;
    case VIRTIO_PCI_ISR_STATUS:
        vcpu_io->access_size = 1;
        mtx_lock(&device->mutex);
        vcpu_io->u8 = device->isr_status;

        // From VIRTIO 1.0 Section 4.1.4.5:
        //
        // To avoid an extra access, simply reading this register resets it to
        // 0 and causes the device to de-assert the interrupt.
        device->isr_status = 0;
        mtx_unlock(&device->mutex);
        return MX_OK;
    }

    // Handle device-specific accesses.
    if (port >= VIRTIO_PCI_DEVICE_CFG_BASE) {
        uint16_t device_offset = static_cast<uint16_t>(port - VIRTIO_PCI_DEVICE_CFG_BASE);
        return device->ops->read(device, device_offset, vcpu_io);
    }

    fprintf(stderr, "Unhandled virtio device read %#x\n", port);
    return MX_ERR_NOT_SUPPORTED;
}

static mx_status_t virtio_queue_set_pfn(virtio_queue_t* queue, uint32_t pfn) {
    virtio_device_t* device = queue->virtio_device;
    uintptr_t mem_addr = device->guest_physmem_addr;
    size_t mem_size = device->guest_physmem_size;

    queue->pfn = pfn;

    // Descriptor Table.
    uintptr_t desc_paddr = queue->pfn * PAGE_SIZE;
    uintptr_t desc_size = queue->size * sizeof(queue->desc[0]);
    mx_vaddr_t desc_host_vaddr = guest_paddr_to_host_vaddr(device, desc_paddr);
    queue->desc = reinterpret_cast<decltype(queue->desc)>(desc_host_vaddr);

    // Avail Ring.
    uintptr_t avail_paddr = desc_paddr + desc_size;
    uintptr_t avail_size = sizeof(queue->avail) + (queue->size * sizeof(queue->avail->ring[0]));
    mx_vaddr_t avail_host_vaddr = guest_paddr_to_host_vaddr(device, avail_paddr);
    queue->avail = reinterpret_cast<decltype(queue->avail)>(avail_host_vaddr);

    // Used Event.
    uintptr_t used_event_paddr = avail_paddr + avail_size;
    uintptr_t used_event_size = sizeof(queue->used_event);
    mx_vaddr_t used_event_host_vaddr = guest_paddr_to_host_vaddr(device, used_event_paddr);
    queue->used_event = reinterpret_cast<decltype(queue->used_event)>(used_event_host_vaddr);

    // Used Ring.
    uintptr_t used_paddr = PCI_ALIGN(used_event_paddr + used_event_size);
    uintptr_t used_size = sizeof(queue->used) + (queue->size * sizeof(queue->used->ring[0]));
    mx_vaddr_t used_host_vaddr = guest_paddr_to_host_vaddr(device, used_paddr);
    queue->used = reinterpret_cast<decltype(queue->used)>(used_host_vaddr);

    // Avail Event.
    uintptr_t avail_event_paddr = used_paddr + used_size;
    uintptr_t avail_event_size = sizeof(queue->avail_event);
    mx_vaddr_t avail_event_host_paddr = guest_paddr_to_host_vaddr(device, avail_event_paddr);
    queue->avail_event = reinterpret_cast<decltype(queue->avail_event)>(avail_event_host_paddr);

    mx_vaddr_t end = avail_event_host_paddr + avail_event_size;
    if (end < desc_paddr || end > mem_addr + mem_size) {
        fprintf(stderr, "Ring is outside of guest memory\n");
        memset(queue, 0, sizeof(*queue));
        return MX_ERR_OUT_OF_RANGE;
    }

    return MX_OK;
}

static void virtio_queue_signal(virtio_queue_t* queue) {
    mtx_lock(&queue->mutex);
    if (ring_avail_count(queue) > 0)
        cnd_signal(&queue->avail_ring_cnd);
    mtx_unlock(&queue->mutex);
}

static mx_status_t virtio_pci_legacy_write(pci_device_t* pci_device, uint8_t bar, uint16_t port,
                                           const mx_packet_guest_io_t* io) {
    if (bar != 0)
        return MX_ERR_NOT_SUPPORTED;

    virtio_device_t* device = pci_device_to_virtio(pci_device);
    virtio_queue_t* queue = selected_queue(device);
    switch (port) {
    case VIRTIO_PCI_DRIVER_FEATURES:
        if (io->access_size != 4)
            return MX_ERR_IO_DATA_INTEGRITY;
        // Currently we expect the driver to accept all our features.
        if (io->u32 != device->features)
            return MX_ERR_INVALID_ARGS;
        return MX_OK;
    case VIRTIO_PCI_DEVICE_STATUS:
        if (io->access_size != 1)
            return MX_ERR_IO_DATA_INTEGRITY;
        device->status = io->u8;
        return MX_OK;
    case VIRTIO_PCI_QUEUE_PFN: {
        if (io->access_size != 4)
            return MX_ERR_IO_DATA_INTEGRITY;
        if (!queue)
            return MX_ERR_NOT_SUPPORTED;
        return virtio_queue_set_pfn(queue, io->u32);
    }
    case VIRTIO_PCI_QUEUE_SIZE:
        if (io->access_size != 2)
            return MX_ERR_IO_DATA_INTEGRITY;
        queue->size = io->u16;
        return MX_OK;
    case VIRTIO_PCI_QUEUE_SELECT:
        if (io->access_size != 2)
            return MX_ERR_IO_DATA_INTEGRITY;
        if (io->u16 >= device->num_queues) {
            fprintf(stderr, "Selected queue does not exist.\n");
            return MX_ERR_NOT_SUPPORTED;
        }
        device->queue_sel = io->u16;
        return MX_OK;
    case VIRTIO_PCI_QUEUE_NOTIFY: {
        if (io->access_size != 2)
            return MX_ERR_IO_DATA_INTEGRITY;
        if (io->u16 >= device->num_queues) {
            fprintf(stderr, "Notify queue does not exist.\n");
            return MX_ERR_NOT_SUPPORTED;
        }

        // Invoke the device callback if one has been provided.
        uint16_t queue_sel = io->u16;
        if (device->ops->queue_notify != NULL) {
            mx_status_t status = device->ops->queue_notify(device, queue_sel);
            if (status != MX_OK) {
                fprintf(stderr, "Failed to handle queue notify event. Error %d\n", status);
                return status;
            }

            // Send an interrupt back to the guest if we've generated one while
            // processing the queue.
            if (device->isr_status > 0) {
                return pci_interrupt(&device->pci_device);
            }
        }

        // Notify threads waiting on a descriptor.
        virtio_queue_signal(&device->queues[queue_sel]);
        return MX_OK;
    }
    }

    // Handle device-specific accesses.
    if (port >= VIRTIO_PCI_DEVICE_CFG_BASE) {
        uint16_t device_offset = static_cast<uint16_t>(port - VIRTIO_PCI_DEVICE_CFG_BASE);
        return device->ops->write(device, device_offset, io);
    }

    fprintf(stderr, "Unhandled virtio device write %#x\n", port);
    return MX_ERR_NOT_SUPPORTED;
}

static const pci_device_ops_t kVirtioPciLegacyDeviceOps = {
    .read_bar = &virtio_pci_legacy_read,
    .write_bar = &virtio_pci_legacy_write,
};

void virtio_pci_init(virtio_device_t* device) {
    device->pci_device.vendor_id = PCI_VENDOR_ID_VIRTIO;
    device->pci_device.device_id = virtio_pci_legacy_id(device->device_id);
    device->pci_device.subsystem_vendor_id = 0;
    device->pci_device.subsystem_id = device->device_id;
    device->pci_device.class_code = 0;
    device->pci_device.bar[0].size = static_cast<uint16_t>(
        sizeof(virtio_pci_legacy_config_t) + device->config_size);
    device->pci_device.impl = device;
    device->pci_device.ops = &kVirtioPciLegacyDeviceOps;
}

mx_status_t virtio_device_notify(virtio_device_t* device) {
    return pci_interrupt(&device->pci_device);
}

// This must not return any errors besides MX_ERR_NOT_FOUND.
static mx_status_t virtio_queue_next_avail_locked(virtio_queue_t* queue, uint16_t* index) {
    if (ring_avail_count(queue) < 1)
        return MX_ERR_NOT_FOUND;

    *index = queue->avail->ring[ring_index(queue, queue->index++)];
    return MX_OK;
}

mx_status_t virtio_queue_next_avail(virtio_queue_t* queue, uint16_t* index) {
    mtx_lock(&queue->mutex);
    mx_status_t status = virtio_queue_next_avail_locked(queue, index);
    mtx_unlock(&queue->mutex);
    return status;
}

void virtio_queue_wait(virtio_queue_t* queue, uint16_t* index) {
    mtx_lock(&queue->mutex);
    while (virtio_queue_next_avail_locked(queue, index) == MX_ERR_NOT_FOUND)
        cnd_wait(&queue->avail_ring_cnd, &queue->mutex);
    mtx_unlock(&queue->mutex);
}

typedef struct poll_task_args {
    virtio_queue_t* queue;
    virtio_queue_poll_fn_t handler;
    void* ctx;
} poll_task_args_t;

static int virtio_queue_poll_task(void* ctx) {
    mx_status_t result = MX_OK;
    mxtl::unique_ptr<poll_task_args_t> args(static_cast<poll_task_args_t*>(ctx));
    while (true) {
        uint16_t descriptor;
        virtio_queue_wait(args->queue, &descriptor);

        uint32_t used = 0;
        mx_status_t status = args->handler(args->queue, descriptor, &used, args->ctx);
        virtio_queue_return(args->queue, descriptor, used);

        if (status == MX_ERR_STOP)
            break;
        if (status != MX_OK) {
            fprintf(stderr, "Error %d while handling queue buffer.\n", status);
            result = status;
            break;
        }

        result = virtio_device_notify(args->queue->virtio_device);
        if (result != MX_OK)
            break;
    }

    return result;
}

mx_status_t virtio_queue_poll(virtio_queue_t* queue, virtio_queue_poll_fn_t handler, void* ctx) {
    poll_task_args_t* args = new poll_task_args_t{queue, handler, ctx};

    thrd_t thread;
    int ret = thrd_create(&thread, virtio_queue_poll_task, args);
    if (ret != thrd_success) {
        fprintf(stderr, "Failed to create queue thread %d\n", ret);
        delete args;
        return MX_ERR_INTERNAL;
    }

    ret = thrd_detach(thread);
    if (ret != thrd_success) {
        fprintf(stderr, "Failed to detach queue thread %d\n", ret);
        return MX_ERR_INTERNAL;
    }

    return MX_OK;
}

mx_status_t virtio_queue_read_desc(virtio_queue_t* queue, uint16_t desc_index,
                                   virtio_desc_t* out) {
    virtio_device_t* device = queue->virtio_device;
    volatile struct vring_desc& desc = queue->desc[desc_index];
    size_t mem_size = device->guest_physmem_size;

    const uint64_t end = desc.addr + desc.len;
    if (end < desc.addr || end > mem_size)
        return MX_ERR_OUT_OF_RANGE;

    out->addr = reinterpret_cast<void*>(guest_paddr_to_host_vaddr(device, desc.addr));
    out->len = desc.len;
    out->has_next = desc.flags & VRING_DESC_F_NEXT;
    out->writable = desc.flags & VRING_DESC_F_WRITE;
    out->next = desc.next;
    return MX_OK;
}

void virtio_queue_return(virtio_queue_t* queue, uint16_t index, uint32_t len) {
    mtx_lock(&queue->mutex);

    volatile struct vring_used_elem* used =
        &queue->used->ring[ring_index(queue, queue->used->idx)];

    used->id = index;
    used->len = len;
    queue->used->idx++;

    mtx_unlock(&queue->mutex);

    // Set the queue bit in the device ISR so that the driver knows to check
    // the queues on the next interrupt.
    virtio_device_t* device = queue->virtio_device;
    mtx_lock(&device->mutex);
    device->isr_status |= VIRTIO_ISR_QUEUE;
    mtx_unlock(&device->mutex);
}

mx_status_t virtio_queue_handler(virtio_queue_t* queue, virtio_queue_fn_t handler, void* context) {
    uint16_t head;
    uint32_t used_len = 0;
    uintptr_t mem_addr = queue->virtio_device->guest_physmem_addr;
    size_t mem_size = queue->virtio_device->guest_physmem_size;

    // Get the next descriptor from the available ring. If none are available
    // we can just no-op.
    mx_status_t status = virtio_queue_next_avail(queue, &head);
    if (status == MX_ERR_NOT_FOUND)
        return MX_OK;
    if (status != MX_OK)
        return status;

    status = MX_OK;
    uint16_t desc_index = head;
    volatile const struct vring_desc* desc;
    do {
        desc = &queue->desc[desc_index];

        const uint64_t end = desc->addr + desc->len;
        if (end < desc->addr || end > mem_size)
            return MX_ERR_OUT_OF_RANGE;

        void* addr = reinterpret_cast<void*>(mem_addr + desc->addr);
        status = handler(addr, desc->len, desc->flags, &used_len, context);
        if (status != MX_OK) {
            fprintf(stderr, "Virtio request (%#lx, %u) failed %d\n", desc->addr, desc->len, status);
            return status;
        }

        desc_index = desc->next;
    } while (desc->flags & VRING_DESC_F_NEXT);

    virtio_queue_return(queue, head, used_len);

    return ring_avail_count(queue) > 0 ? MX_ERR_NEXT : MX_OK;
}
