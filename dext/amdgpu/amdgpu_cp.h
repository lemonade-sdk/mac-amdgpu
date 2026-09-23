// GFX12 kernel graphics queue and PM4 submission.
// MES KIQ maps the queue from a VRAM-backed graphics MQD.
// Ring and write-back storage use GPU-addressable visible VRAM.

#pragma once

#include <stdint.h>

#ifdef __APPLE__
#include <DriverKit/IOBufferMemoryDescriptor.h>
#endif

#include "amdgpu_ip.h"
#include "amdgpu_regs.h"
#include "amdgpu_vram.h"
#include "amdgpu_pm4.h"
#include "amdgpu_cp_registers.h"
#include "amdgpu_cp_firmware.h"

namespace amdgpu {
struct MESContext;

struct GMCContext;   // forward — we ask its VRAM allocator for storage

// CP_ME_CNTL bit positions per gc_12_0_0_sh_mask.h:13868-13889.
//   PFP_HALT__SHIFT = 0x1a (26) → MASK 0x04000000
//   ME_HALT__SHIFT  = 0x1c (28) → MASK 0x10000000
// (Previous values for ME_HALT in this header were WRONG — fixed per
// upstream sh_mask. Audit-7 #1.)
#define CP_ME_CNTL__PFP_HALT__SHIFT 0x1a
#define CP_ME_CNTL__PFP_HALT_MASK   0x04000000
#define CP_ME_CNTL__ME_HALT__SHIFT  0x1c
#define CP_ME_CNTL__ME_HALT_MASK    0x10000000

// CP_MEC_RS64_CNTL bit positions per gc_12_0_0_sh_mask.h:15886-15909.
#define CP_MEC_RS64_CNTL__MEC_INVALIDATE_ICACHE__SHIFT 0x4
#define CP_MEC_RS64_CNTL__MEC_INVALIDATE_ICACHE_MASK   0x00000010
#define CP_MEC_RS64_CNTL__MEC_PIPE0_RESET__SHIFT       0x10
#define CP_MEC_RS64_CNTL__MEC_PIPE0_RESET_MASK         0x00010000
#define CP_MEC_RS64_CNTL__MEC_PIPE1_RESET__SHIFT       0x11
#define CP_MEC_RS64_CNTL__MEC_PIPE1_RESET_MASK         0x00020000
#define CP_MEC_RS64_CNTL__MEC_PIPE2_RESET__SHIFT       0x12
#define CP_MEC_RS64_CNTL__MEC_PIPE2_RESET_MASK         0x00040000
#define CP_MEC_RS64_CNTL__MEC_PIPE3_RESET__SHIFT       0x13
#define CP_MEC_RS64_CNTL__MEC_PIPE3_RESET_MASK         0x00080000
#define CP_MEC_RS64_CNTL__MEC_PIPE0_ACTIVE__SHIFT      0x1a
#define CP_MEC_RS64_CNTL__MEC_PIPE0_ACTIVE_MASK        0x04000000
#define CP_MEC_RS64_CNTL__MEC_PIPE1_ACTIVE__SHIFT      0x1b
#define CP_MEC_RS64_CNTL__MEC_PIPE1_ACTIVE_MASK        0x08000000
#define CP_MEC_RS64_CNTL__MEC_PIPE2_ACTIVE__SHIFT      0x1c
#define CP_MEC_RS64_CNTL__MEC_PIPE2_ACTIVE_MASK        0x10000000
#define CP_MEC_RS64_CNTL__MEC_PIPE3_ACTIVE__SHIFT      0x1d
#define CP_MEC_RS64_CNTL__MEC_PIPE3_ACTIVE_MASK        0x20000000
#define CP_MEC_RS64_CNTL__MEC_HALT__SHIFT              0x1e
#define CP_MEC_RS64_CNTL__MEC_HALT_MASK                0x40000000

// CP_RB0_CNTL fields — gfx_v12_0.c:2735 writes RB_BUFSZ and RB_BLKSZ.
#define CP_RB0_CNTL__RB_BUFSZ__SHIFT 0x0
#define CP_RB0_CNTL__RB_BUFSZ_MASK   0x0000003F
#define CP_RB0_CNTL__RB_BLKSZ__SHIFT 0x8
#define CP_RB0_CNTL__RB_BLKSZ_MASK   0x00003F00

// CP_RB_DOORBELL_CONTROL field shifts (upstream gfx_v12_0.c uses
// REG_SET_FIELD on these). DOORBELL_OFFSET[27:2], DOORBELL_EN[30].
#define CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT 0x2
#define CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK   0x0FFFFFFC
#define CP_RB_DOORBELL_CONTROL__DOORBELL_EN__SHIFT     0x1e
#define CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK       0x40000000

// CP_RB_DOORBELL_RANGE_LOWER.DOORBELL_RANGE_LOWER + RANGE_UPPER mask.
#define CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER__SHIFT 0x2
#define CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER_MASK   0x0FFFFFFC
#define CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER_MASK   0x0FFFFFFC

// CP_RB_RPTR_ADDR_HI bit mask (upstream gfx_v12_0.c:2748 uses the
// `RB_RPTR_ADDR_HI` field mask to keep only the low 16 bits).
#define CP_RB_RPTR_ADDR_HI__RB_RPTR_ADDR_HI_MASK 0x0000FFFF

// 16 KiB ring, matching the GFX12 fetch alignment requirements.
constexpr uint32_t kCPRingDefaultBytes = 16 * 1024;
constexpr uint32_t kCPWBPageBytes      = 16 * 1024;   // AS page-aligned

// Write-back layout (host + GPU agree on these offsets within
// the 16 KB write-back page):
constexpr uint32_t kCPWBOffsetRptr  = 0x000;
constexpr uint32_t kCPWBOffsetWptr  = 0x040;
constexpr uint32_t kCPWBOffsetFence = 0x080;   // 8 B, qword-aligned

struct CPContext {
    bool             inited;
    CPFirmwareStart  firmware[3]; // PFP, ME, MEC (in Linux order)
    bool             firmwarePrepared; // PCs/reset/doorbell range ready; engines halted.
    bool             enginesStarted; // Async GFX/MEC enabled before KIQ bootstrap.
    uint64_t         mqd_bus; // Firmware-owned VRAM descriptor, retained until reset.
    bool             ringReady; // Hardware programmed and CP enable acknowledged.

