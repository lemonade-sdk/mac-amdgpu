//
//  mes_v12_1.cpp — MES v12_1 storage allocation + CP_MES_CNTL enable.
//
//  Sources:
//      drivers/gpu/drm/amd/amdgpu/mes_v12_0.c
//          (mes_v12_0_allocate_eop_buf, mes_v12_0_enable,
//           mes_v12_0_set_ucode_start_addr)
//

#include <os/log.h>
#include <string.h>
#include <time.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>

#include "amdgpu_mes.h"
#include "amdgpu_mes_resources.h"
#include "amdgpu_vram_io.h"
#include "amdgpu_gmc.h"
#include "amdgpu_psp.h"

#define MES_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.mes: " fmt, ##__VA_ARGS__)

namespace amdgpu {

static uint32_t mes_reg(const DeviceContext &dev, MESRegs::Register reg)
{
    return SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, reg.baseIndex, reg.offset);
}

// Allocate visible VRAM plus a CPU staging buffer. The legacy *_bus fields
// below now contain GPU VRAM addresses; no IODMACommand mapping is created.
// VRAM is retained until the session reset/PCI-close cleanup resets GMC's
// allocator, including partial allocations after an initialization failure.
static kern_return_t
mes_alloc_vram_block(DeviceContext &dev, GMCContext &gmc, uint64_t size,
                     IOBufferMemoryDescriptor **outBuf,
                     IODMACommand **outDma, uint64_t *outBus, void **outCpu)
{
    *outBuf = nullptr; *outDma = nullptr; *outBus = 0; *outCpu = nullptr;
    if (!gmc.vram_alloc.is_inited()) return kIOReturnNotReady;
    VRAMAllocation allocation{};
    if (!gmc.vram_alloc.alloc(size, kASPageSize, &allocation))
        return kIOReturnNoMemory;
    IOBufferMemoryDescriptor *buf = nullptr;
    kern_return_t r = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, allocation.size, kASPageSize, &buf);
    if (r != kIOReturnSuccess || !buf) {
        gmc.vram_alloc.free(allocation);
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    buf->SetLength(allocation.size);
    IOAddressSegment cpu{};
    buf->GetAddressRange(&cpu);
    if (!cpu.address || cpu.length < allocation.size) {
        buf->release();
        gmc.vram_alloc.free(allocation);
        return kIOReturnNoMemory;
    }
    void *staging = reinterpret_cast<void *>(cpu.address);
    memset(staging, 0, allocation.size);
    r = vram_write_verified(dev, allocation.gpu_va - gmc.vram_start,
                       staging, allocation.size);
    if (r != kIOReturnSuccess) {
        buf->release();
        gmc.vram_alloc.free(allocation);
        return r;
    }
    *outBuf = buf;
    *outBus = allocation.gpu_va;
    *outCpu = staging;
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_alloc_storage — EOP + MQD + ring + cmd buffer.
//------------------------------------------------------------------
kern_return_t
mes_alloc_storage(DeviceContext &dev, MESInstance &inst, GMCContext &gmc, MESPipe pipe)
{
    if (inst.inited) return kIOReturnSuccess;
    inst.vram_base = gmc.vram_start;
    void *cpu = nullptr;

    kern_return_t r = mes_alloc_vram_block(dev, gmc, kMES_EOP_SIZE,
                                          &inst.eop_buf, &inst.eop_dma,
                                          &inst.eop_bus, &cpu);
    if (r != kIOReturnSuccess) {
        MES_LOG("EOP alloc failed: %#x", r);
        return r;
    }
    memset(cpu, 0, kMES_EOP_SIZE);
    inst.eop_cpu = cpu;

    r = mes_alloc_vram_block(dev, gmc, kMES_MQD_SIZE,
                            &inst.mqd_buf, &inst.mqd_dma,
                            &inst.mqd_bus, &cpu);
    if (r != kIOReturnSuccess) {
        MES_LOG("MQD alloc failed: %#x", r);
        return r;
    }
    memset(cpu, 0, kMES_MQD_SIZE);
    inst.mqd_cpu = cpu;

    r = mes_alloc_vram_block(dev, gmc, kMES_RING_SIZE,
                            &inst.ring_buf, &inst.ring_dma,
                            &inst.ring_bus, &cpu);
    if (r != kIOReturnSuccess) {
        MES_LOG("ring alloc failed: %#x", r);
        return r;
    }
    memset(cpu, 0, kMES_RING_SIZE);
    inst.ring_cpu = cpu;

    r = mes_alloc_vram_block(dev, gmc, kMES_CMD_BUF_SIZE,
                            &inst.cmd_buf, &inst.cmd_dma,
                            &inst.cmd_bus, &cpu);
    if (r != kIOReturnSuccess) {
        MES_LOG("cmd buf alloc failed: %#x", r);
        return r;
    }
    memset(cpu, 0, kMES_CMD_BUF_SIZE);
    inst.cmd_cpu = cpu;

    // Write-back page — rptr/wptr shadows for the SCHED ring.
    r = mes_alloc_vram_block(dev, gmc, kASPageSize,
                            &inst.wb_buf, &inst.wb_dma,
                            &inst.wb_bus, &cpu);
    if (r != kIOReturnSuccess) {
        MES_LOG("wb alloc failed: %#x", r);
        return r;
    }
    memset(cpu, 0, kASPageSize);
    inst.wb_cpu             = cpu;
    inst.ring_rptr_gpu_addr = inst.wb_bus + 0x00;
    inst.ring_wptr_gpu_addr = inst.wb_bus + 0x40;
    inst.ring_size_dwords   = kMES_RING_SIZE / 4;
    // ASIC map uses qword slots; HQD and WDOORBELL64 use dword indices.
    inst.doorbell_index = (dev.doorbell.index.mes_ring0 + static_cast<uint32_t>(pipe)) << 1;

    inst.inited = true;
    MES_LOG("VRAM storage: EOP %#llx, MQD %#llx, ring %#llx, cmd %#llx",
            (unsigned long long)inst.eop_bus,
            (unsigned long long)inst.mqd_bus,
            (unsigned long long)inst.ring_bus,
            (unsigned long long)inst.cmd_bus);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_set_uc_start_addr — call from LoadFirmware after fw bytes
// have been handed to PSP. We parse the firmware header here.
//------------------------------------------------------------------
kern_return_t
mes_set_uc_start_addr(MESContext &mes, MESPipe pipe, uint64_t addr)
{
    const uint32_t p = static_cast<uint32_t>(pipe);
    if (p >= kMaxMESPipes || !addr || (addr & 3)) return kIOReturnBadArgument;
    mes.pipe[p].uc_start_addr = addr;
    if (pipe == MESPipe::Sched) mes.sched_ucode_loaded = true;
    if (pipe == MESPipe::KIQ)   mes.kiq_ucode_loaded   = true;
    MES_LOG("pipe %u uc_start_addr = %#llx",
            p, (unsigned long long)addr);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_enable — port of mes_v12_0_enable for both uni-MES pipes.
//------------------------------------------------------------------
kern_return_t
mes_enable(const DeviceContext &dev, MESContext &mes, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::GC) || !dev.ip.isResolved(IPBlock::GC, 1)) {
        MES_LOG("enable: GC IP base not resolved");
        return kIOReturnNotReady;
    }

    const uint32_t cnt_reg =
        mes_reg(dev, MESRegs::CP_MES_CNTL);

    if (!enable) {
        // Halt + reset + invalidate. Same write sequence as the
        // !enable branch of mes_v12_0_enable.
        uint32_t v = RREG32(dev, cnt_reg);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE0_ACTIVE, 0);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE1_ACTIVE, 0);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_INVALIDATE_ICACHE, 1);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE0_RESET, 1);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE1_RESET, 1);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_HALT, 1);
        WREG32(dev, cnt_reg, v);
        mes.pipe[0].enabled = false;
        mes.pipe[1].enabled = false;
        return kIOReturnSuccess;
    }

    // Uni-MES uses the same image in two distinct microengine pipes.
    // Validate both before performing any reset/enable writes.
    for (const auto &inst : mes.pipe) {
        if (!inst.inited || inst.uc_start_addr == 0) return kIOReturnNotReady;
    }
    for (uint32_t pipe = 0; pipe < kMaxMESPipes; ++pipe) {
        grbm_select(dev, 3, pipe, 0, 0);
        uint32_t v = RREG32(dev, cnt_reg);
        if (pipe == 0) v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE0_RESET, 1);
        else           v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE1_RESET, 1);
        WREG32(dev, cnt_reg, v);
        const uint64_t addr = mes.pipe[pipe].uc_start_addr >> 2;
        WREG32(dev, mes_reg(dev, MESRegs::CP_MES_PRGRM_CNTR_START), uint32_t(addr));
        WREG32(dev, mes_reg(dev, MESRegs::CP_MES_PRGRM_CNTR_START_HI), uint32_t(addr >> 32));
        v = REG_SET_FIELD(0, CP_MES_CNTL, MES_PIPE0_ACTIVE, 1);
        if (pipe) v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE1_ACTIVE, 1);
        WREG32(dev, cnt_reg, v);
        mes.pipe[pipe].enabled = true;
    }
    grbm_select(dev, 0, 0, 0, 0);
    // Linux waits 500 us for uni-MES; DriverKit sleeps in milliseconds.
    IOSleep(1);
    MES_LOG("enable: SCHED and KIQ active");
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_set_hw_resources_1 — port of mes_v12_0_set_hw_resources_1
// (mes_v12_0.c:711).
//
// Audit-7 #6. Sent after SET_HW_RESOURCES, gated on
// sched_version >= 0x4b. Lazy-allocates the cleaner-shader fence
// buffer.
//------------------------------------------------------------------
kern_return_t
mes_set_hw_resources_1(DeviceContext &dev, MESContext &mes, GMCContext &gmc, MESPipe pipe)
{
    const uint32_t p = static_cast<uint32_t>(pipe);
    if (p >= kMaxMESPipes) return kIOReturnBadArgument;
    auto &inst = mes.pipe[p];
    if (!inst.inited || !inst.enabled) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC) || !dev.ip.isResolved(IPBlock::GC, 1)) return kIOReturnNotReady;

    // Lazy-allocate the cleaner-shader fence in visible VRAM.
    if (inst.resource_1_bus == 0) {
        void *cpu = nullptr;
        kern_return_t r = mes_alloc_vram_block(dev, gmc, kMES_Resource1Bytes,
                                              &inst.resource_1_buf,
                                              &inst.resource_1_dma,
                                              &inst.resource_1_bus, &cpu);
        if (r != kIOReturnSuccess) return r;
        memset(cpu, 0, kMES_Resource1Bytes);
    }

    // Field offsets match MESAPI_SET_HW_RESOURCES_1 in the Linux ABI.
    MES_SetHwResources1 pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.u32All = mes_api_header(kMES_API_TYPE_SCHEDULER,
                                       MESSchOp::SET_HW_RSRC_1,
                                       kMES_API_FRAME_DWORDS);
    pkt.mes_kiq_unmap_timeout = 0xa;  // mes_v12_0.c:720
    pkt.cleaner_shader_fence_mc_addr = inst.resource_1_bus;

    const uint32_t api_status_dw =
        offsetof(MES_SetHwResources1, api_status) / 4;
    return mes_submit_pkt(dev, mes, pipe,
                          reinterpret_cast<const uint32_t *>(&pkt),
                          api_status_dw,
                          /*timeout_us=*/2'000'000);
}

