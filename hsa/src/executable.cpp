#include "runtime_state.h"
#include "code_object.h"

namespace mac_hsa::detail {
struct CodeReader { std::vector<uint8_t> bytes; };
struct LoadedImage {
    hsa_agent_t agent{};
    std::shared_ptr<Connection> connection;
    DeviceBuffer buffer;
    CodeObject object;
    ~LoadedImage() { if (buffer.handle) connection->freeBuffer(buffer); }
};
struct Executable {
    std::mutex mutex;
    bool frozen = false;
    std::vector<std::shared_ptr<LoadedImage>> images;
    std::vector<uint64_t> symbolHandles;
};
struct ExecutableSymbol {
    std::weak_ptr<Executable> executable;
    std::shared_ptr<LoadedImage> image;
    size_t kernelIndex = 0;
};
std::unordered_map<uint64_t, std::shared_ptr<Executable>> executables;
std::unordered_map<uint64_t, std::shared_ptr<ExecutableSymbol>> executableSymbols;
std::unordered_map<uint64_t, std::shared_ptr<CodeReader>> codeReaders;

static hsa_status_t findExecutable(hsa_executable_t handle, std::shared_ptr<Executable> &out) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    const auto found = executables.find(handle.handle);
    if (found == executables.end()) return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
    out = found->second; return HSA_STATUS_SUCCESS;
}
}
using namespace mac_hsa::detail;
extern "C" {
hsa_status_t hsa_code_object_reader_create_from_memory(const void *data, size_t size,
                                                       hsa_code_object_reader_t *out) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!out || !data || !size) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = {};
    if (size > (256ull << 20) || lastHandle == UINT64_MAX) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    try {
        auto reader = std::make_shared<CodeReader>();
        const auto bytes = static_cast<const uint8_t *>(data);
        reader->bytes.assign(bytes, bytes + size);
        const auto handle = ++lastHandle;
        codeReaders.emplace(handle, std::move(reader));
        out->handle = handle;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
hsa_status_t hsa_code_object_reader_destroy(hsa_code_object_reader_t reader) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    return codeReaders.erase(reader.handle) ? HSA_STATUS_SUCCESS : HSA_STATUS_ERROR_INVALID_CODE_OBJECT_READER;
}
hsa_status_t hsa_executable_create_alt(hsa_profile_t profile, hsa_default_float_rounding_mode_t rounding,
                                       const char *, hsa_executable_t *out) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = {};
    if (profile != HSA_PROFILE_BASE ||
        (rounding != HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT && rounding != HSA_DEFAULT_FLOAT_ROUNDING_MODE_NEAR))
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    if (lastHandle == UINT64_MAX) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    try {
        const auto handle = ++lastHandle;
        executables.emplace(handle, std::make_shared<Executable>());
        out->handle = handle;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
hsa_status_t hsa_executable_destroy(hsa_executable_t handle) {
    std::shared_ptr<Executable> executable;
    const auto status = findExecutable(handle, executable);
    if (status != HSA_STATUS_SUCCESS) return status;
    std::lock_guard operation(executable->mutex);
    std::lock_guard lock(runtimeMutex);
    if (!executables.erase(handle.handle)) return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
    for (const auto symbol : executable->symbolHandles) executableSymbols.erase(symbol);
    return HSA_STATUS_SUCCESS; // final GPU storage release occurs after both locks
}
hsa_status_t hsa_executable_load_agent_code_object(hsa_executable_t handle, hsa_agent_t agent,
    hsa_code_object_reader_t readerHandle, const char *, hsa_loaded_code_object_t *loaded) {
    std::shared_ptr<Executable> executable;
    std::shared_ptr<CodeReader> reader;
    std::shared_ptr<mac_hsa::Connection> connection;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (loaded) *loaded = {};
        const auto found = executables.find(handle.handle);
        if (found == executables.end()) return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
        executable = found->second;
        const auto r = codeReaders.find(readerHandle.handle);
        if (r == codeReaders.end()) return HSA_STATUS_ERROR_INVALID_CODE_OBJECT_READER;
        reader = r->second;
        const auto device = findAgent(agent);
        if (!device || !device->connection) return HSA_STATUS_ERROR_INVALID_AGENT;
        connection = device->connection;
    }
    std::lock_guard operation(executable->mutex);
    if (executable->frozen) return HSA_STATUS_ERROR_FROZEN_EXECUTABLE;
    try {
        auto image = std::make_shared<LoadedImage>();
        if (!mac_hsa::parseCodeObject(reader->bytes, image->object)) return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
        for (const auto &previous : executable->images) {
            if (previous->agent.handle != agent.handle) continue;
            for (const auto &existing : previous->object.kernels)
                for (const auto &kernel : image->object.kernels)
                    if (existing.name == kernel.name || existing.symbol == kernel.symbol)
                        return HSA_STATUS_ERROR_VARIABLE_ALREADY_DEFINED;
        }
        mac_hsa::DeviceSnapshot device;
        auto status = connection->read(device);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (device.gfxMajor != 12 || device.gfxMinor != 0 || device.gfxRevision != 1)
            return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
        image->connection = connection; image->agent = agent;
        status = connection->allocateBuffer(image->object.image.size(), image->buffer);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!image->buffer.handle || !image->buffer.address || image->buffer.address % 16384 ||
            image->buffer.size < image->object.image.size()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        if (!mac_hsa::relocateCodeObject(image->object, image->buffer.address)) return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
        status = connection->writeBuffer(image->buffer, 0, image->object.image.data(), image->object.image.size());
        if (status != HSA_STATUS_SUCCESS) return status;
        std::vector<std::shared_ptr<ExecutableSymbol>> prepared;
        for (size_t i = 0; i < image->object.kernels.size(); ++i)
            prepared.push_back(std::make_shared<ExecutableSymbol>(ExecutableSymbol{executable, image, i}));
        executable->images.reserve(executable->images.size() + 1);
        executable->symbolHandles.reserve(executable->symbolHandles.size() + prepared.size());
        std::vector<uint64_t> inserted;
        inserted.reserve(prepared.size());
        {
            std::lock_guard lock(runtimeMutex);
            if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
            const auto current = executables.find(handle.handle);
            if (current == executables.end() || current->second != executable) return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
            if (lastHandle == UINT64_MAX || prepared.size() > UINT64_MAX - lastHandle - 1)
                return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            try {
                for (auto &symbol : prepared) {
                    const auto id = ++lastHandle;
                    executableSymbols.emplace(id, symbol);
                    inserted.push_back(id);
                }
            } catch (...) {
                for (const auto id : inserted) executableSymbols.erase(id);
                throw;
            }
            executable->symbolHandles.insert(executable->symbolHandles.end(), inserted.begin(), inserted.end());
            executable->images.push_back(image);
            const auto id = ++lastHandle;
            if (loaded) loaded->handle = id;
        }
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
hsa_status_t hsa_executable_freeze(hsa_executable_t handle, const char *) {
    std::shared_ptr<Executable> executable;
    const auto status = findExecutable(handle, executable);
    if (status != HSA_STATUS_SUCCESS) return status;
    std::lock_guard lock(executable->mutex);
    if (executable->frozen) return HSA_STATUS_ERROR_FROZEN_EXECUTABLE;
    executable->frozen = true;
    return HSA_STATUS_SUCCESS;
}
hsa_status_t hsa_executable_validate_alt(hsa_executable_t handle, const char *, uint32_t *result) {
    std::shared_ptr<Executable> executable;
    const auto status = findExecutable(handle, executable);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (!result) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    std::lock_guard lock(executable->mutex);
    // Parsing, relocation and GPU upload are transactional: invalid images are
    // never published into an executable. This does not validate kernel output.
    *result = 0; return HSA_STATUS_SUCCESS;
}
hsa_status_t hsa_executable_get_symbol_by_name(hsa_executable_t handle, const char *name,
    const hsa_agent_t *agent, hsa_executable_symbol_t *out) {
    std::shared_ptr<Executable> executable;
    const auto status = findExecutable(handle, executable);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (!name || !out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = {};
    std::lock_guard operation(executable->mutex);
    std::lock_guard lock(runtimeMutex);
    if (agent && !findAgent(*agent)) return HSA_STATUS_ERROR_INVALID_AGENT;
    for (const auto id : executable->symbolHandles) {
        const auto entry = executableSymbols.find(id);
        if (entry == executableSymbols.end()) continue;
        const auto &symbol = *entry->second;
        const auto &kernel = symbol.image->object.kernels[symbol.kernelIndex];
        if (agent && agent->handle == symbol.image->agent.handle && (kernel.name == name || kernel.symbol == name)) {
            out->handle = id; return HSA_STATUS_SUCCESS;
        }
    }
    return HSA_STATUS_ERROR_INVALID_SYMBOL_NAME;
}
hsa_status_t hsa_executable_symbol_get_info(hsa_executable_symbol_t handle,
    hsa_executable_symbol_info_t attribute, void *value) {
    std::shared_ptr<ExecutableSymbol> symbol;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        const auto entry = executableSymbols.find(handle.handle);
        if (entry == executableSymbols.end()) return HSA_STATUS_ERROR_INVALID_EXECUTABLE_SYMBOL;
        symbol = entry->second;
    }
    const auto executable = symbol->executable.lock();
    if (!executable) return HSA_STATUS_ERROR_INVALID_EXECUTABLE_SYMBOL;
    if (!value) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    std::lock_guard operation(executable->mutex);
    const auto &kernel = symbol->image->object.kernels[symbol->kernelIndex];
    switch (attribute) {
    case HSA_EXECUTABLE_SYMBOL_INFO_TYPE: return writeValue(value, HSA_SYMBOL_KIND_KERNEL);
    case HSA_EXECUTABLE_SYMBOL_INFO_NAME_LENGTH: return writeValue(value, uint32_t(kernel.symbol.size()));
    case HSA_EXECUTABLE_SYMBOL_INFO_NAME: std::memcpy(value, kernel.symbol.data(), kernel.symbol.size()); return HSA_STATUS_SUCCESS;
    case HSA_EXECUTABLE_SYMBOL_INFO_MODULE_NAME_LENGTH: return writeValue(value, uint32_t(0));
    case HSA_EXECUTABLE_SYMBOL_INFO_MODULE_NAME: return HSA_STATUS_SUCCESS;
    case HSA_EXECUTABLE_SYMBOL_INFO_LINKAGE: return writeValue(value, HSA_SYMBOL_LINKAGE_PROGRAM);
    case HSA_EXECUTABLE_SYMBOL_INFO_IS_DEFINITION: return writeValue(value, true);
    case HSA_EXECUTABLE_SYMBOL_INFO_AGENT: return writeValue(value, symbol->image->agent);
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT:
        return writeValue(value, executable->frozen ? symbol->image->buffer.address + kernel.descriptor : uint64_t(0));
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE: return writeValue(value, kernel.kernargSize);
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_ALIGNMENT: return writeValue(value, kernel.kernargAlignment);
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE: return writeValue(value, kernel.groupSize);
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE: return writeValue(value, kernel.privateSize);
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_DYNAMIC_CALLSTACK: return writeValue(value, kernel.dynamicStack);
    default: return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
}
}
