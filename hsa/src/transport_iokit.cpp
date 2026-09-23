#include "device_init.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <atomic>
#include <new>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace mac_hsa {
namespace {
class IOObject {
public:
    explicit IOObject(io_object_t value) : value_(value) {}
    ~IOObject() { if (value_) IOObjectRelease(value_); }
    IOObject(const IOObject &) = delete;
    IOObject &operator=(const IOObject &) = delete;
private:
    io_object_t value_;
};

class IOKitConnection final : public Connection, private InitializationRPC {
public:
    io_service_t service = IO_OBJECT_NULL;
    uint64_t registryID = 0;
    ~IOKitConnection() override {
        // FinishStop resets or quarantines resources before releasing backing.
        for (const auto &[handle, buffer] : sharedBuffers) {
            (void)handle;
            IOConnectUnmapMemory64(ownerPort, buffer.memoryType, mach_task_self(), reinterpret_cast<uintptr_t>(buffer.host));
        }
        if (ownerPort) IOServiceClose(ownerPort);
        if (service) IOObjectRelease(service);
    }
    bool supportsBuffers() const override { return true; }


    static hsa_status_t call(io_connect_t port, uint32_t selector, const uint64_t *input, uint32_t inputs,
                      uint64_t *output, uint32_t outputs) {
        uint32_t count = outputs;
        const auto result = IOConnectCallScalarMethod(port, selector, input, inputs,
                                                      output, &count);
        if (result == kIOReturnNoDevice || result == kIOReturnNotAttached ||
            result == MACH_SEND_INVALID_DEST) return HSA_STATUS_ERROR_INVALID_AGENT;
        if (result == kIOReturnBusy || result == kIOReturnNoMemory || result == kIOReturnNoSpace || result == kIOReturnNoResources)
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        if (result == kIOReturnBadArgument) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (result != KERN_SUCCESS || count != outputs) return HSA_STATUS_ERROR;
        return HSA_STATUS_SUCCESS;
    }