//------------------------------------------------------------------
// v12_compute_mqd field offsets (in dwords) — from upstream
// include/v12_structs.h. We only mirror the fields written by
// mes_v12_0_mqd_init, plus a couple of constants. Anything we
// don't touch stays 0 (the MQD page was memset to 0 at alloc).
//------------------------------------------------------------------
namespace MQDOff {
    constexpr uint32_t header                         = 0;
    constexpr uint32_t compute_pipelinestat_enable    = 11;
    constexpr uint32_t compute_static_thread_mgmt_se0 = 23;
    constexpr uint32_t compute_static_thread_mgmt_se1 = 24;
    constexpr uint32_t compute_static_thread_mgmt_se2 = 26;
    constexpr uint32_t compute_static_thread_mgmt_se3 = 27;
    constexpr uint32_t compute_misc_reserved         = 32;
    constexpr uint32_t cp_hqd_quantum                = 135;
    constexpr uint32_t cp_hqd_ib_control             = 149;
    constexpr uint32_t cp_hqd_iq_timer               = 150;
    constexpr uint32_t cp_mqd_base_addr_lo             = 128;
    constexpr uint32_t cp_mqd_base_addr_hi             = 129;
    constexpr uint32_t cp_hqd_active                   = 130;
    constexpr uint32_t cp_hqd_vmid                     = 131;
    constexpr uint32_t cp_hqd_persistent_state         = 132;
    constexpr uint32_t cp_hqd_pq_base_lo               = 136;
    constexpr uint32_t cp_hqd_pq_base_hi               = 137;
    constexpr uint32_t cp_hqd_pq_rptr_report_addr_lo   = 139;
    constexpr uint32_t cp_hqd_pq_rptr_report_addr_hi   = 140;
    constexpr uint32_t cp_hqd_pq_wptr_poll_addr_lo     = 141;
    constexpr uint32_t cp_hqd_pq_wptr_poll_addr_hi     = 142;
    constexpr uint32_t cp_hqd_pq_doorbell_control      = 143;
    constexpr uint32_t cp_hqd_pq_control               = 145;
    constexpr uint32_t cp_mqd_control                  = 162;
    constexpr uint32_t cp_hqd_eop_base_addr_lo         = 165;
    constexpr uint32_t cp_hqd_eop_base_addr_hi         = 166;
    constexpr uint32_t cp_hqd_eop_control              = 167;
    constexpr uint32_t cp_hqd_pq_wptr_lo               = 182;
    constexpr uint32_t cp_hqd_pq_wptr_hi               = 183;
    constexpr uint32_t reserved_184                    = 184;  // unmapped doorbell
}

static inline uint32_t order_base_2_u32(uint32_t x)
{
    uint32_t r = 0;
    while ((1u << r) < x) r++;
    return r;
}

