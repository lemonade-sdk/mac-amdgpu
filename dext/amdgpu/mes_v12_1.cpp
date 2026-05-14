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
    return mes_enable(dev, mes, true);
}

} // namespace amdgpu
