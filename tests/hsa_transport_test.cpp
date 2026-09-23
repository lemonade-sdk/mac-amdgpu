#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <cassert>
#include <array>
#include <map>
#include <vector>
#include <cstring>
#include <thread>
#include <atomic>
#include <algorithm>
#include "../dext/amdgpu/amdgpu_dispatch_abi.h"

namespace {
struct Buffer { std::vector<uint8_t> bytes; uint64_t domain; };
unsigned opens = 0, closes = 0, resets = 0, uploads = 0, maps = 0, unmaps = 0;
bool busy = true, copyFailure = false, malformedRead = false;
uint64_t stage = 15, nextHandle = 0;
uint64_t driverBuild = 179, exportedBuffer = 0, exportToken[2]{};
std::map<uint64_t, Buffer> buffers;
std::vector<uint8_t> dma(32 << 20);
alignas(16384) uint8_t sharedStorage[65536];
bool sharedMapFailure = false;
uint64_t sharedBase = reinterpret_cast<uintptr_t>(sharedStorage) & ~uint64_t((1ull << 28) - 1);
unsigned hostChecks = 0, sharedMaps = 0, sharedUnmaps = 0;
std::vector<std::array<uint64_t, 10>> atomicPackets;
bool atomicTimeout = false, streamLive = false;
unsigned submittedStreams = 0;
unsigned computeCalls = 0, computeFault = 0;
kern_return_t mockOpen(io_service_t, task_port_t, uint32_t, io_connect_t *port) { *port = ++opens; return KERN_SUCCESS; }
kern_return_t mockClose(io_connect_t) { ++closes; return KERN_SUCCESS; }
kern_return_t mockRelease(io_object_t) { return KERN_SUCCESS; }
kern_return_t mockScalar(mach_port_t, uint32_t selector, const uint64_t *in, uint32_t count,
                         uint64_t *out, uint32_t *outCount) {
    switch (selector) {
    case 37:
        assert(count == 2 && *outCount == 1 && in[0] == 0 && in[1] == 0 && !streamLive);
        streamLive = true; atomicPackets.clear(); out[0] = 77; break;
    case 38: {
        assert(streamLive && count == 10 && *outCount == 0 && in[0] == 77 && in[9] == 8);
        std::array<uint64_t, 10> packet; std::copy_n(in, 10, packet.begin());
        assert(packet[1] == 0x5e00000a && !packet[6] && !packet[7] && !packet[8]);
        atomicPackets.push_back(packet); break;
    }
    case 19:
        assert(streamLive && count == 1 && in[0] == 77 && *outCount == 1);
        ++submittedStreams; out[0] = 77;
        for (const auto &packet : atomicPackets) {
            const uint64_t address = packet[2] | (packet[3] << 32);
            assert(address >= reinterpret_cast<uintptr_t>(sharedStorage) &&
                address <= reinterpret_cast<uintptr_t>(sharedStorage) + sizeof(sharedStorage) - 8);
            std::atomic_ref<int64_t>(*reinterpret_cast<int64_t *>(address)).fetch_add(
                static_cast<int64_t>(packet[4] | (packet[5] << 32)));
        }
        break;
    case 20:
        assert(streamLive && count == 2 && in[0] == 77 && in[1] == 1000000000 && *outCount == 1);
        out[0] = atomicTimeout; break;
    case 39:
        assert(streamLive && count == 1 && in[0] == 77 && *outCount == 0);
        streamLive = false; break;
    case 43: assert(*outCount == 3); out[0] = 0x414d444750554142ull; out[1] = 1; out[2] = driverBuild; break;
    case 1:
        if (busy) return kIOReturnBusy;
        assert(*outCount == 7); out[3] = 0x1002; out[4] = 0x7551; out[6] = 0xc0; break;
    case 21:
        assert(count == 1);
        if (in[0] == 4) out[0] = stage;
        else if (in[0] == 1) { out[0] = 12; out[1] = 0; out[2] = 1; }
        else if (in[0] == 2) { out[0] = 256ull << 20; out[1] = 32ull << 30; }
        else if (in[0] == 3) { out[0] = 0; out[1] = 0x070001; out[2] = out[3] = 0x0e0003; }
        else { assert(in[0] == 5); out[0] = out[1] = 1; out[10] = 31ull << 30; }
        break;
    case 6: out[0] = 1; out[1] = 0x80000000; break;
    case 8: ++resets; break;
    case 9: stage = in[0]; out[0] = stage; break;
    case 10: ++uploads; assert(maps == unmaps + 1 && in[1] > 0); out[0] = 0; break;
    case 16: {
        assert(count == 4 && *outCount == 3 && in[2] == 16384 && in[3] == 0);
        auto handle = ++nextHandle;
        buffers.emplace(handle, Buffer{std::vector<uint8_t>(in[0], 0x91), in[1]});
        out[0] = handle;
        out[1] = in[1] == 2 ? reinterpret_cast<uintptr_t>(sharedStorage) : 0x8001000000ull + handle * 0x100000;
        out[2] = in[1] == 2 ? 0x12340000 : 0;
        break;
    }
    case 17: assert(buffers.erase(in[0]) == 1); break;
    case 54:
        assert(count == 1 && *outCount == 3 && in[0] == 0);
        out[0] = sharedBase; out[1] = 1ull << 28; out[2] = 0; break;
    case 44:
        assert(count == 1 && *outCount == 6); ++hostChecks;
        out[0] = 0; out[1] = 8; out[2] = 0; break;
    case 36:
        assert(count == 1 && *outCount == 2 && buffers.at(in[0]).domain == 2);
        out[0] = 1000 + in[0]; out[1] = buffers.at(in[0]).bytes.size(); break;
    case 52:
        assert(count == 3 && *outCount == 3 && buffers.at(in[0]).domain == 3 && (in[1] || in[2]));
        exportedBuffer = in[0]; exportToken[0] = out[0] = in[1]; exportToken[1] = out[1] = in[2];
        out[2] = buffers.at(in[0]).bytes.size(); break;
    case 53:
        assert(count == 3 && *outCount == 3);
        if (in[0] != exportToken[0] || in[1] != exportToken[1] || !buffers.contains(exportedBuffer) ||
            in[2] != buffers.at(exportedBuffer).bytes.size()) return kIOReturnBadArgument;
        out[0] = ++nextHandle; out[1] = 0x8001000000ull + exportedBuffer * 0x100000; out[2] = in[2];
        buffers.emplace(out[0], buffers.at(exportedBuffer)); break;
    case 48: {
        assert(count == 5 && *outCount == 1);
        if (copyFailure) { out[0] = kIOReturnTimeout; break; }
        auto &src = buffers.at(in[0]).bytes, &dst = buffers.at(in[2]).bytes;
        assert(in[1] + in[4] <= src.size() && in[3] + in[4] <= dst.size());
        assert(in[4] && !(in[1] % 4) && !(in[3] % 4) && !(in[4] % 4));
        auto *source = buffers.at(in[0]).domain == 2 ? sharedStorage : src.data();
        auto *destination = buffers.at(in[2]).domain == 2 ? sharedStorage : dst.data();
        std::memmove(destination + in[3], source + in[1], in[4]); out[0] = 0;
        break;
    }
    default: assert(false);
    }
    return KERN_SUCCESS;
}
kern_return_t mockMap(io_connect_t, uint32_t memory, task_port_t, mach_vm_address_t *address,
                      mach_vm_size_t *size, IOOptionBits options) {
    if (memory >= 1000) {
        assert(options == 0 && *address == reinterpret_cast<uintptr_t>(sharedStorage));
        if (sharedMapFailure) return KERN_NO_SPACE;
        ++sharedMaps; *size = buffers.at(memory - 1000).bytes.size(); return KERN_SUCCESS;
    }
    assert(memory == 6 && options == kIOMapAnywhere);
    ++maps; *address = reinterpret_cast<mach_vm_address_t>(dma.data()); *size = dma.size(); return KERN_SUCCESS;
}
kern_return_t mockUnmap(io_connect_t, uint32_t memory, task_port_t, mach_vm_address_t address) {
    if (memory >= 1000) {
        assert(address == reinterpret_cast<uintptr_t>(sharedStorage)); ++sharedUnmaps; return KERN_SUCCESS;
    }
    assert(memory == 6 && address == reinterpret_cast<uintptr_t>(dma.data())); ++unmaps; return KERN_SUCCESS;
}
kern_return_t mockMethod(mach_port_t, uint32_t selector, const uint64_t *in, uint32_t count,
    const void *structureIn, size_t inSize, uint64_t *scalarOut, uint32_t *scalarCount, void *structureOut, size_t *outSize) {
    if (selector == 51) {
        assert(count == 0 && in == nullptr);
        const auto &request = *static_cast<const amdgpu::ComputeDispatchRequest *>(structureIn);
        assert(inSize == (request.version == 1 ? amdgpu::kComputeDispatchV1Bytes : sizeof(request)));
        assert(amdgpu::compute_dispatch_shape(request) && buffers.contains(request.codeHandle));
        assert(*scalarCount == 3 && structureOut == nullptr && outSize == nullptr);
        ++computeCalls;
        scalarOut[0] = computeFault == 1 ? kIOReturnTimeout : 0;
        scalarOut[1] = computeFault == 2 ? 0 : computeCalls;
        scalarOut[2] = computeFault == 3 ? 2 : 3;
        if (computeFault == 4) *scalarCount = 2;
        return computeFault == 5 ? kIOReturnTimeout : KERN_SUCCESS;
    }
    assert(count == 3 && in[1] == 0 && in[2] && in[2] <= 4096 && in[2] % 4 == 0);
    auto &buffer = buffers.at(in[0]); assert(buffer.domain == 1);
    if (selector == 49) { assert(inSize == in[2]); std::memcpy(buffer.bytes.data(), structureIn, inSize); }
    else {
        assert(selector == 50 && *outSize == in[2]);
        std::memcpy(structureOut, buffer.bytes.data(), *outSize);
        if (malformedRead) --*outSize;
    }
    return KERN_SUCCESS;
}
}
#define IOServiceOpen mockOpen
#define IOServiceClose mockClose
#define IOObjectRelease mockRelease
#define IOConnectCallScalarMethod mockScalar
#define IOConnectMapMemory64 mockMap
#define IOConnectUnmapMemory64 mockUnmap
#define IOConnectCallMethod mockMethod
#include "transport_iokit.cpp"
#undef IOServiceOpen
#undef IOServiceClose
#undef IOObjectRelease
#undef IOConnectCallScalarMethod
#undef IOConnectMapMemory64
#undef IOConnectUnmapMemory64
#undef IOConnectCallMethod