//------------------------------------------------------------------
// mes_queue_init — build the MQD for either pipe. KIQ uses direct HQD
// writes; SCHED is mapped by a completed ADD_QUEUE command on KIQ.
//------------------------------------------------------------------
kern_return_t
mes_queue_init(const DeviceContext &dev, MESContext &mes, MESPipe pipe)
{
    const uint32_t p = static_cast<uint32_t>(pipe);
    if (p >= kMaxMESPipes) return kIOReturnBadArgument;
    MESInstance &inst = mes.pipe[p];
    if (!inst.inited || !inst.enabled) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC) || !dev.ip.isResolved(IPBlock::GC, 1)) return kIOReturnNotReady;

    // ---- 1) Compute the MQD field values upstream writes ----
    const uint64_t mqd_addr = inst.mqd_bus;
    const uint32_t cp_mqd_base_lo = static_cast<uint32_t>(mqd_addr) & 0xfffffffcu;
    const uint32_t cp_mqd_base_hi = static_cast<uint32_t>(mqd_addr >> 32);

    const uint64_t hqd_addr  = inst.ring_bus >> 8;
    const uint32_t cp_pq_lo  = static_cast<uint32_t>(hqd_addr);
    const uint32_t cp_pq_hi  = static_cast<uint32_t>(hqd_addr >> 32);

    const uint64_t rptr_addr = inst.ring_rptr_gpu_addr;
    const uint32_t cp_rptr_addr_lo = static_cast<uint32_t>(rptr_addr) & 0xfffffffcu;
    const uint32_t cp_rptr_addr_hi = static_cast<uint32_t>(rptr_addr >> 32) & 0xffffu;

    const uint64_t wptr_addr = inst.ring_wptr_gpu_addr;
    const uint32_t cp_wptr_addr_lo = static_cast<uint32_t>(wptr_addr) & 0xfffffff8u;
    const uint32_t cp_wptr_addr_hi = static_cast<uint32_t>(wptr_addr >> 32) & 0xffffu;

    // cp_hqd_pq_control — set QUEUE_SIZE, RPTR_BLOCK_SIZE, the
    // dispatch + queue flags. AMDGPU_GPU_PAGE_SIZE = 4096.
    // Mirrors mes_v12_0_mqd_init (mes_v12_0.c:1327-1336). The
    // raw value passed to REG_SET_FIELD is the FIELD VALUE (not
    // pre-shifted); REG_SET_FIELD applies the shift internally.
    uint32_t pq_ctrl = kCP_HQD_PQ_CONTROL_DEFAULT;
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, QUEUE_SIZE,
                            order_base_2_u32(inst.ring_size_dwords) - 1);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE,
                            order_base_2_u32(4096u / 4u) - 1);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, UNORD_DISPATCH,  1);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, 0);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, PRIV_STATE,      1);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, KMD_QUEUE,       1);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, NO_UPDATE_RPTR,  1);

    uint32_t db_ctrl = 0;
    db_ctrl = REG_SET_FIELD(db_ctrl, CP_HQD_PQ_DOORBELL_CONTROL,
                            DOORBELL_OFFSET, inst.doorbell_index);
    db_ctrl = REG_SET_FIELD(db_ctrl, CP_HQD_PQ_DOORBELL_CONTROL,
                            DOORBELL_EN, 1);

    uint32_t persist = kCP_HQD_PERSISTENT_STATE_DEFAULT;
    persist = REG_SET_FIELD(persist, CP_HQD_PERSISTENT_STATE,
                            PRELOAD_SIZE, 0x55);

    uint32_t mqd_ctrl = kCP_MQD_CONTROL_DEFAULT;
    mqd_ctrl = REG_SET_FIELD(mqd_ctrl, CP_MQD_CONTROL, VMID, 0);

    // EOP fields (upstream sets in mqd_init):
    const uint64_t eop_base_addr = inst.eop_bus >> 8;
    const uint32_t cp_eop_lo = static_cast<uint32_t>(eop_base_addr);
    const uint32_t cp_eop_hi = static_cast<uint32_t>(eop_base_addr >> 32);
    uint32_t eop_ctrl = kCP_HQD_EOP_CONTROL_DEFAULT;
    // EOP size: log2(MES_EOP_SIZE/4) - 1 = log2(512) - 1 = 8.
    // Field at bits [5:0]; default already 0x06, override to 0x08.
    eop_ctrl = (eop_ctrl & ~0x3fu)
             | ((order_base_2_u32(kMES_EOP_SIZE / 4u) - 1u) & 0x3fu);

    // ---- 2) Write the MQD struct in memory ----
    auto *mqd = static_cast<uint32_t *>(inst.mqd_cpu);
    if (!mqd) return kIOReturnNotReady;
    {
        memset(mqd, 0, kMES_MQD_SIZE);
        mqd[MQDOff::header] = 0xC0310800u;
        mqd[MQDOff::compute_pipelinestat_enable] = 1;
        mqd[MQDOff::compute_static_thread_mgmt_se0] = 0xffffffffu;
        mqd[MQDOff::compute_static_thread_mgmt_se1] = 0xffffffffu;
        mqd[MQDOff::compute_static_thread_mgmt_se2] = 0xffffffffu;
        mqd[MQDOff::compute_static_thread_mgmt_se3] = 0xffffffffu;
        mqd[MQDOff::compute_misc_reserved] = 7;
        mqd[MQDOff::cp_hqd_ib_control] = 0x00300000u;
        mqd[MQDOff::cp_hqd_iq_timer] = 0;
        mqd[MQDOff::cp_hqd_quantum] = 0;

        mqd[MQDOff::cp_mqd_base_addr_lo]         = cp_mqd_base_lo;
        mqd[MQDOff::cp_mqd_base_addr_hi]         = cp_mqd_base_hi;
        mqd[MQDOff::cp_hqd_active]               = 1;
        mqd[MQDOff::cp_hqd_vmid]                 = 0;
        mqd[MQDOff::cp_hqd_persistent_state]     = persist;
        mqd[MQDOff::cp_hqd_pq_base_lo]           = cp_pq_lo;
        mqd[MQDOff::cp_hqd_pq_base_hi]           = cp_pq_hi;
        mqd[MQDOff::cp_hqd_pq_rptr_report_addr_lo] = cp_rptr_addr_lo;
        mqd[MQDOff::cp_hqd_pq_rptr_report_addr_hi] = cp_rptr_addr_hi;
        mqd[MQDOff::cp_hqd_pq_wptr_poll_addr_lo] = cp_wptr_addr_lo;
        mqd[MQDOff::cp_hqd_pq_wptr_poll_addr_hi] = cp_wptr_addr_hi;
        mqd[MQDOff::cp_hqd_pq_doorbell_control]  = db_ctrl;
        mqd[MQDOff::cp_hqd_pq_control]           = pq_ctrl;
        mqd[MQDOff::cp_mqd_control]              = mqd_ctrl;
        mqd[MQDOff::cp_hqd_eop_base_addr_lo]     = cp_eop_lo;
        mqd[MQDOff::cp_hqd_eop_base_addr_hi]     = cp_eop_hi;
        mqd[MQDOff::cp_hqd_eop_control]          = eop_ctrl;
        mqd[MQDOff::cp_hqd_pq_wptr_lo]           = 0;
        mqd[MQDOff::cp_hqd_pq_wptr_hi]           = 0;
        // Unmapped-doorbell handling — bit 15 of reserved_184.
        mqd[MQDOff::reserved_184]                = (1u << 15);
    }

    const auto upload = vram_write_verified(dev, inst.mqd_bus - inst.vram_base,
                                        inst.mqd_cpu, kMES_MQD_SIZE);
    if (upload != kIOReturnSuccess) return upload;
    amdgpu_hdp_flush(dev);

    // The KIQ is bootstrapped through registers. It must map SCHED from
    // its uploaded MQD, exactly as mes_v12_0_queue_init does for uni-MES.
    if (pipe == MESPipe::Sched) {
        return mes_map_legacy_queue(dev, mes, kMESQueueType_SCHQ, p, 0,
            inst.doorbell_index, inst.mqd_bus, inst.ring_wptr_gpu_addr);
    }

    // ---- 3) GRBM-select MES pipe, write the same values live ----
    grbm_select(dev, /*me=*/3, /*pipe=*/p, /*queue=*/0, /*vmid=*/0);

    auto reg = [&](MESRegs::Register r) { return mes_reg(dev, r); };

    // Disable doorbell first while we reprogram.
    {
        uint32_t v = RREG32(dev, reg(MESRegs::CP_HQD_PQ_DOORBELL_CONTROL));
        v = REG_SET_FIELD(v, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 0);
        WREG32(dev, reg(MESRegs::CP_HQD_PQ_DOORBELL_CONTROL), v);
    }
    // VMID = 0.
    {
        uint32_t v = RREG32(dev, reg(MESRegs::CP_HQD_VMID));
        v = REG_SET_FIELD(v, CP_HQD_VMID, VMID, 0);
        WREG32(dev, reg(MESRegs::CP_HQD_VMID), v);
    }

    WREG32(dev, reg(MESRegs::CP_MQD_BASE_ADDR),         cp_mqd_base_lo);
    WREG32(dev, reg(MESRegs::CP_MQD_BASE_ADDR_HI),      cp_mqd_base_hi);
    // Upstream writes 0 to CP_MQD_CONTROL (not mqd_ctrl) — keep that.
    WREG32(dev, reg(MESRegs::CP_MQD_CONTROL),           0);
    WREG32(dev, reg(MESRegs::CP_HQD_PQ_BASE),           cp_pq_lo);
    WREG32(dev, reg(MESRegs::CP_HQD_PQ_BASE_HI),        cp_pq_hi);
    WREG32(dev, reg(MESRegs::CP_HQD_PQ_RPTR_REPORT_ADDR),    cp_rptr_addr_lo);
    WREG32(dev, reg(MESRegs::CP_HQD_PQ_RPTR_REPORT_ADDR_HI), cp_rptr_addr_hi);
    WREG32(dev, reg(MESRegs::CP_HQD_PQ_CONTROL),        pq_ctrl);
    WREG32(dev, reg(MESRegs::CP_HQD_PQ_WPTR_POLL_ADDR),     cp_wptr_addr_lo);
    WREG32(dev, reg(MESRegs::CP_HQD_PQ_WPTR_POLL_ADDR_HI),  cp_wptr_addr_hi);
    WREG32(dev, reg(MESRegs::CP_HQD_PQ_DOORBELL_CONTROL),   db_ctrl);
    WREG32(dev, reg(MESRegs::CP_HQD_PERSISTENT_STATE),      persist);
    WREG32(dev, reg(MESRegs::CP_HQD_ACTIVE),            1);

    grbm_select(dev, 0, 0, 0, 0);

    MES_LOG("queue_init: pipe %u, ring %#llx (%u dw), doorbell %#x, "
            "pq_ctrl=%#x db_ctrl=%#x",
            p, (unsigned long long)inst.ring_bus,
            inst.ring_size_dwords, inst.doorbell_index,
            pq_ctrl, db_ctrl);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_ring_write — write `n_dw` dwords to the SCHED ring at the
