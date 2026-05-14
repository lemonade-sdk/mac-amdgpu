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
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>

#include "amdgpu_mes.h"
#include "amdgpu_psp.h"

#define MES_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.mes: " fmt, ##__VA_ARGS__)

namespace amdgpu {

// Local DMA helper — same shape as the SDMA/CP versions.
static kern_return_t
mes_alloc_dma_block(DeviceContext &dev, uint64_t size,
                    IOBufferMemoryDescriptor **outBuf,
                    IODMACommand             **outDma,
                    uint64_t *outBus, void **outCpu)
{
    *outBuf = nullptr; *outDma = nullptr; *outBus = 0; *outCpu = nullptr;
    IOBufferMemoryDescriptor *buf = nullptr;
    kern_return_t r = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, size, kASPageSize, &buf);
    if (r != kIOReturnSuccess || buf == nullptr) {
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    buf->SetLength(size);
    IODMACommandSpecification spec = {};
    spec.options = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 64;
    IODMACommand *dma = nullptr;
    r = IODMACommand::Create(dev.pci, kIODMACommandCreateNoOptions,
                             &spec, &dma);
    if (r != kIOReturnSuccess || dma == nullptr) {
        buf->release();
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    uint64_t flags = 0;
    uint32_t segCount = 1;
    IOAddressSegment seg = {};
    r = dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, buf, 0,
                           size, &flags, &segCount, &seg);
    if (r != kIOReturnSuccess || segCount != 1) {
        dma->release(); buf->release();
        return r != kIOReturnSuccess ? r : kIOReturnNotAligned;
    }
    IOAddressSegment cpu = {};
    buf->GetAddressRange(&cpu);
    *outBuf = buf;
    *outDma = dma;
    *outBus = seg.address;
    *outCpu = reinterpret_cast<void *>(cpu.address);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_alloc_storage — EOP + MQD + ring + cmd buffer.
//------------------------------------------------------------------
kern_return_t
mes_alloc_storage(DeviceContext &dev, MESInstance &inst)
{
    if (inst.inited) return kIOReturnSuccess;
    void *cpu = nullptr;

    kern_return_t r = mes_alloc_dma_block(dev, kMES_EOP_SIZE,
                                          &inst.eop_buf, &inst.eop_dma,
                                          &inst.eop_bus, &cpu);
    if (r != kIOReturnSuccess) {
        MES_LOG("EOP alloc failed: %#x", r);
        return r;
    }
    memset(cpu, 0, kMES_EOP_SIZE);
    inst.eop_cpu = cpu;

    r = mes_alloc_dma_block(dev, kMES_MQD_SIZE,
                            &inst.mqd_buf, &inst.mqd_dma,
                            &inst.mqd_bus, &cpu);
    if (r != kIOReturnSuccess) {
        MES_LOG("MQD alloc failed: %#x", r);
        return r;
    }
    memset(cpu, 0, kMES_MQD_SIZE);
    inst.mqd_cpu = cpu;

    r = mes_alloc_dma_block(dev, kMES_RING_SIZE,
                            &inst.ring_buf, &inst.ring_dma,
                            &inst.ring_bus, &cpu);
    if (r != kIOReturnSuccess) {
        MES_LOG("ring alloc failed: %#x", r);
        return r;
    }
    memset(cpu, 0, kMES_RING_SIZE);
    inst.ring_cpu = cpu;

    r = mes_alloc_dma_block(dev, kMES_CMD_BUF_SIZE,
                            &inst.cmd_buf, &inst.cmd_dma,
                            &inst.cmd_bus, &cpu);
    if (r != kIOReturnSuccess) {
        MES_LOG("cmd buf alloc failed: %#x", r);
        return r;
    }
    memset(cpu, 0, kMES_CMD_BUF_SIZE);
    inst.cmd_cpu = cpu;

    // Write-back page — rptr/wptr shadows for the SCHED ring.
    r = mes_alloc_dma_block(dev, kASPageSize,
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
    // Doorbell index: Sched pipe gets slot 0x20, KIQ pipe 0x22.
    // First-PM4 picks arbitrary slots that don't collide with
    // CP doorbell 0 (used by cp_v12_0) or SDMA 0x10/0x12.
    inst.doorbell_index = 0x20;

    inst.inited = true;
    MES_LOG("storage: EOP %#llx, MQD %#llx, ring %#llx, cmd %#llx",
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
    if (p >= kMaxMESPipes) return kIOReturnBadArgument;
    mes.pipe[p].uc_start_addr = addr;
    if (pipe == MESPipe::Sched) mes.sched_ucode_loaded = true;
    if (pipe == MESPipe::KIQ)   mes.kiq_ucode_loaded   = true;
    MES_LOG("pipe %u uc_start_addr = %#llx",
            p, (unsigned long long)addr);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_enable — port of mes_v12_0_enable. For our uni_mes path we
// only program pipe 0; the non-uni path is deferred until we need
// the KIQ pipe.
//------------------------------------------------------------------
kern_return_t
mes_enable(const DeviceContext &dev, MESContext &mes, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        MES_LOG("enable: GC IP base not resolved");
        return kIOReturnNotReady;
    }

    const uint32_t cnt_reg =
        SOC15_REG_OFFSET(dev, IPBlock::GC, MESRegs::CP_MES_CNTL);

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

    if (mes.pipe[0].uc_start_addr == 0) {
        MES_LOG("enable: pipe 0 uc_start_addr not set "
                "(load MES microcode via LoadFirmware first)");
        return kIOReturnNotReady;
    }

    // GRBM select MES pipe 0 (me=3, pipe=0, queue=0, vmid=0).
    grbm_select(dev, /*me=*/3, /*pipe=*/0, /*queue=*/0, /*vmid=*/0);

    // Pre-reset: pulse PIPE0_RESET.
    {
        uint32_t v = RREG32(dev, cnt_reg);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE0_RESET, 1);
        WREG32(dev, cnt_reg, v);
    }

    // Program ucode start address (shift right 2 per upstream).
    const uint64_t ucode_addr = mes.pipe[0].uc_start_addr >> 2;
    WREG32(dev,
           SOC15_REG_OFFSET(dev, IPBlock::GC,
                            MESRegs::CP_MES_PRGRM_CNTR_START),
           static_cast<uint32_t>(ucode_addr));
    WREG32(dev,
           SOC15_REG_OFFSET(dev, IPBlock::GC,
                            MESRegs::CP_MES_PRGRM_CNTR_START_HI),
           static_cast<uint32_t>(ucode_addr >> 32));

    // Activate pipe 0 (start from cleared CP_MES_CNTL — matches
    // upstream which builds the activate value from 0).
    {
        uint32_t v = 0;
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE0_ACTIVE, 1);
        WREG32(dev, cnt_reg, v);
    }

    // GRBM deselect.
    grbm_select(dev, 0, 0, 0, 0);

    mes.pipe[0].enabled = true;
    MES_LOG("enable: pipe 0 active, uc_start=%#llx",
            (unsigned long long)mes.pipe[0].uc_start_addr);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// v12_compute_mqd field offsets (in dwords) — from upstream
// include/v12_structs.h. We only mirror the fields written by
// mes_v12_0_mqd_init, plus a couple of constants. Anything we
// don't touch stays 0 (the MQD page was memset to 0 at alloc).
//------------------------------------------------------------------
namespace MQDOff {
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
// mes_queue_init — port of mes_v12_0_mqd_init + queue_init_register
// fused into one pass. Computes the HQD field values from the
// MESInstance addresses, writes them to the MQD struct in memory
// at the upstream byte offsets, then GRBM-selects MES pipe 0 and
// writes the same values to the live CP_HQD_* / CP_MQD_* registers.
//------------------------------------------------------------------
kern_return_t
mes_queue_init(const DeviceContext &dev, MESContext &mes, MESPipe pipe)
{
    const uint32_t p = static_cast<uint32_t>(pipe);
    if (p >= kMaxMESPipes) return kIOReturnBadArgument;
    MESInstance &inst = mes.pipe[p];
    if (!inst.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

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
    uint32_t pq_ctrl = kCP_HQD_PQ_CONTROL_DEFAULT;
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, QUEUE_SIZE,
                            order_base_2_u32(inst.ring_size_dwords) - 1);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE,
                            (order_base_2_u32(4096u / 4u) - 1) << 8);
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

    uint32_t mqd_ctrl = 0;
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
    if (mqd != nullptr) {
        // Header magic from upstream mqd_init.
        mqd[80]                                  = 0xC0310800u;  // header
        // compute_pipelinestat_enable (idx 81) + compute_static_thread_mgmt
        // (82..85) + compute_misc_reserved (86): not strictly needed
        // for a kernel-mode queue but set to upstream defaults to
        // keep MES happy on context save.
        mqd[81] = 0x00000001u;
        mqd[82] = 0xffffffffu;
        mqd[83] = 0xffffffffu;
        mqd[84] = 0xffffffffu;
        mqd[85] = 0xffffffffu;
        mqd[86] = 0x00000007u;

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

    // ---- 3) GRBM-select MES pipe, write the same values live ----
    grbm_select(dev, /*me=*/3, /*pipe=*/p, /*queue=*/0, /*vmid=*/0);

    auto reg = [&](uint32_t r) { return SOC15_REG_OFFSET(dev, IPBlock::GC, r); };

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
// mes_init_full — MESInit bringup stage.
//------------------------------------------------------------------
kern_return_t
mes_init_full(DeviceContext &dev, PSPContext &psp,
              GMCContext &gmc, MESContext &mes)
{
    (void)psp;
    (void)gmc;

    if (!dev.ip.isResolved(IPBlock::GC)) {
        MES_LOG("init_full: GC IP base not resolved");
        return kIOReturnNotReady;
    }

    // Always allocate storage. mes_enable runs only when microcode
    // has been loaded for the SCHED pipe.
    kern_return_t r = mes_alloc_storage(dev, mes.pipe[0]);
    if (r != kIOReturnSuccess) return r;
    if (!kEnableUniMES) {
        r = mes_alloc_storage(dev, mes.pipe[1]);
        if (r != kIOReturnSuccess) return r;
    }
    mes.uni_mes_active = kEnableUniMES;

    if (!mes.sched_ucode_loaded) {
        MES_LOG("init_full: storage allocated; awaiting MES microcode "
                "via LoadFirmware before enabling pipe 0");
        return kIOReturnSuccess;
    }

    // Program the SCHED ring HQD before activating MES so that the
    // scheduler sees a valid queue on first run.
    r = mes_queue_init(dev, mes, MESPipe::Sched);
    if (r != kIOReturnSuccess) return r;

    return mes_enable(dev, mes, true);
}

} // namespace amdgpu