int main() {
    {
        mac_hsa::IOKitConnection connection; connection.service = 123; connection.registryID = 456;
        mac_hsa::DeviceSnapshot snapshot;
        assert(connection.read(snapshot) == 0 && snapshot.stage == 15);
        assert(opens == closes && resets == 0 && uploads == 0);
        uint64_t capacity = 0;
        assert(connection.memoryCapacity(capacity) == HSA_STATUS_ERROR_OUT_OF_RESOURCES);
        assert(opens == closes && resets == 0 && uploads == 0);
        busy = false; stage = 0;
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 4; ++i) threads.emplace_back([&] {
            uint64_t local = 0;
            assert(connection.memoryCapacity(local) == 0 && local == 31ull << 30);
        });
        for (auto &thread : threads) thread.join();
        assert(resets == 1 && uploads == 10 && maps == 10 && unmaps == maps);
        assert(opens == closes + 1);
        const auto priorOpens = opens;
        assert(connection.read(snapshot) == 0 && snapshot.stage == 15 && opens == priorOpens);
        mac_hsa::DeviceBuffer device;
        assert(connection.allocateBuffer(16385, device) == 0 && device.size == 32768);
        amdgpu::ComputeDispatchRequest dispatch{};
        dispatch.version = 1; dispatch.codeHandle = device.handle; dispatch.codeBytes = 256;
        dispatch.groups[0] = dispatch.groups[1] = dispatch.groups[2] = 1;
        dispatch.threads[0] = 32; dispatch.threads[1] = dispatch.threads[2] = 1;
        dispatch.rsrc1 = 0xc0000; dispatch.timeoutUS = 100000;
        uint64_t fence = 0;
        assert(connection.dispatch(dispatch, fence) == 0 && fence == computeCalls);
        const auto previous = fence;
        assert(connection.dispatch(dispatch, fence) == 0 && fence > previous);
        dispatch.version = 3;
        assert(connection.dispatch(dispatch, fence) == HSA_STATUS_ERROR_INVALID_ARGUMENT && !fence);
        dispatch.version = 2; dispatch.rsrc1 = 0xe00f0000; dispatch.rsrc3 = 0x10;
        assert(connection.dispatch(dispatch, fence) == HSA_STATUS_ERROR_OUT_OF_RESOURCES && !fence);
        driverBuild = 183;
        assert(connection.dispatch(dispatch, fence) == 0 && fence > previous);
        driverBuild = 179;
        mac_hsa::BufferToken token{};
        assert(connection.exportBuffer(device, token) == HSA_STATUS_ERROR_OUT_OF_RESOURCES && !exportedBuffer);
        driverBuild = 181;
        assert(connection.exportBuffer(device, token) == 0 && token.registryID == 456 && token.size == device.size);
        {
            mac_hsa::IOKitConnection importer; importer.service = 123; importer.registryID = 456;
            mac_hsa::DeviceBuffer imported;
            const auto beforeResets = resets, beforeUploads = uploads;
            assert(importer.importBuffer(token, imported) == 0 && imported.address == device.address && imported.size == device.size);
            assert(resets == beforeResets && uploads == beforeUploads);
            assert(importer.freeBuffer(imported) == 0);
        }
        {
            mac_hsa::IOKitConnection stopped; stopped.service = 123; stopped.registryID = 456;
            stage = 0; const auto beforeResets = resets;
            mac_hsa::DeviceBuffer imported;
            assert(stopped.importBuffer(token, imported) == HSA_STATUS_ERROR_INVALID_ARGUMENT && resets == beforeResets);
            stage = 15;
        }
        mac_hsa::SharedBuffer shared;
        assert(connection.allocateSharedBuffer(16384, shared) == HSA_STATUS_ERROR_OUT_OF_RESOURCES && !hostChecks);
        driverBuild = 182;
        assert(connection.allocateSharedBuffer(16384, shared) == 0 && hostChecks == 1);
        assert(shared.host == sharedStorage && shared.device.address == reinterpret_cast<uintptr_t>(shared.host));
        for (size_t i = 0; i < shared.device.size; ++i) assert(sharedStorage[i] == 0);
        auto &signalWord = *reinterpret_cast<int64_t *>(sharedStorage + 64);
        signalWord = 1;
        assert(connection.testSharedAtomicAdd(shared, 64, -1, 1) == 0 && signalWord == 0 && !streamLive);
        signalWord = 0xffffffffll;
        assert(connection.testSharedAtomicAdd(shared, 64, 1, 64) == 0 && signalWord == 0x10000003fll);
        const auto beforeInvalid = submittedStreams;
        assert(connection.testSharedAtomicAdd(shared, 3, 1, 1) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
        assert(connection.testSharedAtomicAdd(shared, shared.device.size, 1, 1) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
        assert(connection.testSharedAtomicAdd(shared, 0, 1, 0) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
        assert(connection.testSharedAtomicAdd(shared, 0, 1, 65) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
        auto forged = shared; forged.device.address += 8;
        assert(connection.testSharedAtomicAdd(forged, 0, 1, 1) == HSA_STATUS_ERROR_INVALID_ALLOCATION);
        assert(submittedStreams == beforeInvalid);
        std::memset(sharedStorage, 0x79, shared.device.size);
        assert(connection.copyBuffers(shared.device, 0, device, 0, shared.device.size) == 0);
        assert(buffers.at(device.handle).bytes[16383] == 0x79);
        std::memset(sharedStorage, 0, shared.device.size);
        assert(connection.copyBuffers(device, 0, shared.device, 0, shared.device.size) == 0 && sharedStorage[16383] == 0x79);
        assert(connection.freeBuffer(shared.device) == HSA_STATUS_ERROR_INVALID_ALLOCATION);
        assert(connection.copyBuffers(shared.device, 0, shared.device, 4, 16) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
        auto stale = shared;
        assert(connection.freeSharedBuffer(shared) == 0 && sharedMaps == sharedUnmaps);
        assert(connection.freeSharedBuffer(stale) == HSA_STATUS_ERROR_INVALID_ALLOCATION);
        sharedMapFailure = true;
        const auto priorBuffers = buffers.size();
        assert(connection.allocateSharedBuffer(16384, shared) == HSA_STATUS_ERROR_OUT_OF_RESOURCES && buffers.size() == priorBuffers);
        sharedMapFailure = false;
        // Restore device guards for the independent unaligned-copy test below.
        std::fill(buffers.at(device.handle).bytes.begin(), buffers.at(device.handle).bytes.end(), 0x91);
        driverBuild = 179;
        std::vector<uint8_t> source(12003), destination(source.size());
        for (size_t i = 0; i < source.size(); ++i) source[i] = uint8_t(i * 113);
        assert(connection.writeBuffer(device, 3, source.data(), source.size()) == 0);
        assert(connection.readBuffer(device, 3, destination.data(), destination.size()) == 0 && source == destination);
        const auto &stored = buffers.at(device.handle).bytes;
        for (unsigned i = 0; i < 3; ++i) assert(stored[i] == 0x91);
        for (size_t i = 3 + source.size(); i < stored.size(); ++i) assert(stored[i] == 0x91);
        assert(connection.writeBuffer(device, device.size - 1, source.data(), 2) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
        assert(connection.freeBuffer(device) == 0);
        assert(connection.allocateBuffer(16384, device) == 0);
        copyFailure = true;
        assert(connection.writeBuffer(device, 0, source.data(), 4) != 0);
        const auto retained = buffers.size();
        assert(connection.freeBuffer(device) != 0 && buffers.size() == retained);
        assert(connection.readBuffer(device, 0, destination.data(), 4) != 0);
        assert(connection.memoryCapacity(capacity) != 0 && resets == 1);
    }
    assert(opens == closes);
    {
        driverBuild = 182;
        mac_hsa::IOKitConnection connection; connection.service = 123; connection.registryID = 456;
        mac_hsa::SharedBuffer shared;
        assert(connection.allocateSharedBuffer(16384, shared) == 0);
        atomicTimeout = true;
        assert(connection.testSharedAtomicAdd(shared, 64, -1, 1) == HSA_STATUS_ERROR && streamLive);
        const auto retained = buffers.size();
        assert(connection.freeSharedBuffer(shared) == HSA_STATUS_ERROR && buffers.size() == retained);
        assert(connection.testSharedAtomicAdd(shared, 64, 1, 1) == HSA_STATUS_ERROR);
    }
    assert(opens == closes && sharedMaps == sharedUnmaps);
    for (computeFault = 1; computeFault <= 5; ++computeFault) {
        mac_hsa::IOKitConnection connection; connection.service = 123; connection.registryID = 456;
        mac_hsa::DeviceBuffer code;
        assert(connection.allocateBuffer(16384, code) == 0);
        amdgpu::ComputeDispatchRequest request{};
        request.version = 1; request.codeHandle = code.handle; request.codeBytes = 256; request.timeoutUS = 100000;
        request.groups[0] = request.groups[1] = request.groups[2] = 1;
        request.threads[0] = 32; request.threads[1] = request.threads[2] = 1;
        request.rsrc1 = 0xc0000;
        uint64_t fence = 123;
        assert(connection.dispatch(request, fence) == HSA_STATUS_ERROR && !fence);
        assert(connection.freeBuffer(code) == HSA_STATUS_ERROR && buffers.contains(code.handle));
        assert(connection.dispatch(request, fence) == HSA_STATUS_ERROR);
    }
    puts("HSA: transient observers, owner Busy without reset, single concurrent initialization, firmware mapping, unaligned SDMA staging/guards and fault retention pass");
}