// current software wptr (wraps modulo ring size).
//------------------------------------------------------------------
static uint32_t
mes_ring_write(MESInstance &inst, const uint32_t *src, uint32_t n_dw)
{
    if (!inst.inited || !src || !inst.ring_cpu || !inst.wb_cpu || n_dw == 0)
        return 0;
    if (inst.ring_size_dwords == 0 ||
        (inst.ring_size_dwords & (inst.ring_size_dwords - 1))) return 0;
    if (n_dw > inst.ring_size_dwords) return 0;
    auto *ring = static_cast<uint32_t *>(inst.ring_cpu);
    // Track wptr inside the cmd_buf slot we never use — reuse the
    // upper part of the wb page after the rptr/wptr shadow.
    auto *wb_bytes = static_cast<volatile uint8_t *>(inst.wb_cpu);
    volatile uint64_t *sw_wptr = reinterpret_cast<volatile uint64_t *>(
        wb_bytes + 0x80);
    uint64_t wptr = *sw_wptr;
    if (wptr > UINT64_MAX - n_dw || wptr < inst.published_wptr ||
        wptr - inst.published_wptr > inst.ring_size_dwords - n_dw) return 0;
    const uint32_t mask = inst.ring_size_dwords - 1u;
    for (uint32_t i = 0; i < n_dw; i++) {
        ring[(wptr + i) & mask] = src[i];
    }
    wptr += n_dw;
    *sw_wptr = wptr;
    return n_dw;
}

//------------------------------------------------------------------
// mes_kick_doorbell — publish a monotonic dword WPTR, then WDOORBELL64.
// The ASIC map uses qword slots, but inst.doorbell_index and HQD use dwords.
//------------------------------------------------------------------
static kern_return_t
mes_kick_doorbell(const DeviceContext &dev, MESInstance &inst)
{
    if (!inst.inited) return kIOReturnNotReady;
    if (!dev.pci) return kIOReturnNotAttached;
    const uint64_t dbOffset = static_cast<uint64_t>(inst.doorbell_index) * 4;
    if (dbOffset > dev.bar2Size || 8 > dev.bar2Size - dbOffset)
        return kIOReturnBadArgument;
    auto *wb = static_cast<uint8_t *>(inst.wb_cpu);
    uint64_t wptr;
    memcpy(&wptr, wb + 0x80, sizeof(wptr));
    if (wptr < inst.published_wptr ||
        wptr - inst.published_wptr > inst.ring_size_dwords)
        return kIOReturnNoSpace;

    // Only upload newly appended words, including a wrap into the ring head.
    uint64_t cursor = inst.published_wptr;
    const auto *ring = static_cast<const uint32_t *>(inst.ring_cpu);
    while (cursor < wptr) {
        const uint32_t index = cursor & (inst.ring_size_dwords - 1);
        uint64_t count = inst.ring_size_dwords - index;
        if (count > wptr - cursor) count = wptr - cursor;
        const auto r = vram_write_verified(dev,
            inst.ring_bus - inst.vram_base + index * 4, ring + index, count * 4);
        if (r != kIOReturnSuccess) return r;
        cursor += count;
    }
    const auto r = vram_write_verified(dev, inst.wb_bus - inst.vram_base + 0x40,
                                  &wptr, sizeof(wptr));
    if (r != kIOReturnSuccess) return r;
    memcpy(wb + 0x40, &wptr, sizeof(wptr));
    amdgpu_hdp_flush(dev);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    inst.published_wptr = wptr;
    dev.pci->MemoryWrite64(dev.bar2MemIndex, dbOffset, wptr);
    return kIOReturnSuccess;
}

