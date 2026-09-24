#include "runtime_state.h"
#include "code_object.h"
#include "mac_hsa.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mac_hsa::detail {
std::recursive_mutex executableLifecycleMutex;
struct CodeReader {
    std::vector<uint8_t> bytes;
    std::string path;
    size_t offset = 0;
    int fd = -1;
    ~CodeReader() { if (fd >= 0) close(fd); }
};
struct LoadedImage {
    hsa_agent_t agent{};
    hsa_executable_t executable{};
    hsa_loaded_code_object_t handle{};
    std::shared_ptr<CodeReader> reader;
    std::string uri;
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
static std::unordered_map<uint64_t, std::weak_ptr<LoadedImage>> loadedImages;
void clearLoadedImages() { loadedImages.clear(); }

static std::string readerURI(const CodeReader &reader) {
    if (reader.fd < 0)
        return "memory://" + std::to_string(getpid()) + "#offset=" +
            std::to_string(reinterpret_cast<uintptr_t>(reader.bytes.data())) +
            "&size=" + std::to_string(reader.bytes.size());
    std::string result = "file://";
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : reader.path) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || std::strchr("/_.~-", c)) result += char(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result + "#offset=" + std::to_string(reader.offset) + "&size=" + std::to_string(reader.bytes.size());
}

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
static hsa_status_t dispatchExecutable(bool aql, hsa_executable_symbol_t handle,
    const void *kernarg, size_t kernargSize, const uint32_t groups[3],
    const uint32_t threads[3], const void *const *buffers, size_t bufferCount,
    uint64_t *fence) {
    std::lock_guard lifecycle(executableLifecycleMutex);
    if (!fence) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *fence = 0;
    std::shared_ptr<ExecutableSymbol> symbol;
    std::shared_ptr<Executable> executable;
    std::array<std::shared_ptr<Allocation>, 15> retained;
    amdgpu::ComputeDispatchRequest request{};
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!groups || !threads || (kernargSize && !kernarg) || bufferCount > retained.size() ||
            (bufferCount && !buffers)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const auto found = executableSymbols.find(handle.handle);
        if (found == executableSymbols.end()) return HSA_STATUS_ERROR_INVALID_EXECUTABLE_SYMBOL;
        symbol = found->second; executable = symbol->executable.lock();
        if (!executable || !executable->frozen) return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
        for (size_t i = 0; i < bufferCount; ++i) {
            retained[i] = findAllocation(buffers[i]);
            if (!retained[i] || retained[i]->connection != symbol->image->connection || !retained[i]->buffer.handle)
                return HSA_STATUS_ERROR_INVALID_ALLOCATION;
            request.buffers[i] = retained[i]->buffer.handle;
        }
    }
    const auto &image = *symbol->image;
    const auto &kernel = image.object.kernels[symbol->kernelIndex];
    if (kernel.kernargSize != kernargSize || kernargSize > 4 * 1024 * 1024)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    // AMDHSA descriptor: ENABLE_SGPR_KERNARG_SEGMENT_PTR (bit 3), wave32
    // (bit 10). Other implicit SGPR layouts need explicit runtime support.
    if (kernel.properties != 0x408 || kernel.preload ||
        kernel.privateSize || kernel.groupSize || kernel.dynamicStack)
        return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
    request.version = 2; request.codeHandle = image.buffer.handle;
    request.codeOffset = kernel.entry;
    for (const auto &segment : image.object.segments) {
        if ((segment.flags & 1) && kernel.entry >= segment.offset && kernel.entry - segment.offset < segment.size)
            request.codeBytes = (segment.size - (kernel.entry - segment.offset)) & ~uint64_t(3);
    }
    for (unsigned i = 0; i < 3; ++i) { request.groups[i] = groups[i]; request.threads[i] = threads[i]; }
    request.rsrc1 = kernel.rsrc1; request.rsrc2 = kernel.rsrc2;
    request.rsrc3 = kernel.rsrc3;
    request.userSGPRCount = 2; request.timeoutUS = 100000;
    auto validatedRequest=request;
    // AQL firmware reads the compiler's SGPR allocation field from the kernel
    // descriptor. The older native PM4 ABI deliberately restricts that field.
    if (aql) validatedRequest.rsrc1 &= ~0x000003c0u;
    if (!amdgpu::compute_dispatch_shape(validatedRequest)) return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
    struct Arguments {
        std::shared_ptr<mac_hsa::Connection> connection;
        mac_hsa::DeviceBuffer buffer;
        ~Arguments() { if (buffer.handle) connection->freeBuffer(buffer); }
    } arguments{image.connection, {}};
    auto status = image.connection->allocateBuffer(kernargSize ? kernargSize : 16, arguments.buffer);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (!arguments.buffer.handle || arguments.buffer.size < kernargSize ||
        !arguments.buffer.address || arguments.buffer.address % 16 ||
        (kernel.kernargAlignment && arguments.buffer.address % kernel.kernargAlignment) ||
        arguments.buffer.address >= (1ull << 48)) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    if (kernargSize) {
        status = image.connection->writeBuffer(arguments.buffer, 0, kernarg, kernargSize);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    request.userSGPR[0] = uint32_t(arguments.buffer.address);
    request.userSGPR[1] = uint32_t(arguments.buffer.address >> 32);
    request.buffers[bufferCount] = arguments.buffer.handle;
    if (aql) {
        amdgpu::AQLDispatchRequest packet{};
        packet.version=1; packet.codeHandle=image.buffer.handle;
        packet.descriptorOffset=kernel.descriptor; packet.kernargHandle=arguments.buffer.handle;
        packet.kernargBytes=kernargSize; packet.timeoutUS=request.timeoutUS;
        for (unsigned i=0;i<3;++i) { packet.groups[i]=groups[i]; packet.threads[i]=threads[i]; }
        for (size_t i=0;i<bufferCount;++i) packet.buffers[i]=request.buffers[i];
        if (!amdgpu::aql_dispatch_shape(packet)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        return image.connection->dispatchAQL(packet,*fence);
    }
    return image.connection->dispatch(request, *fence);
}
hsa_status_t mac_hsa_executable_dispatch(hsa_executable_symbol_t symbol,
    const void *kernarg, size_t bytes, const uint32_t groups[3], const uint32_t threads[3],
    const void *const *buffers, size_t count, uint64_t *fence) {
    return dispatchExecutable(false,symbol,kernarg,bytes,groups,threads,buffers,count,fence);
}
hsa_status_t mac_hsa_executable_dispatch_aql(hsa_executable_symbol_t symbol,
    const void *kernarg, size_t bytes, const uint32_t groups[3], const uint32_t threads[3],
    const void *const *buffers, size_t count, uint64_t *completion) {
    return dispatchExecutable(true,symbol,kernarg,bytes,groups,threads,buffers,count,completion);
}
hsa_status_t hsa_code_object_reader_create_from_memory(const void *data, size_t size,
                                                       hsa_code_object_reader_t *out) {
    std::lock_guard lifecycle(executableLifecycleMutex);
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
    std::lock_guard lifecycle(executableLifecycleMutex);
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    return codeReaders.erase(reader.handle) ? HSA_STATUS_SUCCESS : HSA_STATUS_ERROR_INVALID_CODE_OBJECT_READER;
}
hsa_status_t hsa_executable_create_alt(hsa_profile_t profile, hsa_default_float_rounding_mode_t rounding,
                                       const char *, hsa_executable_t *out) {
    std::lock_guard lifecycle(executableLifecycleMutex);
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
    std::lock_guard lifecycle(executableLifecycleMutex);
    std::shared_ptr<Executable> executable;
    const auto status = findExecutable(handle, executable);
    if (status != HSA_STATUS_SUCCESS) return status;
    std::lock_guard operation(executable->mutex);
    std::lock_guard lock(runtimeMutex);
    if (!executables.erase(handle.handle)) return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
    for (const auto symbol : executable->symbolHandles) executableSymbols.erase(symbol);
    for (const auto &image : executable->images) loadedImages.erase(image->handle.handle);
    return HSA_STATUS_SUCCESS; // final GPU storage release occurs after both locks
}
hsa_status_t hsa_executable_load_agent_code_object(hsa_executable_t handle, hsa_agent_t agent,
    hsa_code_object_reader_t readerHandle, const char *, hsa_loaded_code_object_t *loaded) {
    std::lock_guard lifecycle(executableLifecycleMutex);
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
        image->reader = reader; image->executable = handle; image->uri = readerURI(*reader);
        status = connection->allocateBuffer(image->object.image.size(), image->buffer);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!image->buffer.handle || !image->buffer.address || image->buffer.address % 16384 ||
            image->buffer.size < image->object.image.size() || image->buffer.address >= (1ull << 48) ||
            image->buffer.size > (1ull << 48) - image->buffer.address) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        if (!mac_hsa::relocateCodeObject(image->object, image->buffer.address)) return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
        status = connection->writeBuffer(image->buffer, 0, image->object.image.data(), image->object.image.size());
        if (status != HSA_STATUS_SUCCESS) return status;
        // ROCr RegionMemory::Freeze invalidates agent code caches after upload.
        // AQL acquire fences alone do not retire stale instructions when a
        // destroyed executable's allocation is reused by a different image.
        status = connection->invalidateCodeCaches();
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
            for (const auto &[existingID, weak] : loadedImages) {
                (void)existingID;
                const auto existing = weak.lock();
                if (existing && image->buffer.address < existing->buffer.address + existing->buffer.size &&
                    existing->buffer.address < image->buffer.address + image->buffer.size)
                    return HSA_STATUS_ERROR_OUT_OF_RESOURCES; // address-only loader queries must be unambiguous
            }
            if (lastHandle == UINT64_MAX || prepared.size() > UINT64_MAX - lastHandle - 1)
                return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            try {
                image->handle.handle = ++lastHandle;
                loadedImages.emplace(image->handle.handle, image);
                for (auto &symbol : prepared) {
                    const auto id = ++lastHandle;
                    executableSymbols.emplace(id, symbol);
                    inserted.push_back(id);
                }
            } catch (...) {
                for (const auto id : inserted) executableSymbols.erase(id);
                loadedImages.erase(image->handle.handle);
                throw;
            }
            executable->symbolHandles.insert(executable->symbolHandles.end(), inserted.begin(), inserted.end());
            executable->images.push_back(image);
            if (loaded) *loaded = image->handle;
        }
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
hsa_status_t hsa_executable_freeze(hsa_executable_t handle, const char *) {
    std::lock_guard lifecycle(executableLifecycleMutex);
    std::shared_ptr<Executable> executable;
    const auto status = findExecutable(handle, executable);
    if (status != HSA_STATUS_SUCCESS) return status;
    std::lock_guard lock(executable->mutex);
    if (executable->frozen) return HSA_STATUS_ERROR_FROZEN_EXECUTABLE;
    executable->frozen = true;
    return HSA_STATUS_SUCCESS;
}
hsa_status_t hsa_executable_validate_alt(hsa_executable_t handle, const char *, uint32_t *result) {
    std::lock_guard lifecycle(executableLifecycleMutex);
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
    std::lock_guard lifecycle(executableLifecycleMutex);
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
    std::lock_guard lifecycle(executableLifecycleMutex);
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

namespace {
// The caller holds runtimeMutex. Loaded images are published only after upload.
std::shared_ptr<LoadedImage> imageAt(uintptr_t address, bool host = false) {
    for (const auto &[id, weak] : loadedImages) {
        (void)id;
        const auto image = weak.lock();
        if (!image) continue;
        const auto base = host ? reinterpret_cast<uintptr_t>(image->object.image.data()) : image->buffer.address;
        if (address < base) continue;
        const auto offset = address - base;
        for (const auto &segment : image->object.segments)
            if (offset >= segment.offset && offset - segment.offset < segment.size) return image;
    }
    return {};
}
}
extern "C" {
HSA_API_EXPORT hsa_status_t hsa_ven_amd_loader_query_host_address(const void *device, const void **host) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!device || !host) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *host = nullptr;
    auto image = imageAt(reinterpret_cast<uintptr_t>(device));
    if (image) {
        *host = image->object.image.data() + (reinterpret_cast<uintptr_t>(device) - image->buffer.address);
        return HSA_STATUS_SUCCESS;
    }
    if (imageAt(reinterpret_cast<uintptr_t>(device), true)) { *host = device; return HSA_STATUS_SUCCESS; }
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
}
HSA_API_EXPORT hsa_status_t hsa_ven_amd_loader_query_executable(const void *device, hsa_executable_t *out) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!device || !out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = {};
    const auto image = imageAt(reinterpret_cast<uintptr_t>(device));
    if (!image) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = image->executable; return HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_ven_amd_loader_query_segment_descriptors(
    hsa_ven_amd_loader_segment_descriptor_t *out, size_t *count) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!count || (!out != !*count)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    size_t required = 0;
    for (const auto &[id, weak] : loadedImages) {
        (void)id;
        const auto image = weak.lock();
        if (!image) continue;
        for (const auto &segment : image->object.segments) {
            required += segment.fileSize != 0;
            required += segment.size > segment.fileSize;
        }
    }
    if (!out) { *count = required; return HSA_STATUS_SUCCESS; }
    if (*count != required) return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
    size_t index = 0;
    for (const auto &[id, weak] : loadedImages) {
        (void)id;
        const auto image = weak.lock();
        if (!image) continue;
        const auto &reader = *image->reader;
        for (const auto &segment : image->object.segments) {
            if (segment.fileSize) {
                out[index++] = {image->agent, image->executable,
                    reader.fd < 0 ? HSA_VEN_AMD_LOADER_CODE_OBJECT_STORAGE_TYPE_MEMORY : HSA_VEN_AMD_LOADER_CODE_OBJECT_STORAGE_TYPE_FILE,
                    reader.fd < 0 ? static_cast<const void *>(reader.bytes.data()) : reader.path.c_str(),
                    reader.fd < 0 ? reader.bytes.size() : reader.path.size() + 1,
                    size_t(segment.fileOffset + reader.offset),
                    reinterpret_cast<const void *>(image->buffer.address + segment.offset), size_t(segment.fileSize)};
            }
            if (segment.size > segment.fileSize) {
                out[index++] = {image->agent, image->executable, HSA_VEN_AMD_LOADER_CODE_OBJECT_STORAGE_TYPE_NONE,
                    nullptr, 0, 0, reinterpret_cast<const void *>(image->buffer.address + segment.offset + segment.fileSize),
                    size_t(segment.size - segment.fileSize)};
            }
        }
    }
    return HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_ven_amd_loader_executable_iterate_loaded_code_objects(
    hsa_executable_t handle, hsa_status_t (*callback)(hsa_executable_t, hsa_loaded_code_object_t, void *), void *data) {
    std::lock_guard lifecycle(executableLifecycleMutex);
    std::shared_ptr<Executable> executable;
    const auto status = findExecutable(handle, executable);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (!callback) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    // No runtime lock across callbacks: HRX queries agent and load metadata here.
    for (const auto &image : executable->images) {
        const auto result = callback(handle, image->handle, data);
        if (result != HSA_STATUS_SUCCESS) return result;
    }
    return HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_ven_amd_loader_loaded_code_object_get_info(hsa_loaded_code_object_t handle,
    hsa_ven_amd_loader_loaded_code_object_info_t attribute, void *value) {
    std::lock_guard lifecycle(executableLifecycleMutex);
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    const auto found = loadedImages.find(handle.handle);
    const auto image = found == loadedImages.end() ? nullptr : found->second.lock();
    if (!image) return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
    if (!value) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto executable = executables.find(image->executable.handle);
    if (executable == executables.end()) return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
    if (attribute >= HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_DELTA &&
        attribute <= HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_SIZE && !executable->second->frozen)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto &reader = *image->reader;
    switch (attribute) {
    case HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_EXECUTABLE: return writeValue(value, image->executable);
    case HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_KIND: return writeValue(value, uint32_t(HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_KIND_AGENT));
    case HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_AGENT: return writeValue(value, image->agent);
    case HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_CODE_OBJECT_STORAGE_TYPE:
        return writeValue(value, uint32_t(reader.fd < 0 ? HSA_VEN_AMD_LOADER_CODE_OBJECT_STORAGE_TYPE_MEMORY : HSA_VEN_AMD_LOADER_CODE_OBJECT_STORAGE_TYPE_FILE));
    case HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_CODE_OBJECT_STORAGE_MEMORY_BASE:
        if (reader.fd >= 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        return writeValue(value, uint64_t(reinterpret_cast<uintptr_t>(reader.bytes.data())));
    case HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_CODE_OBJECT_STORAGE_MEMORY_SIZE:
        if (reader.fd >= 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        return writeValue(value, uint64_t(reader.bytes.size()));
    case HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_CODE_OBJECT_STORAGE_FILE:
        if (reader.fd < 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        return writeValue(value, reader.fd);
    case HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_DELTA:
        return writeValue(value, int64_t(image->buffer.address - image->object.virtualBase));
    case HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_BASE: return writeValue(value, image->buffer.address);
    case HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_SIZE: return writeValue(value, image->buffer.size);
    case HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_URI_LENGTH: return writeValue(value, uint32_t(image->uri.size()));
    case HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_URI:
        std::memcpy(value, image->uri.c_str(), image->uri.size() + 1); return HSA_STATUS_SUCCESS;
    default: return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
}
HSA_API_EXPORT hsa_status_t hsa_ven_amd_loader_code_object_reader_create_from_file_with_offset_size(
    hsa_file_t file, size_t offset, size_t size, hsa_code_object_reader_t *out) {
    std::lock_guard lifecycle(executableLifecycleMutex);
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = {};
    if (!size || size > (256ull << 20) || offset > INT64_MAX || size > INT64_MAX - offset)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    struct stat stat{};
    if (fstat(file, &stat) != 0 || !S_ISREG(stat.st_mode) || stat.st_size < 0 ||
        offset > uint64_t(stat.st_size) || size > uint64_t(stat.st_size) - offset)
        return HSA_STATUS_ERROR_INVALID_FILE;
    if (lastHandle == UINT64_MAX) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    try {
        auto reader = std::make_shared<CodeReader>();
        reader->fd = fcntl(file, F_DUPFD_CLOEXEC, 0);
        if (reader->fd < 0) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        char path[PATH_MAX]{};
        if (fcntl(reader->fd, F_GETPATH, path) != 0) return HSA_STATUS_ERROR_INVALID_FILE;
        reader->path = path; reader->offset = offset; reader->bytes.resize(size);
        size_t done = 0;
        while (done < size) {
            const auto bytes = pread(reader->fd, reader->bytes.data() + done, size - done, off_t(offset + done));
            if (bytes < 0 && errno == EINTR) continue;
            if (bytes <= 0) return HSA_STATUS_ERROR_INVALID_FILE;
            done += size_t(bytes);
        }
        const auto handle = ++lastHandle;
        codeReaders.emplace(handle, std::move(reader)); out->handle = handle;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
HSA_API_EXPORT hsa_status_t hsa_ven_amd_loader_iterate_executables(
    hsa_status_t (*callback)(hsa_executable_t, void *), void *data) {
    std::lock_guard lifecycle(executableLifecycleMutex);
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!callback) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    // Creation/destruction is serialized for the entire traversal, as specified.
    for (const auto &[handle, executable] : executables) {
        (void)executable;
        const auto status = callback({handle}, data);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    return HSA_STATUS_SUCCESS;
}
}
namespace mac_hsa::detail {
hsa_status_t loaderExtensionTable(size_t size, void *table) {
    const hsa_ven_amd_loader_1_03_pfn_t functions{
        hsa_ven_amd_loader_query_host_address, hsa_ven_amd_loader_query_segment_descriptors,
        hsa_ven_amd_loader_query_executable, hsa_ven_amd_loader_executable_iterate_loaded_code_objects,
        hsa_ven_amd_loader_loaded_code_object_get_info,
        hsa_ven_amd_loader_code_object_reader_create_from_file_with_offset_size,
        hsa_ven_amd_loader_iterate_executables};
    std::memcpy(table, &functions, std::min(size, sizeof(functions)));
    return HSA_STATUS_SUCCESS;
}
}