    hsa_status_t memoryCapacity(uint64_t &bytes) override {
        std::lock_guard lock(sessionMutex);
        const auto status = ensureReady();
        if (status == HSA_STATUS_SUCCESS) bytes = capacity;
        return status;
    }
    hsa_status_t allocateBuffer(uint64_t bytes, DeviceBuffer &buffer) override {
        std::lock_guard lock(sessionMutex);
        const auto status = ensureReady();
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!bytes || bytes > capacity || bytes > UINT64_MAX - 16383)
            return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        return allocateRaw((bytes + 16383) & ~uint64_t(16383), 3, buffer);
    }
    hsa_status_t freeBuffer(const DeviceBuffer &buffer) override {
        std::lock_guard lock(sessionMutex);
        if (state != State::Ready) return HSA_STATUS_ERROR;
        if (sharedBuffers.contains(buffer.handle)) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        const auto status = scalar(17, {&buffer.handle, 1}, {});
        if (status != HSA_STATUS_SUCCESS) state = State::Faulted;
        return status;
    }
    hsa_status_t readBuffer(const DeviceBuffer &buffer, uint64_t offset, void *out, size_t bytes) override {
        std::lock_guard lock(sessionMutex);
        return transfer(buffer, offset, out, bytes, false);
    }
    hsa_status_t writeBuffer(const DeviceBuffer &buffer, uint64_t offset, const void *in, size_t bytes) override {
        std::lock_guard lock(sessionMutex);
        return transfer(buffer, offset, const_cast<void *>(in), bytes, true);
    }
    hsa_status_t allocateSharedBuffer(uint64_t bytes, SharedBuffer &out) override {
        std::lock_guard lock(sessionMutex);
        out = {};
        auto status = ensureReady();
        if (status != HSA_STATUS_SUCCESS) return status;
        status = ensureHostWindow();
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!bytes || bytes > hostWindowSize || bytes > UINT64_MAX - 16383)
            return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        SharedBuffer buffer;
        status = allocateRaw((bytes + 16383) & ~uint64_t(16383), 2, buffer.device);
        if (status != HSA_STATUS_SUCCESS) return status;
        std::array<uint64_t, 2> mapping{};
        status = scalar(36, {&buffer.device.handle, 1}, mapping);
        mach_vm_address_t address = buffer.device.address;
        mach_vm_size_t size = 0;
        if (status == HSA_STATUS_SUCCESS && mapping[0] <= UINT32_MAX && mapping[1] == buffer.device.size &&
            address >= hostWindowBase && address - hostWindowBase <= hostWindowSize &&
            buffer.device.size <= hostWindowSize - (address - hostWindowBase)) {
            buffer.memoryType = uint32_t(mapping[0]);
            // A placed mapping fails on collisions; never overwrite process memory.
            if (IOConnectMapMemory64(ownerPort, buffer.memoryType, mach_task_self(), &address, &size, 0) == KERN_SUCCESS) {
                if (address == buffer.device.address && size == buffer.device.size) {
                    buffer.host = reinterpret_cast<void *>(address);
                    try {
                        sharedBuffers.emplace(buffer.device.handle, buffer);
                        std::memset(buffer.host, 0, size);
                        std::atomic_thread_fence(std::memory_order_seq_cst);
                        out = buffer; return HSA_STATUS_SUCCESS;
                    } catch (const std::bad_alloc &) { /* unmap before releasing backing */ }
                }
                if (IOConnectUnmapMemory64(ownerPort, buffer.memoryType, mach_task_self(), address) != KERN_SUCCESS) {
                    state = State::Faulted; return HSA_STATUS_ERROR;
                }
            }
        }
        const auto cleanup = scalar(17, {&buffer.device.handle, 1}, {});
        if (cleanup != HSA_STATUS_SUCCESS) { state = State::Faulted; return cleanup; }
        return status != HSA_STATUS_SUCCESS ? status : HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    hsa_status_t freeSharedBuffer(const SharedBuffer &buffer) override {
        std::lock_guard lock(sessionMutex);
        const auto found = sharedBuffers.find(buffer.device.handle);
        if (found == sharedBuffers.end() || found->second.host != buffer.host ||
            found->second.device.size != buffer.device.size || found->second.device.address != buffer.device.address ||
            found->second.memoryType != buffer.memoryType)
            return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        if (state != State::Ready) return HSA_STATUS_ERROR;
        for (const auto &[queue,handles]:hardwareQueues) {
            (void)queue;
            if (handles[0]==buffer.device.handle || handles[1]==buffer.device.handle)
                return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        }
        if (IOConnectUnmapMemory64(ownerPort, buffer.memoryType, mach_task_self(), reinterpret_cast<uintptr_t>(buffer.host)) != KERN_SUCCESS) {
            state = State::Faulted; return HSA_STATUS_ERROR;
        }
        sharedBuffers.erase(found);
        const auto status = scalar(17, {&buffer.device.handle, 1}, {});
        if (status != HSA_STATUS_SUCCESS) state = State::Faulted;
        return status;
    }
    hsa_status_t createQueue(const SharedBuffer &ring,const SharedBuffer &metadata,uint32_t packets,uint64_t &handle) override {
        std::lock_guard lock(sessionMutex); handle=0;
        if (state!=State::Ready) return HSA_STATUS_ERROR;
        for (const auto *buffer:{&ring,&metadata}) {
            const auto found=sharedBuffers.find(buffer->device.handle);
            if (found==sharedBuffers.end() || found->second.host!=buffer->host ||
                found->second.device.address!=buffer->device.address || found->second.device.size!=buffer->device.size)
                return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        }
        if (packets<64 || packets>4096 || (packets&(packets-1)) ||
            ring.device.size<uint64_t(packets)*64 || metadata.device.size<512 || ring.device.handle==metadata.device.handle)
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        std::array<uint64_t,3> build{};
        auto status=scalar(43,{},build);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (build[2]<kPersistentQueueDriverBuild) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        // Reserve bookkeeping before firmware can own the shared buffers.
        auto [record,inserted]=hardwareQueues.emplace(0,std::array<uint64_t,2>{ring.device.handle,metadata.device.handle});
        if (!inserted) return HSA_STATUS_ERROR;
        const std::array<uint64_t,3> input={ring.device.handle,metadata.device.handle,packets};
        std::array<uint64_t,2> output{};
        std::atomic_thread_fence(std::memory_order_seq_cst);
        status=scalar(56,input,output);
        // These RPC errors are returned before any queue map is attempted.
        // Exhausting the seven queue slots must not poison existing queues.
        if (status==HSA_STATUS_ERROR_OUT_OF_RESOURCES || status==HSA_STATUS_ERROR_INVALID_ARGUMENT) {
            hardwareQueues.erase(record);return status;
        }
        if (status!=HSA_STATUS_SUCCESS || output[0] || !output[1] || hardwareQueues.contains(output[1])) {
            state=State::Faulted;return HSA_STATUS_ERROR;
        }
        auto node=hardwareQueues.extract(record);node.key()=output[1];hardwareQueues.insert(std::move(node));
        handle=output[1];return HSA_STATUS_SUCCESS;
    }
    hsa_status_t kickQueue(uint64_t handle,uint64_t packet) override {
        std::lock_guard lock(sessionMutex);
        if (state!=State::Ready) return HSA_STATUS_ERROR;
        if (!hardwareQueues.contains(handle) || !handle || packet==UINT64_MAX) return HSA_STATUS_ERROR_INVALID_QUEUE;
        const std::array<uint64_t,2> input={handle,packet};std::array<uint64_t,1> output{};
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto status=scalar(57,input,output);
        if (status!=HSA_STATUS_SUCCESS || output[0]) {state=State::Faulted;return HSA_STATUS_ERROR;}
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t destroyQueue(uint64_t handle) override {
        std::lock_guard lock(sessionMutex);
        if (state!=State::Ready) return HSA_STATUS_ERROR;
        if (!handle || !hardwareQueues.contains(handle)) return HSA_STATUS_ERROR_INVALID_QUEUE;
        std::array<uint64_t,1> output{};
        const auto status=scalar(58,{&handle,1},output);
        if (status!=HSA_STATUS_SUCCESS || output[0]) {state=State::Faulted;return HSA_STATUS_ERROR;}
        std::atomic_thread_fence(std::memory_order_seq_cst);
        hardwareQueues.erase(handle);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t dispatchAQL(const amdgpu::AQLDispatchRequest &request, uint64_t &completion) override {
        std::lock_guard lock(sessionMutex);
        completion=UINT64_MAX;
        if (state != State::Ready) return HSA_STATUS_ERROR;
        if (!amdgpu::aql_dispatch_shape(request)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        std::array<uint64_t,3> build{};
        const auto buildStatus=scalar(43,{},build);
        if (buildStatus != HSA_STATUS_SUCCESS) return buildStatus;
        if (build[2]<184) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::array<uint64_t,5> output{};
        uint32_t count=output.size();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto status=IOConnectCallMethod(ownerPort,55,nullptr,0,&request,sizeof(request),
            output.data(),&count,nullptr,nullptr);
        if (status != KERN_SUCCESS || count != output.size() || output[0] || output[1] ||
            output[2]!=5 || output[3] || output[4]!=1) {
            state=State::Faulted; return HSA_STATUS_ERROR;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        completion=output[1]; return HSA_STATUS_SUCCESS;
    }
    hsa_status_t dispatch(const amdgpu::ComputeDispatchRequest &request, uint64_t &fence) override {
        std::lock_guard lock(sessionMutex);
        fence = 0;
        if (state != State::Ready) return HSA_STATUS_ERROR;
        if (!amdgpu::compute_dispatch_shape(request)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (request.version == 2) {
            std::array<uint64_t, 3> build{};
            const auto status = scalar(43, {}, build);
            if (status != HSA_STATUS_SUCCESS) return status;
            if (build[2] < 183) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        }
        std::array<uint64_t, 3> output{};
        uint32_t count = output.size();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto status = IOConnectCallMethod(ownerPort, 51, nullptr, 0, &request,
            request.version == 1 ? amdgpu::kComputeDispatchV1Bytes : sizeof(request),
            output.data(), &count, nullptr, nullptr);
        if (status != KERN_SUCCESS || count != output.size() || output[0] ||
            output[2] != 3 || !output[1] || output[1] <= lastComputeFence) {
            // A failed or malformed response cannot prove completion. The
            // driver keeps code, arguments and referenced BOs until recovery.
            state = State::Faulted; return HSA_STATUS_ERROR;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        fence = lastComputeFence = output[1];
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t testSharedAtomicAdd(const SharedBuffer &buffer, uint64_t offset,
                                    int64_t increment, uint32_t repetitions) override {
        std::lock_guard lock(sessionMutex);
        if (state != State::Ready) return HSA_STATUS_ERROR;
        const auto found = sharedBuffers.find(buffer.device.handle);
        if (found == sharedBuffers.end() || found->second.host != buffer.host ||
            found->second.device.address != buffer.device.address ||
            found->second.device.size != buffer.device.size || found->second.memoryType != buffer.memoryType)
            return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        if (offset % 8 || offset > buffer.device.size || buffer.device.size - offset < 8 ||
            !repetitions || repetitions > 64) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        std::array<uint64_t, 3> gfx{};
        const uint64_t tag = 1;
        auto status = scalar(21, {&tag, 1}, gfx);
        if (status != HSA_STATUS_SUCCESS) return status;
        // Match ROCr's BlitSdmaV5 for GFX12.0.1: no GFX12.5 scope fields.
        if (gfx != std::array<uint64_t, 3>{12, 0, 1}) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        const std::array<uint64_t, 2> create{0, 0};
        uint64_t stream = 0;
        status = scalar(37, create, {&stream, 1});
        if (status != HSA_STATUS_SUCCESS) return status;
        const auto address = buffer.device.address + offset;
        const auto value = static_cast<uint64_t>(increment);
        // SDMA_PKT_ATOMIC / ADD64, as emitted by BuildAtomicDecrementCommand.
        const std::array<uint64_t, 10> packet{stream, 10u | (47u << 25),
            uint32_t(address), uint32_t(address >> 32), uint32_t(value), uint32_t(value >> 32), 0, 0, 0, 8};
        for (uint32_t i = 0; i < repetitions && status == HSA_STATUS_SUCCESS; ++i)
            status = scalar(38, packet, {});
        if (status != HSA_STATUS_SUCCESS) {
            if (scalar(39, {&stream, 1}, {}) != HSA_STATUS_SUCCESS) state = State::Faulted;
            return status;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        uint64_t fence = 0;
        status = scalar(19, {&stream, 1}, {&fence, 1});
        if (status != HSA_STATUS_SUCCESS) {
            // Submission may have partially reached hardware; retain all backing.
            state = State::Faulted; return status;
        }
        const std::array<uint64_t, 2> wait{fence, 1000000000};
        uint64_t timedOut = 1;
        status = scalar(20, wait, {&timedOut, 1});
        if (status != HSA_STATUS_SUCCESS || timedOut) {
            state = State::Faulted;
            return status == HSA_STATUS_SUCCESS ? HSA_STATUS_ERROR : status;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        status = scalar(39, {&stream, 1}, {});
        if (status != HSA_STATUS_SUCCESS) state = State::Faulted;
        return status;
    }
    hsa_status_t copyBuffers(const DeviceBuffer &source, uint64_t sourceOffset,
        const DeviceBuffer &destination, uint64_t destinationOffset, size_t bytes) override {
        std::lock_guard lock(sessionMutex);
        if (state != State::Ready) return HSA_STATUS_ERROR;
        if (!bytes || bytes > 4 * 1024 * 1024 || sourceOffset > source.size || bytes > source.size - sourceOffset ||
            destinationOffset > destination.size || bytes > destination.size - destinationOffset ||
            (source.handle == destination.handle && sourceOffset < destinationOffset + bytes &&
             destinationOffset < sourceOffset + bytes))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto status = copyRaw(source.handle, sourceOffset, destination.handle, destinationOffset, bytes);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return status;
    }
    hsa_status_t exportBuffer(const DeviceBuffer &buffer, BufferToken &token) override {
        std::lock_guard lock(sessionMutex);
        if (state != State::Ready) return HSA_STATUS_ERROR;
        std::array<uint64_t, 3> build{};
        auto status = scalar(43, {}, build);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (build[2] < 181) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::array<uint64_t, 3> input{buffer.handle, 0, 0}, output{};
        arc4random_buf(input.data() + 1, 16);
        status = scalar(52, input, output);
        if (status != HSA_STATUS_SUCCESS) return status;
        if ((!output[0] && !output[1]) || output[2] != buffer.size) return HSA_STATUS_ERROR;
        token = {registryID, {output[0], output[1]}, output[2]}; return HSA_STATUS_SUCCESS;
    }
    hsa_status_t importBuffer(const BufferToken &token, DeviceBuffer &buffer) override {
        std::lock_guard lock(sessionMutex);
        if (token.registryID != registryID || !token.size || token.size % 16384 || (!token.token[0] && !token.token[1]))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        // Probe before claiming: a stale token must not initialize/reset a GPU.
        io_connect_t probe = IO_OBJECT_NULL;
        if (IOServiceOpen(service, mach_task_self(), 0, &probe) != KERN_SUCCESS) return HSA_STATUS_ERROR_INVALID_AGENT;
        std::array<uint64_t, 3> build{}; uint64_t tag = 4, stage = 0;
        auto status = call(probe, 43, nullptr, 0, build.data(), 3);
        if (status == HSA_STATUS_SUCCESS) status = call(probe, 21, &tag, 1, &stage, 1);
        IOServiceClose(probe);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (build[2] < 181 || stage != 15) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        status = ensureReady(false);
        if (status != HSA_STATUS_SUCCESS) return status;
        std::array<uint64_t, 3> input{token.token[0], token.token[1], token.size}, output{};
        status = scalar(53, input, output);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!output[0] || !output[1] || output[2] != token.size || output[1] > UINT64_MAX - output[2]) {
            state = State::Faulted; return HSA_STATUS_ERROR;
        }
        buffer = {output[0], output[1], output[2]}; return HSA_STATUS_SUCCESS;
    }

private:
    enum class State { Unclaimed, Initializing, Ready, Faulted } state = State::Unclaimed;
    std::mutex sessionMutex;
    std::map<uint64_t,std::array<uint64_t,2>> hardwareQueues;
    io_connect_t ownerPort = IO_OBJECT_NULL;
    uint64_t lastComputeFence = 0;
    uint64_t capacity = 0;
    DeviceBuffer staging;
    uint64_t hostWindowBase = 0, hostWindowSize = 0;
    std::map<uint64_t, SharedBuffer> sharedBuffers;
    std::map<std::string, std::vector<uint8_t>> firmware;

    hsa_status_t scalar(uint32_t selector, std::span<const uint64_t> input,
                        std::span<uint64_t> output) override {
        return call(ownerPort, selector, input.data(), uint32_t(input.size()),
                    output.data(), uint32_t(output.size()));
    }
    hsa_status_t prepareFirmware(const std::vector<FirmwareFile> &files) override {
        const auto configured = std::getenv("MAC_AMDGPU_FIRMWARE_DIR");
        const std::string directory = configured ? configured :
            "/Applications/MacAMDGPUHost.app/Contents/Resources/firmware";
        for (const auto &file : files) {
            std::ifstream stream(directory + "/" + file.name, std::ios::binary | std::ios::ate);
            if (!stream) return HSA_STATUS_ERROR_INVALID_FILE;
            const auto size = stream.tellg();
            if (size <= 0 || size > (32 << 20)) return HSA_STATUS_ERROR_INVALID_FILE;
            auto &data = firmware[file.name];
            data.resize(size_t(size));
            stream.seekg(0);
            if (!stream.read(reinterpret_cast<char *>(data.data()), size)) return HSA_STATUS_ERROR_INVALID_FILE;
        }
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t uploadFirmware(const FirmwareFile &file) override {
        const auto found = firmware.find(file.name);
        if (found == firmware.end()) return HSA_STATUS_ERROR_INVALID_FILE;
        mach_vm_address_t address = 0;
        mach_vm_size_t size = 0;
        if (IOConnectMapMemory64(ownerPort, 6, mach_task_self(), &address, &size, kIOMapAnywhere) != KERN_SUCCESS)
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        const auto &data = found->second;
        hsa_status_t status = HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        if (address && data.size() <= size) {
            std::memcpy(reinterpret_cast<void *>(address), data.data(), data.size());
            const std::array<uint64_t, 2> input{file.type, data.size()};
            uint64_t output = 0;
            status = scalar(10, input, {&output, 1});
        }
        if (IOConnectUnmapMemory64(ownerPort, 6, mach_task_self(), address) != KERN_SUCCESS)
            return HSA_STATUS_ERROR;
        return status;
    }
    void waitAfterReset() override { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }
    hsa_status_t ensureReady(bool allowInitialize = true) {
        if (state == State::Ready) return HSA_STATUS_SUCCESS;
        if (state != State::Unclaimed) return HSA_STATUS_ERROR;
        if (IOServiceOpen(service, mach_task_self(), 0, &ownerPort) != KERN_SUCCESS)
            return HSA_STATUS_ERROR_INVALID_AGENT;
        state = State::Initializing;
        bool claimed = false;
        hsa_status_t status;
        try { status = initializeDevice(*this, claimed, capacity, allowInitialize); }
        catch (const std::bad_alloc &) { status = HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
        firmware.clear();
        if (!claimed) {
            // Busy acquisition must not leave an idle client blocking Host Stop.
            IOServiceClose(ownerPort);
            ownerPort = IO_OBJECT_NULL;
            state = State::Unclaimed;
            return status;
        }
        state = status == HSA_STATUS_SUCCESS ? State::Ready : State::Faulted;
        return status;
    }
    hsa_status_t ensureHostWindow() {
        if (hostWindowBase) return HSA_STATUS_SUCCESS;
        std::array<uint64_t, 3> build{};
        auto status = scalar(43, {}, build);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (build[2] < 182) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        uint64_t candidate = 0;
        std::array<uint64_t, 3> window{};
        status = scalar(54, {&candidate, 1}, window);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!window[1] || window[1] > (1ull << 30) || (window[1] & (window[1] - 1))) return HSA_STATUS_ERROR;
        if (!window[0]) {
            // Ask Mach for an unused aligned range. It is only a placement hint;
            // each later IOKit mapping must independently succeed at its GPU VA.
            mach_vm_address_t reservation = 0;
            const auto result = mach_vm_map(mach_task_self(), &reservation, window[1], window[1] - 1,
                VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, false, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE);
            if (result != KERN_SUCCESS) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            candidate = reservation;
            const auto reservedSize = window[1];
            status = scalar(54, {&candidate, 1}, window);
            const auto released = mach_vm_deallocate(mach_task_self(), reservation, reservedSize);
            if (released != KERN_SUCCESS) return HSA_STATUS_ERROR;
            if (status != HSA_STATUS_SUCCESS) return status;
        }
        if (window[0] < (1ull << 32) || (window[0] & (window[1] - 1)) ||
            window[0] >= (1ull << 47) || window[1] > (1ull << 47) - window[0]) return HSA_STATUS_ERROR;
        if (!window[2]) {
            const uint64_t seed = arc4random();
            std::array<uint64_t, 6> result{};
            status = scalar(44, {&seed, 1}, result);
            if (status != HSA_STATUS_SUCCESS || result[0] || result[1] != 8 || result[2]) {
                state = State::Faulted; return status == HSA_STATUS_SUCCESS ? HSA_STATUS_ERROR : status;
            }
        }
        hostWindowBase = window[0]; hostWindowSize = window[1];
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t allocateRaw(uint64_t bytes, uint64_t domain, DeviceBuffer &buffer) {
        const std::array<uint64_t, 4> input{bytes, domain, 16384, 0};
        std::array<uint64_t, 3> output{};
        const auto status = scalar(16, input, output);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!output[0] || !output[1] || (domain != 2 && output[2]) || output[1] > UINT64_MAX - bytes) {
            state = State::Faulted; return HSA_STATUS_ERROR;
        }
        buffer = {output[0], output[1], bytes};
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t copyRaw(uint64_t src, uint64_t srcOffset, uint64_t dst, uint64_t dstOffset, size_t size) {
        const std::array<uint64_t, 5> input{src, srcOffset, dst, dstOffset, size};
        uint64_t operation = UINT64_MAX;
        const auto status = scalar(48, input, {&operation, 1});
        if (status == HSA_STATUS_SUCCESS && !operation) return status;
        // A failed submission may still reference staging. Never free/reuse it.
        state = State::Faulted;
        return status == HSA_STATUS_SUCCESS ? HSA_STATUS_ERROR : status;
    }
    hsa_status_t transfer(const DeviceBuffer &buffer, uint64_t offset, void *host, size_t bytes, bool upload) {
        if (state != State::Ready) return HSA_STATUS_ERROR;
        if (!host || offset > buffer.size || bytes > buffer.size - offset)
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (!bytes) return HSA_STATUS_SUCCESS;
        if (!staging.handle) {
            const auto status = allocateRaw(16384, 1, staging);
            if (status != HSA_STATUS_SUCCESS) return status;
        }
        auto pointer = static_cast<uint8_t *>(host);
        std::array<uint8_t, 4096> chunk{};
        while (bytes) {
            const auto aligned = offset & ~uint64_t(3);
            const auto prefix = size_t(offset - aligned);
            const auto length = std::min(bytes, chunk.size() - prefix);
            const auto span = (prefix + length + 3) & ~size_t(3);
            hsa_status_t status = HSA_STATUS_SUCCESS;
            const std::array<uint64_t, 3> io{staging.handle, 0, span};
            // A partial dword write preserves neighboring bytes using read/modify/write.
            if (!upload || prefix || length != span) {
                status = copyRaw(buffer.handle, aligned, staging.handle, 0, span);
                if (status != HSA_STATUS_SUCCESS) return status;
                size_t returned = span;
                const auto result = IOConnectCallMethod(ownerPort, 50, io.data(), 3, nullptr, 0,
                                                        nullptr, nullptr, chunk.data(), &returned);
                if (result != KERN_SUCCESS || returned != span) { state = State::Faulted; return HSA_STATUS_ERROR; }
            }
            if (upload) {
                std::memcpy(chunk.data() + prefix, pointer, length);
                const auto result = IOConnectCallMethod(ownerPort, 49, io.data(), 3, chunk.data(), span,
                                                        nullptr, nullptr, nullptr, nullptr);
                if (result != KERN_SUCCESS) { state = State::Faulted; return HSA_STATUS_ERROR; }
                status = copyRaw(staging.handle, 0, buffer.handle, aligned, span);
                if (status != HSA_STATUS_SUCCESS) return status;
            } else std::memcpy(pointer, chunk.data() + prefix, length);
            offset += length; pointer += length; bytes -= length;
        }
        return HSA_STATUS_SUCCESS;
    }

public:
    hsa_status_t read(DeviceSnapshot &snapshot) override {
        // A registry reference does not keep a user client connected. Each probe
        // closes before returning, so idle discovery cannot block owner shutdown.
        std::lock_guard lock(sessionMutex);
        io_connect_t port = ownerPort;
        if (!port && IOServiceOpen(service, mach_task_self(), 0, &port) != KERN_SUCCESS)
            return HSA_STATUS_ERROR_INVALID_AGENT;
        struct Close { io_connect_t port; ~Close() { if (port) IOServiceClose(port); } } close{ownerPort ? 0 : port};
        uint64_t identity[3]{};
        auto status = call(port, 43, nullptr, 0, identity, 3);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (identity[0] != 0x414d444750554142ull || identity[1] != 1 || identity[2] < 172)
            return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
        uint64_t gfx[3]{}, vram[2]{}, stage = 0, tag = 1;
        status = call(port, 21, &tag, 1, gfx, 3);
        if (status != HSA_STATUS_SUCCESS) return status;
        tag = 2;
        status = call(port, 21, &tag, 1, vram, 2);
        if (status != HSA_STATUS_SUCCESS) return status;
        tag = 4;
        status = call(port, 21, &tag, 1, &stage, 1);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (gfx[0] > UINT32_MAX || gfx[1] > UINT32_MAX || gfx[2] > UINT32_MAX ||
            vram[0] > vram[1]) return HSA_STATUS_ERROR;
        snapshot = {registryID, identity[2], stage, vram[0], vram[1],
                    uint32_t(gfx[0]), uint32_t(gfx[1]), uint32_t(gfx[2])};
        return HSA_STATUS_SUCCESS;
    }
};
} // namespace

hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &connections) {
    io_iterator_t iterator = IO_OBJECT_NULL;
    const auto matching = IOServiceNameMatching("MacAMDGPU");
    if (!matching) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS)
        return HSA_STATUS_ERROR;
    IOObject iteratorOwner(iterator);
    try {
        for (io_service_t service; (service = IOIteratorNext(iterator));) {
            IOObject serviceOwner(service);
            auto connection = std::make_shared<IOKitConnection>();
            if (IORegistryEntryGetRegistryEntryID(service, &connection->registryID) != KERN_SUCCESS)
                return HSA_STATUS_ERROR;
            IOObjectRetain(service);
            connection->service = service;
            DeviceSnapshot snapshot;
            const auto status = connection->read(snapshot);
            if (status != HSA_STATUS_SUCCESS) return status;
            connections.push_back(std::move(connection));
        }
    } catch (const std::bad_alloc &) {
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    return HSA_STATUS_SUCCESS;
}
} // namespace mac_hsa