static void
mes_log_queue_state(const DeviceContext &dev, const MESInstance &inst, uint32_t pipe)
{
    grbm_select(dev, 3, pipe, 0, 0);
    const uint32_t active = RREG32(dev, mes_reg(dev, MESRegs::CP_HQD_ACTIVE));
    const uint32_t rptr = RREG32(dev, mes_reg(dev, MESRegs::CP_HQD_PQ_RPTR));
    const uint32_t wptrLo = RREG32(dev, mes_reg(dev, MESRegs::CP_HQD_PQ_WPTR_LO));
    const uint32_t wptrHi = RREG32(dev, mes_reg(dev, MESRegs::CP_HQD_PQ_WPTR_HI));
    const uint32_t doorbell = RREG32(dev, mes_reg(dev, MESRegs::CP_HQD_PQ_DOORBELL_CONTROL));
    const uint32_t cntl = RREG32(dev, mes_reg(dev, MESRegs::CP_MES_CNTL));
    const uint32_t pc = RREG32(dev, mes_reg(dev, MESRegs::CP_MES_INSTR_PNTR));
    grbm_select(dev, 0, 0, 0, 0);
    MES_LOG("queue snapshot: pipe=%u active=%#x RPTR=%#x WPTR=%#x:%08x "
            "published=%#llx doorbell=%#x CNTL=%#x PC=%#x",
            pipe, active, rptr, wptrHi, wptrLo,
            (unsigned long long)inst.published_wptr, doorbell, cntl, pc);
    MES_LOG("GFXHUB fault snapshot: status=%#x:%08x address=%#x:%08x",
        RREG32(dev, mes_reg(dev, MESRegs::GCVM_L2_PROTECTION_FAULT_STATUS_HI32)),
        RREG32(dev, mes_reg(dev, MESRegs::GCVM_L2_PROTECTION_FAULT_STATUS_LO32)),
        RREG32(dev, mes_reg(dev, MESRegs::GCVM_L2_PROTECTION_FAULT_ADDR_HI32)),
        RREG32(dev, mes_reg(dev, MESRegs::GCVM_L2_PROTECTION_FAULT_ADDR_LO32)));
}

//------------------------------------------------------------------
// mes_submit_pkt — write a 64-dword API frame to the ring, chain a
// QUERY_SCHEDULER_STATUS frame for fence acknowledgement, kick the
// doorbell, poll the status slot.
//------------------------------------------------------------------
kern_return_t
mes_submit_pkt(const DeviceContext &dev, MESContext &mes, MESPipe pipe,
               const uint32_t *pkt, uint32_t api_status_off_dw,
               uint64_t timeout_us)
{
    const uint32_t p = static_cast<uint32_t>(pipe);
    if (p >= kMaxMESPipes) return kIOReturnBadArgument;
    MESInstance &inst = mes.pipe[p];
    if (!inst.inited || !inst.enabled) return kIOReturnNotReady;
    if (inst.submission_pending) return kIOReturnBusy;
    if (pkt == nullptr || api_status_off_dw > kMES_API_FRAME_DWORDS - 4) {
        return kIOReturnBadArgument;
    }
    // Reserve both frames before modifying staging or completion slots. The
    // previous chained query must finish before any ring storage is reused.
    if (!inst.ring_cpu || !inst.wb_cpu ||
        inst.ring_size_dwords < 2 * kMES_API_FRAME_DWORDS ||
        (inst.ring_size_dwords & (inst.ring_size_dwords - 1)))
        return kIOReturnBadArgument;
    uint64_t staged_wptr;
    memcpy(&staged_wptr, static_cast<uint8_t *>(inst.wb_cpu) + 0x80, 8);
    if (staged_wptr != inst.published_wptr) return kIOReturnBusy;
    if (staged_wptr > UINT64_MAX - 2 * kMES_API_FRAME_DWORDS)
        return kIOReturnNoSpace;

    // Both completions reside in VRAM and are read through BAR0. Clear only
    // their slots: overwriting an entire write-back page can destroy GPU state.
    const uint64_t zero = 0;
    auto r = vram_write_verified(dev, inst.wb_bus - inst.vram_base + 0xC0, &zero, 8);
    if (r != kIOReturnSuccess) return r;
    r = vram_write_verified(dev, inst.wb_bus - inst.vram_base + 0xD0, &zero, 8);
    if (r != kIOReturnSuccess) return r;
    const uint64_t status_gpu = inst.wb_bus + 0xC0;
    const uint64_t fence_value = 1;

    // Patch the embedded MES_API_Status fence_addr / fence_value.
    uint32_t frame[kMES_API_FRAME_DWORDS];
    memcpy(frame, pkt, sizeof(frame));
    const MES_API_Status status = {status_gpu, fence_value};
    memcpy(frame + api_status_off_dw, &status, sizeof(status));

    if (mes_ring_write(inst, frame, kMES_API_FRAME_DWORDS) !=
        kMES_API_FRAME_DWORDS) {
        return kIOReturnNoSpace;
    }

    // The chained query fence establishes completion before reading API status.
    MES_QueryStatus q = {};
    q.header.u32All = mes_api_header(kMES_API_TYPE_SCHEDULER,
                                     MESSchOp::QUERY_SCHEDULER_STATUS,
                                     kMES_API_FRAME_DWORDS);
    q.api_status.fence_addr = inst.wb_bus + 0xD0;
    q.api_status.fence_value = fence_value;
    if (mes_ring_write(inst, reinterpret_cast<const uint32_t *>(&q),
                       kMES_API_FRAME_DWORDS) != kMES_API_FRAME_DWORDS) {
        return kIOReturnNoSpace;
    }

    // Retain this latch on timeout/error; only session reset permits reuse.
    inst.submission_pending = true;
    r = mes_kick_doorbell(dev, inst);
    if (r != kIOReturnSuccess) return r;

    const uint64_t start_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t elapsed_us = 0, query_status = 0, api_status = 0;
    do {
        elapsed_us = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start_ns) / 1000;
        // Linux waits for the chained query, then checks the original API.
        // The high 32 bits of API status are debug data when the low half is 0.
        r = vram_read_fence64(dev, inst.wb_bus - inst.vram_base + 0xD0,
                                &query_status);
        if (r != kIOReturnSuccess) return r;
        if (query_status == fence_value) {
            r = vram_read_fence64(dev, inst.wb_bus - inst.vram_base + 0xC0,
                                    &api_status);
            if (r != kIOReturnSuccess) return r;
            inst.submission_pending = false;
            if (static_cast<uint32_t>(api_status) != 0) {
                MES_LOG("submit_pkt: pipe %u complete after %llu us",
                        p, (unsigned long long)elapsed_us);
                return kIOReturnSuccess;
            }
            MES_LOG("submit_pkt: pipe %u API failed (status=%#llx)",
                    p, (unsigned long long)api_status);
            return kIOReturnInternalError;
        }
        if (elapsed_us >= timeout_us) break;
        IOSleep(1);
    } while (true);
    r = vram_read_fence64(dev, inst.wb_bus - inst.vram_base + 0xC0, &api_status);
    if (r != kIOReturnSuccess) return r;
    MES_LOG("submit_pkt: pipe %u timeout after %llu us (API=%#llx query=%#llx)",
            p, (unsigned long long)elapsed_us,
            (unsigned long long)api_status, (unsigned long long)query_status);
    mes_log_queue_state(dev, inst, p);
    return kIOReturnTimeout;
}

