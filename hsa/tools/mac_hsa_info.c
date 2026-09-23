#include "mac_hsa.h"
#include <inttypes.h>
#include <stdio.h>

static unsigned gpu_count;
static hsa_status_t inspect(hsa_agent_t agent, void *unused) {
    (void)unused;
    char name[64];
    hsa_device_type_t type;
    uint32_t features;
    hsa_status_t status = hsa_agent_get_info(agent, HSA_AGENT_INFO_NAME, name);
    if (status != HSA_STATUS_SUCCESS) return status;
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type);
    if (status != HSA_STATUS_SUCCESS) return status;
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_FEATURE, &features);
    if (status != HSA_STATUS_SUCCESS) return status;
    printf("%s: %s, dispatch features=0x%x\n", type == HSA_DEVICE_TYPE_GPU ? "GPU" : "CPU",
           name, features);
    if (type == HSA_DEVICE_TYPE_GPU) {
        mac_hsa_device_info_t info;
        status = mac_hsa_agent_get_driver_info(agent, &info, sizeof(info));
        if (status != HSA_STATUS_SUCCESS) return status;
        ++gpu_count;
        printf("  registry=0x%" PRIx64 " driver=%" PRIu64 " stage=%" PRIu64
               " GFX=%u.%u.%u\n", info.registry_id, info.driver_build, info.bringup_stage,
               info.gfx_major, info.gfx_minor, info.gfx_revision);
        printf("  VRAM visible=%" PRIu64 " total=%" PRIu64 " bytes\n",
               info.visible_vram_bytes, info.total_vram_bytes);
    }
    return HSA_STATUS_SUCCESS;
}

int main(void) {
    const hsa_status_t init = hsa_init();
    if (init != HSA_STATUS_SUCCESS) {
        fprintf(stderr, "HSA observer initialization failed: 0x%x\n", init);
        return 1;
    }
    const hsa_status_t status = hsa_iterate_agents(inspect, NULL);
    const hsa_status_t shutdown = hsa_shut_down();
    if (status != HSA_STATUS_SUCCESS || shutdown != HSA_STATUS_SUCCESS) {
        fprintf(stderr, "HSA discovery failed: 0x%x (shutdown 0x%x)\n", status, shutdown);
        return 1;
    }
    printf("Discovered %u MacAMDGPU device(s). Discovery only; hardware HSA queue dispatch is unavailable.\n",
           gpu_count);
    return 0;
}