    // CPU staging is never exposed as a GPU address. GMC owns the VRAM
    // allocations until successful reset/PCI close resets the session arena.
#ifdef __APPLE__
    IOBufferMemoryDescriptor *ring_buf;
#endif
    uint64_t          ring_bus;        // GPU VRAM address
    uint64_t          ring_vram_off;   // BAR0 byte offset
    void             *ring_cpu;        // CPU-only packet staging
    uint32_t          ring_size_dwords;
    uint32_t          ring_ptr_mask;
    uint64_t          published_wptr;
    uint64_t          wb_bus;          // GPU VRAM address
    uint64_t          wb_vram_off;
    const DeviceContext *wb_device;
    uint64_t          fence_shadow;    // cache of the last successful BAR read
    volatile uint64_t *fence_cpu;      // points to fence_shadow, never GPU backing

    // GPU-side addresses derived from wb_bus
    uint64_t  rptr_gpu_addr;
    uint64_t  wptr_gpu_addr;
    uint64_t  fence_gpu_addr;

    // Software wptr — what the host has committed but not yet kicked.
    uint64_t  wptr;
    uint32_t  fence_counter;

    // Doorbell index in DWORDs, as in Linux ring->doorbell_index.
    uint32_t  doorbell_index;
};

// Allocate visible VRAM ring/write-back and CPU-only staging. Idempotent.
kern_return_t cp_alloc_storage(DeviceContext &dev,
                               GMCContext &gmc, CPContext &cp);

// Release CPU staging only after GPU access has been stopped.
// VRAM backing is retained until the GMC arena is reset.
void cp_release_storage(CPContext &cp);

// Read GPU completion through BAR0; the callback feeds ClientSubmission.
kern_return_t cp_read_fence(const CPContext &cp, uint64_t *value);
bool cp_read_cs_fence(void *context, uint64_t *value);

// Append PM4 dwords to the ring at the current software wptr.
// Masks buffer indices, retaining a monotonic 64-bit write pointer.
// Returns 0 if there is insufficient space including commit padding.
// Does NOT kick the
// doorbell — caller does that after all packets are staged.
uint32_t cp_ring_write(CPContext &cp, const uint32_t *src,
                       uint32_t dwords);

// Build a NOP+RELEASE_MEM packet pair into the ring. Returns the
// fence value the EOP write will deposit at fence_gpu_addr; caller
// should kick the doorbell then read the VRAM fence for that value.
uint32_t cp_emit_eop_fence(CPContext &cp);

// Build a Linux kernel GFX MQD in VRAM and map it through MES KIQ.
kern_return_t cp_map_gfx_queue(DeviceContext &dev, GMCContext &gmc,
                              CPContext &cp, MESContext &mes);

// Configure PSP-loaded RS64 entry PCs and reset pipes after RLC autoload.
kern_return_t cp_configure_rs64(const DeviceContext &dev, const CPContext &cp);

// Toggle CP_ME_CNTL.{ME_HALT,PFP_HALT}. After cp_enable(true) the
// CP can fetch + execute from the GFX ring; before, the ring is
// dormant. Mirrors upstream gfx_v12_0_cp_gfx_enable
// (gfx_v12_0.c:2332) — both halts must drop together.
kern_return_t cp_enable(const DeviceContext &dev, bool enable);

// Toggle CP_MEC_RS64_CNTL — bring all 4 MEC pipes in/out of reset
// and active. Mirrors upstream gfx_v12_0_cp_compute_enable
// (gfx_v12_0.c:2778).
kern_return_t cp_compute_enable(const DeviceContext &dev, bool enable);

// Program CP_RB_DOORBELL_RANGE_{LOWER,UPPER} for GFX and
// CP_MEC_DOORBELL_RANGE_{LOWER,UPPER} for compute. Mirrors upstream
// gfx_v12_0_cp_set_doorbell_range (gfx_v12_0.c:2954).
kern_return_t cp_set_doorbell_range(const DeviceContext &dev,
                                    const CPContext &cp,
                                    uint32_t mec_first_doorbell,
                                    uint32_t mec_last_doorbell);

// Pad to the GFX fetch boundary, publish the 64-bit write pointer,
// then notify BAR2 using the queue's DWORD doorbell index.
kern_return_t cp_kick_doorbell(const DeviceContext &dev,
                               CPContext &cp);

// End-to-end test: emit NOP+RELEASE_MEM, kick doorbell, poll fence.
// Returns kIOReturnSuccess if the fence value materialised at
// the VRAM completion slot within timeout_us microseconds. Designed for the
// SubmitTestPM4 selector — sanity-checks the entire submit path
// once HQD + CP enable have run.
kern_return_t cp_submit_eop_test(const DeviceContext &dev,
                                 CPContext &cp,
                                 uint64_t timeout_us,
                                 uint32_t *outFence);

// Forward decls — defined in amdgpu_gmc.h / amdgpu_mes.h respectively.
struct MESContext;

// Kernel GFX queue NOP + RELEASE_MEM test; name retained for ABI compatibility.
// A Linux scratch-register test must pass first, then a poisoned VRAM fence
// must become expected_fence_value before its deadline. A zero output GPU
// address means the fence target was not allocated (scratch/preflight failure).
// Requires the kernel queue to have been mapped through MES KIQ.
//
// Out scalars:
//   *out_elapsed_us     — wall-clock from kick to observed fence
//   *out_fence_gpu_va   — GPU MC address of the fence dword
//   *out_observed_fence — last value read from the fence slot
kern_return_t cp_kiq_smoke_test(DeviceContext &dev,
                                CPContext &cp,
                                MESContext &mes,
                                GMCContext &gmc,
                                uint32_t expected_fence_value,
                                uint32_t timeout_us,
                                uint64_t *out_elapsed_us,
                                uint64_t *out_fence_gpu_va,
                                uint32_t *out_observed_fence);

// Stage 12 prepares firmware without starting the queue.
kern_return_t cp_prepare_firmware(DeviceContext &dev, GMCContext &gmc, CPContext &cp);
kern_return_t cp_start_engines(const DeviceContext &dev, CPContext &cp);
void cp_log_control(const DeviceContext &dev, const char *phase);

// Stage 14 resumes the queue after GFXHUB/constants and MES preparation.
kern_return_t cp_init_full(DeviceContext &dev,
                           GMCContext &gmc, CPContext &cp, MESContext &mes);

} // namespace amdgpu