//------------------------------------------------------------------
// mes_query_sched_status — convenience wrapper. Sends a no-payload
// QUERY frame and checks MES echoes the fence.
//------------------------------------------------------------------
kern_return_t
mes_query_sched_status(const DeviceContext &dev, MESContext &mes,
                       MESPipe pipe)
{
    MES_QueryStatus pkt = {};
    pkt.header.u32All = mes_api_header(kMES_API_TYPE_SCHEDULER,
                                       MESSchOp::QUERY_SCHEDULER_STATUS,
                                       kMES_API_FRAME_DWORDS);
    return mes_submit_pkt(dev, mes, pipe,
                          reinterpret_cast<const uint32_t *>(&pkt),
                          offsetof(MES_QueryStatus, api_status) / 4, 2'000'000);
}

//------------------------------------------------------------------
// mes_set_hw_resources — port of mes_v12_0_set_hw_resources for
// the SCHED pipe. Lazy-allocates the scheduler context + status-
// fence buffers (4 KB VRAM payloads) on first call.
//------------------------------------------------------------------
kern_return_t
mes_set_hw_resources(DeviceContext &dev, MESContext &mes, GMCContext &gmc,
                     const MESSetHwResourcesInput &in, MESPipe pipe)
{
    const uint32_t p = static_cast<uint32_t>(pipe);
    if (p >= kMaxMESPipes) return kIOReturnBadArgument;
    auto &inst = mes.pipe[p];
    if (!inst.inited || !inst.enabled) return kIOReturnNotReady;
    MES_SetHwResources pkt{};
    if (!mes_set_register_bases(dev.ip, pkt)) {
        MES_LOG("set_hw_resources: required GC/MMHUB/OSSSYS register bases unresolved");
        return kIOReturnNotReady;
    }

    // 1) Lazy-alloc scheduler context + status fence.
    if (inst.sch_ctx_bus == 0) {
        void *cpu = nullptr;
        kern_return_t r = mes_alloc_vram_block(dev, gmc, kMES_SchCtxBytes,
                                              &inst.sch_ctx_buf,
                                              &inst.sch_ctx_dma,
                                              &inst.sch_ctx_bus, &cpu);
        if (r != kIOReturnSuccess) return r;
        memset(cpu, 0, kMES_SchCtxBytes);
    }
    if (inst.status_fence_bus == 0) {
        void *cpu = nullptr;
        kern_return_t r = mes_alloc_vram_block(dev, gmc, kMES_StatusFenceBytes,
                                              &inst.status_fence_buf,
                                              &inst.status_fence_dma,
                                              &inst.status_fence_bus, &cpu);
        if (r != kIOReturnSuccess) return r;
        memset(cpu, 0, kMES_StatusFenceBytes);
    }

    // 2) Build the 64-dword frame.
    pkt.header.u32All = mes_api_header(kMES_API_TYPE_SCHEDULER,
                                       MESSchOp::SET_HW_RSRC,
                                       kMES_API_FRAME_DWORDS);
    if (pipe == MESPipe::Sched) {
        pkt.vmid_mask_mmhub  = in.vmid_mask_mmhub;
        pkt.vmid_mask_gfxhub = in.vmid_mask_gfxhub;
        pkt.gds_size         = 0;
        pkt.paging_vmid      = 0;
        for (int i = 0; i < 8; i++) pkt.compute_hqd_mask[i] = in.compute_hqd_mask[i];
        for (int i = 0; i < 2; i++) pkt.gfx_hqd_mask[i]     = in.gfx_hqd_mask[i];
        for (int i = 0; i < 2; i++) pkt.sdma_hqd_mask[i]    = in.sdma_hqd_mask[i];
        for (int i = 0; i < 5; i++) pkt.aggregated_doorbells[i] = in.aggregated_doorbells[i];
    }

    pkt.g_sch_ctx_gpu_mc_ptr              = inst.sch_ctx_bus;
    pkt.query_status_fence_gpu_mc_ptr     = inst.status_fence_bus;

    // Flags match mes_v12_0_set_hw_resources (mes_v12_0.c:780-792):
    //   disable_reset = 1, disable_mes_log = 1,
    //   use_different_vmid_compute = 1, enable_reg_active_poll = 1,
    //   enable_level_process_quantum_check = 1,
    //   unmapped_doorbell_handling = 1 (basic version)
    pkt.flags = kSetHwRsrcFlag_disable_reset
              | kSetHwRsrcFlag_disable_mes_log
              | kSetHwRsrcFlag_use_different_vmid_compute
              | kSetHwRsrcFlag_enable_reg_active_poll
              | kSetHwRsrcFlag_enable_level_process_quantum_check
              | kSetHwRsrcFlag_unmapped_doorbell_handling_BASIC;

    pkt.oversubscription_timer =
        ((pipe == MESPipe::Sched ? mes.sched_version : mes.kiq_version) & kMES_VERSION_MASK) >= 0x8b ? 50 : 0;

    // 3) Submit. api_status sits at byte offsetof(MES_SetHwResources,
    //    api_status); convert to dword offset for mes_submit_pkt.
    const uint32_t api_status_dw =
        offsetof(MES_SetHwResources, api_status) / 4;
    return mes_submit_pkt(dev, mes, pipe,
                          reinterpret_cast<const uint32_t *>(&pkt),
                          api_status_dw,
                          /*timeout_us=*/2'000'000);
}

// Linux mes_v12_0_map_legacy_queue: mapping is a KIQ operation, not
// a scheduler ADD_QUEUE with process/gang context allocation.
kern_return_t
mes_map_legacy_queue(const DeviceContext &dev, MESContext &mes,
    uint32_t queueType, uint32_t pipe, uint32_t queue, uint32_t doorbell,
    uint64_t mqdAddress, uint64_t wptrAddress)
{
    if (queueType > kMESQueueType_SCHQ || pipe >= 4 || queue >= 8 ||
        (doorbell & 1) || doorbell > 0x03fffffeu ||
        !mqdAddress || (mqdAddress & 255) || !wptrAddress || (wptrAddress & 7))
        return kIOReturnBadArgument;
    if (!mes.uni_mes_active || !mes.pipe[1].enabled || !mes.pipe[1].inited ||
        !mes.pipe[1].sch_ctx_bus || !mes.pipe[1].resource_1_bus)
        return kIOReturnNotReady;
    MES_AddQueue pkt{};
    pkt.header.u32All = mes_api_header(kMES_API_TYPE_SCHEDULER,
        MESSchOp::ADD_QUEUE, kMES_API_FRAME_DWORDS);
    pkt.pipe_id = pipe;
    pkt.queue_id = queue;
    pkt.doorbell_offset = doorbell;
    pkt.mqd_addr = mqdAddress;
    pkt.wptr_addr = wptrAddress;
    pkt.queue_type = queueType;
    pkt.flags = kAddQueueFlag_map_legacy_kq;
    return mes_submit_pkt(dev, mes, MESPipe::KIQ,
        reinterpret_cast<const uint32_t *>(&pkt),
        offsetof(MES_AddQueue, api_status) / 4, 2'000'000);
}

