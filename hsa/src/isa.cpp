#include "runtime_state.h"
#include <array>

using namespace mac_hsa::detail;
namespace {
hsa_status_t isaConnection(uint64_t handle, std::shared_ptr<mac_hsa::Connection> &connection) {
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        const auto agent = findAgent({handle});
        if (!agent || !agent->connection) return HSA_STATUS_ERROR_INVALID_ISA;
        connection = agent->connection;
    }
    mac_hsa::DeviceSnapshot snapshot;
    const auto status = connection->read(snapshot);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (snapshot.gfxMajor != 12 || snapshot.gfxMinor != 0 || snapshot.gfxRevision != 1)
        return HSA_STATUS_ERROR_INVALID_ISA;
    return HSA_STATUS_SUCCESS;
}
}
extern "C" {
hsa_status_t hsa_agent_iterate_isas(hsa_agent_t handle,
    hsa_status_t (*callback)(hsa_isa_t, void *), void *data) {
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        const auto agent = findAgent(handle);
        if (!agent) return HSA_STATUS_ERROR_INVALID_AGENT;
        if (!callback) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (!agent->connection) return HSA_STATUS_SUCCESS; // no CPU kernel agent
    }
    std::shared_ptr<mac_hsa::Connection> connection;
    const auto status = isaConnection(handle.handle, connection);
    if (status != HSA_STATUS_SUCCESS) return status;
    return callback({handle.handle}, data);
}
hsa_status_t hsa_isa_get_info_alt(hsa_isa_t isa, hsa_isa_info_t attribute, void *value) {
    std::shared_ptr<mac_hsa::Connection> connection;
    const auto status = isaConnection(isa.handle, connection);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (!value) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    constexpr char name[] = "amdgcn-amd-amdhsa--gfx1201";
    switch (attribute) {
    // Match the ROCr ABI used by HRX: NAME_LENGTH includes the NUL byte,
    // despite the older header prose describing the length without it.
    case HSA_ISA_INFO_NAME_LENGTH: return writeValue(value, uint32_t(sizeof(name)));
    case HSA_ISA_INFO_NAME: std::memcpy(value, name, sizeof(name)); return HSA_STATUS_SUCCESS;
    case HSA_ISA_INFO_MACHINE_MODELS: return writeValue(value, std::array<bool, 2>{false, true});
    case HSA_ISA_INFO_PROFILES: return writeValue(value, std::array<bool, 2>{true, false});
    case HSA_ISA_INFO_DEFAULT_FLOAT_ROUNDING_MODES:
    case HSA_ISA_INFO_BASE_PROFILE_DEFAULT_FLOAT_ROUNDING_MODES:
        return writeValue(value, std::array<bool, 3>{false, false, true});
    case HSA_ISA_INFO_FAST_F16_OPERATION: return writeValue(value, true);
    case HSA_ISA_INFO_WORKGROUP_MAX_DIM: return writeValue(value, std::array<uint16_t, 3>{1024, 1024, 1024});
    case HSA_ISA_INFO_WORKGROUP_MAX_SIZE: return writeValue(value, uint32_t(1024));
    case HSA_ISA_INFO_GRID_MAX_DIM: return writeValue(value, hsa_dim3_t{INT32_MAX, UINT16_MAX, UINT16_MAX});
    case HSA_ISA_INFO_GRID_MAX_SIZE: return writeValue(value, uint64_t(UINT64_MAX));
    case HSA_ISA_INFO_FBARRIER_MAX_SIZE: return writeValue(value, uint32_t(32));
    default: return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
}
}
