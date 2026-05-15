//
//  imu_v12_0.cpp — IMU stage handler for GFX12 (RDNA4) PSP path.
//
//  On the PSP firmware load path the IMU is driven entirely by
//  PSP + RLC. The driver never writes IMU registers; it only has
//  to confirm that PSP has been handed both IMU_I (id 68) and
//  IMU_D (id 69) firmware blobs before RLC autoload kicks off.
//
//  Audit-7 #11.
//
//  Upstream reference: imu_v12_0_setup_imu is empty on the PSP
//  load path; the GFX12 autoload chain (gfx_v12_0.c:3651) takes
//  the AMDGPU_FW_LOAD_RLC_BACKDOOR_AUTO branch on R9700 which
//  bypasses all imu_v12_0_*_microcode helpers.
//

#include <os/log.h>
#include "amdgpu_imu.h"

#define IMU_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.imu: " fmt, ##__VA_ARGS__)

namespace amdgpu {

kern_return_t
imu_init_full(const DeviceContext &dev, IMUContext &imu)
{
    (void)dev;

    if (!imu.microcode_loaded) {
        // The firmware extractor in MacAMDGPU.cpp must call
        // psp_load_ip_fw twice (IMU_I, IMU_D) and then set
        // imu.microcode_loaded = true. If it hasn't run, IMUInit
        // returns NotReady so the orchestrator can retry once the
        // firmware list is loaded.
        IMU_LOG("init_full: IMU microcode not yet handed to PSP "
                "(IMU_I/IMU_D LOAD_IP_FW not acked) — deferring");
        return kIOReturnNotReady;
    }

    imu.inited = true;

    // On PSP path imu_v12_0_setup_imu is a no-op — RLC autoload
    // takes care of bringing IMU online. We just log version info
    // (if the firmware extractor stashed it) and return.
    IMU_LOG("init_full: IMU microcode handed to PSP (version=%#x). "
            "RLC autoload will bring IMU online — no driver MMIO needed.",
            imu.imu_fw_version);
    return kIOReturnSuccess;
}

} // namespace amdgpu