//------------------------------------------------------------------
// mes_add_hw_queue — port of mes_v12_0_add_hw_queue.
//------------------------------------------------------------------
kern_return_t
mes_add_hw_queue(const DeviceContext &dev, MESContext &mes,
                 const MESAddQueueInput &in)
{
    if (!mes.pipe[0].inited || !mes.pipe[0].enabled) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC) || !dev.ip.isResolved(IPBlock::GC, 1)) return kIOReturnNotReady;

    MES_AddQueue pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.u32All = mes_api_header(kMES_API_TYPE_SCHEDULER,
                                       MESSchOp::ADD_QUEUE,
                                       kMES_API_FRAME_DWORDS);
    pkt.process_id              = in.process_id;
    pkt.page_table_base_addr    = in.page_table_base_addr;
    pkt.process_va_start        = 0;
    pkt.process_va_end          = 0;
    pkt.process_quantum         = 0;
    pkt.process_context_addr    = in.process_context_addr;
    pkt.gang_quantum            = 0;
    pkt.gang_context_addr       = in.gang_context_addr;
    pkt.inprocess_gang_priority = in.inprocess_gang_priority;
    pkt.gang_global_priority_level = in.gang_global_priority_level;
    pkt.doorbell_offset         = in.doorbell_offset;
    pkt.mqd_addr                = in.mqd_addr;
    pkt.wptr_addr               = in.wptr_addr;
    pkt.queue_type              = in.queue_type;
    pkt.pipe_id                 = in.pipe_id;
    pkt.queue_id                = in.queue_id;
    pkt.flags                   = in.flags;

    const uint32_t api_status_dw =
        offsetof(MES_AddQueue, api_status) / 4;
    return mes_submit_pkt(dev, mes, MESPipe::Sched,
                          reinterpret_cast<const uint32_t *>(&pkt),
                          api_status_dw,
                          /*timeout_us=*/2'000'000);
}

//------------------------------------------------------------------
// mes_kiq_setting — port of mes_v12_0_kiq_setting (mes_v12_0.c:1728).
//
// Writes RLC_CP_SCHEDULERS to identify the KIQ ring for RLC's
// IRQ-routing logic. Upstream packs:
//     value = (existing & 0xffffff00)
//           | (me << 5) | (pipe << 3) | (queue)
//           | 0x80   /* enable scheduler */
//
// For uni-MES on RDNA4 the KIQ queue is mes.ring[KIQ_PIPE] with
// me=3, pipe=1, queue=0 — but we accept any (me, pipe, queue) tuple
// so the bringup orchestrator can also call this for the legacy
// gfx.kiq[0] ring if uni_mes is ever disabled.
//
// Audit-7 #5/#6.
//------------------------------------------------------------------
kern_return_t
mes_kiq_setting(const DeviceContext &dev, uint32_t me, uint32_t pipe,
                uint32_t queue)
{
    if (!dev.ip.isResolved(IPBlock::GC) || !dev.ip.isResolved(IPBlock::GC, 1)) return kIOReturnNotReady;
    const uint32_t reg =
        mes_reg(dev, MESRegs::RLC_CP_SCHEDULERS);

    // mes_v12_0.c:1734-1737 — RMW preserving the high bytes that
    // RLC owns for its own state machine. The low byte encodes
    // (me, pipe, queue) and the 0x80 bit flips on the scheduler.
    uint32_t tmp = RREG32(dev, reg);
    tmp &= 0xffffff00u;
    tmp |= ((me & 0x7u) << 5) | ((pipe & 0x3u) << 3) | (queue & 0x7u);
    WREG32(dev, reg, tmp | 0x80u);

    MES_LOG("kiq_setting: me=%u pipe=%u queue=%u RLC_CP_SCHEDULERS=%#x",
            me, pipe, queue, tmp | 0x80u);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_enable_unmapped_doorbell_handling — port of
// mes_v12_0_enable_unmapped_doorbell_handling (mes_v12_0.c:863).
//
// Audit-7 #6.
//------------------------------------------------------------------
kern_return_t
mes_enable_unmapped_doorbell_handling(const DeviceContext &dev, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::GC) || !dev.ip.isResolved(IPBlock::GC, 1)) return kIOReturnNotReady;
    const uint32_t reg =
        mes_reg(dev, MESRegs::CP_UNMAPPED_DOORBELL);

    // mes_v12_0.c:867-880 — read-modify-write. PROC_LSB encodes the
    // bit position that selects the doorbell page; 0xd matches KFD's
    // 2-page-per-process convention.
    uint32_t data = RREG32(dev, reg);
    data &= ~CP_UNMAPPED_DOORBELL__PROC_LSB_MASK;
    data |= 0xdu << CP_UNMAPPED_DOORBELL__PROC_LSB__SHIFT;
    if (enable) data |= (1u << CP_UNMAPPED_DOORBELL__ENABLE__SHIFT);
    else        data &= ~(1u << CP_UNMAPPED_DOORBELL__ENABLE__SHIFT);
    WREG32(dev, reg, data);

    MES_LOG("unmapped_doorbell_handling %s: CP_UNMAPPED_DOORBELL=%#x",
            enable ? "enabled" : "disabled", data);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_read_sched_version — port of the inline RREG32 in
