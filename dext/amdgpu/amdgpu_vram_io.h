#pragma once

#include <stdint.h>
#include <string.h>

namespace amdgpu {

// Bootstrap command and completion storage lives in visible VRAM. CPU staging
// buffers are not GPU mappings; their PCI DMA addresses must not reach firmware.
// DriverKit's PCI memory accessors return void, so verify staged writes before
// publishing the queue. These helpers accept BAR0 offsets, not GPU addresses.
static inline bool vram_io_range(const DeviceContext &dev, uint64_t offset,
                                  uint64_t bytes)
{
    return bytes != 0 && (offset & 3) == 0 && (bytes & 3) == 0 &&
        offset <= dev.bar0Size && bytes <= dev.bar0Size - offset;
}

static inline kern_return_t
vram_write_verified(const DeviceContext &dev, uint64_t offset,
               const void *source, uint64_t bytes)
{
    if (!dev.pci) return kIOReturnNotAttached;
    if (!source || !vram_io_range(dev, offset, bytes)) return kIOReturnBadArgument;
    const auto *src = static_cast<const uint8_t *>(source);
    for (uint64_t i = 0; i < bytes; i += 4) {
        uint32_t word;
        memcpy(&word, src + i, sizeof(word));
        dev.pci->MemoryWrite32(dev.bar0MemIndex, offset + i, word);
    }
    // Readback also drains the CPU's posted BAR writes before the doorbell.
    for (uint64_t i = 0; i < bytes; i += 4) {
        uint32_t expected, observed = UINT32_MAX;
        memcpy(&expected, src + i, sizeof(expected));
        dev.pci->MemoryRead32(dev.bar0MemIndex, offset + i, &observed);
        if (observed != expected) return kIOReturnIOError;
    }
    return kIOReturnSuccess;
}

static inline kern_return_t
vram_read_fence64(const DeviceContext &dev, uint64_t offset, uint64_t *value)
{
    if (!dev.pci) return kIOReturnNotAttached;
    if (!value || (offset & 7) || !vram_io_range(dev, offset, 8))
        return kIOReturnBadArgument;
    dev.pci->MemoryRead64(dev.bar0MemIndex, offset, value);
    // Neither API nor query completion uses an all-ones value.
    return *value == UINT64_MAX ? kIOReturnNotAttached : kIOReturnSuccess;
}

static inline kern_return_t
vram_read_fence32(const DeviceContext &dev, uint64_t offset, uint32_t *value)
{
    if (!dev.pci) return kIOReturnNotAttached;
    if (!value || !vram_io_range(dev, offset, 4)) return kIOReturnBadArgument;
    dev.pci->MemoryRead32(dev.bar0MemIndex, offset, value);
    // UINT32_MAX is reserved and must never be issued as a fence sequence.
    return *value == UINT32_MAX ? kIOReturnNotAttached : kIOReturnSuccess;
}

static inline kern_return_t
vram_clear_verified(const DeviceContext &dev, uint64_t offset, uint64_t bytes)
{
    if (!dev.pci) return kIOReturnNotAttached;
    if (!vram_io_range(dev, offset, bytes)) return kIOReturnBadArgument;
    const uint32_t zeros[64]{};
    while (bytes) {
        const uint64_t chunk = bytes < sizeof(zeros) ? bytes : sizeof(zeros);
        const auto r = vram_write_verified(dev, offset, zeros, chunk);
        if (r != kIOReturnSuccess) return r;
        offset += chunk;
        bytes -= chunk;
    }
    return kIOReturnSuccess;
}

} // namespace amdgpu