// mes_v12_0_queue_init (mes_v12_0.c:1499-1512).
//
// Sequence (must run AFTER mes_enable(true)):
//   GRBM-select MES pipe → read CP_MES_GP3_LO → store on mes
//   → GRBM-deselect.
//
// Audit-7 #6.
//------------------------------------------------------------------
kern_return_t
mes_read_sched_version(const DeviceContext &dev, MESContext &mes,
                       MESPipe pipe)
{
    const uint32_t p = static_cast<uint32_t>(pipe);
    if (p >= kMaxMESPipes) return kIOReturnBadArgument;
    if (!dev.ip.isResolved(IPBlock::GC) || !dev.ip.isResolved(IPBlock::GC, 1)) return kIOReturnNotReady;

    grbm_select(dev, /*me=*/3, /*pipe=*/p, /*queue=*/0, /*vmid=*/0);
    const uint32_t v = RREG32(dev,
        mes_reg(dev, MESRegs::CP_MES_GP3_LO));
    grbm_select(dev, 0, 0, 0, 0);

    if (pipe == MESPipe::Sched) mes.sched_version = v;
    else                        mes.kiq_version   = v;

    MES_LOG("sched_version (pipe %u) = %#x (masked = %#x)",
            p, v, v & kMES_VERSION_MASK);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_init_aggregated_doorbell — port of mes_v12_0_init_aggregated_doorbell.
// Programs CP_MES_DOORBELL_CONTROL1..5 with the 5 priority doorbells
// and sets CP_HQD_GFX_CONTROL.DB_UPDATED_MSG_EN.
//------------------------------------------------------------------
kern_return_t
mes_init_aggregated_doorbell(const DeviceContext &dev,
                             const uint32_t doorbells[5])
{
    if (!dev.ip.isResolved(IPBlock::GC) || !dev.ip.isResolved(IPBlock::GC, 1)) return kIOReturnNotReady;

    auto reg = [&](MESRegs::Register r) { return mes_reg(dev, r); };

    const MESRegs::Register ctrl_regs[5] = {
        MESRegs::CP_MES_DOORBELL_CONTROL1,
        MESRegs::CP_MES_DOORBELL_CONTROL2,
        MESRegs::CP_MES_DOORBELL_CONTROL3,
        MESRegs::CP_MES_DOORBELL_CONTROL4,
        MESRegs::CP_MES_DOORBELL_CONTROL5,
    };
    const uint32_t clear_mask =
        CP_MES_DOORBELL_CONTROL1__DOORBELL_OFFSET_MASK
      | CP_MES_DOORBELL_CONTROL1__DOORBELL_EN_MASK
      | CP_MES_DOORBELL_CONTROL1__DOORBELL_HIT_MASK;

    for (int i = 0; i < 5; i++) {
        uint32_t v = RREG32(dev, reg(ctrl_regs[i]));
        v &= ~clear_mask;
        v = REG_SET_FIELD(v, CP_MES_DOORBELL_CONTROL1,
                          DOORBELL_OFFSET, doorbells[i]);
        v = REG_SET_FIELD(v, CP_MES_DOORBELL_CONTROL1, DOORBELL_EN, 1);
        WREG32(dev, reg(ctrl_regs[i]), v);
    }

    // Final touch: gate the GFX queue update msg through to MES.
    uint32_t v = (1u << CP_HQD_GFX_CONTROL__DB_UPDATED_MSG_EN__SHIFT);
    WREG32(dev, reg(MESRegs::CP_HQD_GFX_CONTROL), v);

    MES_LOG("aggregated_doorbell: LOW=%#x NORMAL=%#x MED=%#x HIGH=%#x RT=%#x",
            doorbells[0], doorbells[1], doorbells[2], doorbells[3], doorbells[4]);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_init_full — MESInit bringup stage.
//------------------------------------------------------------------
kern_return_t
mes_init_full(DeviceContext &dev, PSPContext &psp,
              GMCContext &gmc, MESContext &mes)
{
    (void)psp;

    if (!dev.ip.isResolved(IPBlock::GC) || !dev.ip.isResolved(IPBlock::GC, 1)) {
        MES_LOG("init_full: GC IP base not resolved");
        return kIOReturnNotReady;
    }

    if (!kEnableUniMES || !mes.sched_ucode_loaded || !mes.kiq_ucode_loaded)
        return kIOReturnNotReady;
    for (uint32_t p = 0; p < kMaxMESPipes; ++p) {
        const auto r = mes_alloc_storage(dev, mes.pipe[p], gmc, static_cast<MESPipe>(p));
        if (r != kIOReturnSuccess) return r;
    }
    mes.uni_mes_active = true;
    auto r = mes_kiq_setting(dev, 3, 1, 0);
    if (r != kIOReturnSuccess) return r;
    r = mes_enable(dev, mes, true);
    if (r != kIOReturnSuccess) return r;
    MES_LOG("checkpoint: both pipes enabled");
    mes_log_queue_state(dev, mes.pipe[1], 1);

    r = mes_queue_init(dev, mes, MESPipe::KIQ);
    if (r != kIOReturnSuccess) return r;
    r = mes_read_sched_version(dev, mes, MESPipe::KIQ);
    if (r != kIOReturnSuccess) return r;
    MESSetHwResourcesInput kiqResources{};
    r = mes_set_hw_resources(dev, mes, gmc, kiqResources, MESPipe::KIQ);
    if (r != kIOReturnSuccess) return r;
    r = mes_set_hw_resources_1(dev, mes, gmc, MESPipe::KIQ);
    if (r != kIOReturnSuccess) return r;
    MES_LOG("checkpoint: KIQ resources acknowledged");
    mes_log_queue_state(dev, mes.pipe[1], 1);

    r = mes_enable_unmapped_doorbell_handling(dev, true);
    if (r != kIOReturnSuccess) return r;
    r = mes_queue_init(dev, mes, MESPipe::Sched);
    if (r != kIOReturnSuccess) return r;
    r = mes_read_sched_version(dev, mes, MESPipe::Sched);
    if (r != kIOReturnSuccess) return r;
    MES_LOG("checkpoint: KIQ mapped scheduler");
    mes_log_queue_state(dev, mes.pipe[0], 0);

    uint32_t doorbells[kMES_PriorityLevels];
    for (uint32_t i = 0; i < kMES_PriorityLevels; ++i)
        doorbells[i] = kMES_AggregatedDoorbellsBase + i;

    // (4) Tell MES which hw resources it owns. VMID 0 stays kernel-only;
    // VMIDs 1..7 are MES-scheduled compute VMIDs. We keep GFX HQD 0
    // for the direct CP_RB0 path (used by SubmitIB/SubmitTestPM4)
    // so gfx_hqd_mask[0] = 0xFE — MES owns 1..7. Compute HQDs are
    // all owned by MES; SDMA HQDs likewise.
    MESSetHwResourcesInput in{};
    in.vmid_mask_mmhub  = 0xFE;
    in.vmid_mask_gfxhub = 0xFE;
    for (int i = 0; i < 8; i++) in.compute_hqd_mask[i] = 0xFF;
    in.gfx_hqd_mask[0]  = 0xFE;
    in.gfx_hqd_mask[1]  = 0x00;
    in.sdma_hqd_mask[0] = 0x0F;
    in.sdma_hqd_mask[1] = 0x0F;
    for (uint32_t i = 0; i < kMES_PriorityLevels; i++) {
        in.aggregated_doorbells[i] = doorbells[i];
    }
    // Like Linux mes_v12_0_hw_init, do not publish a completed MES
    // stage unless the scheduler acknowledges its required resources.
    kern_return_t sr = mes_set_hw_resources(dev, mes, gmc, in, MESPipe::Sched);
    if (sr != kIOReturnSuccess) {
        MES_LOG("init_full: set_hw_resources failed (%#x) — MES enabled "
                "but scheduler not configured", sr);
        return sr;
    }

    // (5) Conditional SET_HW_RESOURCES_1. Upstream mes_v12_0.c:1859
    // gates on (sched_version & MASK) >= 0x4b.  Audit-7 #6.
    if ((mes.sched_version & kMES_VERSION_MASK) >=
            kMES_HwResources1MinSchedVersion) {
        kern_return_t r1 = mes_set_hw_resources_1(dev, mes, gmc, MESPipe::Sched);
        if (r1 != kIOReturnSuccess) {
            MES_LOG("init_full: set_hw_resources_1 failed (%#x) — stopping before reusing completion slots", r1);
            return r1;
        }
    } else {
        MES_LOG("init_full: sched_version %#x < 0x4b, skipping "
                "SET_HW_RESOURCES_1", mes.sched_version & kMES_VERSION_MASK);
    }

    MES_LOG("checkpoint: scheduler resources acknowledged");
    mes_log_queue_state(dev, mes.pipe[0], 0);
    r = mes_init_aggregated_doorbell(dev, doorbells);
    if (r != kIOReturnSuccess) return r;

    // (7) Query echoes the running scheduler — confirms MES is alive
    // before we hand it user queues. A timeout fails this stage.
    kern_return_t qr = mes_query_sched_status(dev, mes, MESPipe::Sched);
    if (qr != kIOReturnSuccess) {
        MES_LOG("init_full: query_sched_status failed (%#x) — MES may "
                "be busy or wedged", qr);
        return qr;
    }
    return kIOReturnSuccess;
}

} // namespace amdgpu
