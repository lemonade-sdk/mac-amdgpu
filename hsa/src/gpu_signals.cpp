#include "runtime_state.h"
#include "code_object.h"
#include "signal_operations_code.h"
#include <array>
#include <map>

namespace mac_hsa::detail {
namespace {
struct GPUSignalContext {
    std::shared_ptr<Connection> connection;
    DeviceBuffer code,arguments;
    SharedBuffer result,arena;
    CodeObject object;
    std::mutex slotsMutex,operationsMutex;
    std::array<bool,256> used{};
    std::atomic<bool> faulted{false};
    std::array<std::weak_ptr<Signal>,256> signals;
    explicit GPUSignalContext(std::shared_ptr<Connection> c):connection(std::move(c)) {}
    ~GPUSignalContext() {
        if (arguments.handle) connection->freeBuffer(arguments);
        if (code.handle) connection->freeBuffer(code);
        if (result.host) connection->freeSharedBuffer(result);
        if (arena.host) connection->freeSharedBuffer(arena);
    }
    hsa_status_t initialize() {
        DeviceSnapshot snapshot;
        auto status=connection->read(snapshot);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!supportsPersistentQueues(snapshot))
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        if (!parseCodeObject(kSignalOperationsCodeObject,object) || object.kernels.size()!=1)
            return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
        const auto &kernel=object.kernels[0];
        if (kernel.kernargSize!=36 || kernel.properties!=0x408 || kernel.privateSize || kernel.groupSize || kernel.preload)
            return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
        status=connection->allocateBuffer(object.image.size(),code);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!relocateCodeObject(object,code.address)) return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
        status=connection->writeBuffer(code,0,object.image.data(),object.image.size());
        if (status!=HSA_STATUS_SUCCESS) return status;
        status=connection->allocateBuffer(16384,arguments);
        if (status!=HSA_STATUS_SUCCESS) return status;
        status=connection->allocateSharedBuffer(16384,result);
        if (status!=HSA_STATUS_SUCCESS) return status;
        status=connection->allocateSharedBuffer(16384,arena);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!arena.host || arena.device.size<16384 || arena.device.address!=reinterpret_cast<uintptr_t>(arena.host) ||
            !result.host || result.device.size<8 || result.device.address!=reinterpret_cast<uintptr_t>(result.host))
            return HSA_STATUS_ERROR;
        return HSA_STATUS_SUCCESS;
    }
    bool fail() {
        std::array<std::shared_ptr<Signal>,256> affected;
        {
            std::lock_guard lock(slotsMutex);
            faulted=true;
            for (unsigned i=0;i<signals.size();++i) affected[i]=signals[i].lock();
        }
        // Release strong references outside slotsMutex: a signal's final
        // destructor returns its slot under that same mutex.
        for (auto &signal:affected) if (signal) {
            signal->alive=false;signal->changed.notify_all();
        }
        return false;
    }
    bool execute(unsigned slot,unsigned operation,int64_t value,int64_t compare,int64_t &old) {
        std::lock_guard lock(operationsMutex);
        if (faulted || operation<1 || operation>8) return false;
        const uint64_t address=arena.device.address+slot*sizeof(SignalABI)+offsetof(SignalABI,value);
        std::array<uint8_t,36> args{};
        std::memcpy(args.data(),&address,8);std::memcpy(args.data()+8,&result.device.address,8);
        std::memcpy(args.data()+16,&value,8);std::memcpy(args.data()+24,&compare,8);
        const uint32_t op=operation;std::memcpy(args.data()+32,&op,4);
        auto status=connection->writeBuffer(arguments,0,args.data(),args.size());
        if (status!=HSA_STATUS_SUCCESS) return fail();
        amdgpu::AQLDispatchRequest request{};
        request.version=1;request.codeHandle=code.handle;request.descriptorOffset=object.kernels[0].descriptor;
        request.kernargHandle=arguments.handle;request.kernargBytes=args.size();request.timeoutUS=100000;
        for (unsigned i=0;i<3;++i) request.groups[i]=request.threads[i]=1;
        request.threads[0]=32;request.buffers[0]=result.device.handle;request.buffers[1]=arena.device.handle;
        uint64_t completion=UINT64_MAX;
        status=connection->dispatchAQL(request,completion);
        if (status!=HSA_STATUS_SUCCESS || completion) return fail();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::memcpy(&old,result.host,8);return true;
    }
};
std::mutex contextsMutex;
std::map<Connection *,std::weak_ptr<GPUSignalContext>> contexts;
struct SignalSlot {
    std::shared_ptr<GPUSignalContext> context;
    unsigned index;
    SignalSlot(std::shared_ptr<GPUSignalContext> c,unsigned i):context(std::move(c)),index(i) {}
    ~SignalSlot() {std::lock_guard lock(context->slotsMutex);context->signals[index].reset();context->used[index]=false;}
};
}
void invalidateGPUSignals(const std::shared_ptr<Connection> &connection) {
    std::shared_ptr<GPUSignalContext> context;
    {
        std::lock_guard lock(contextsMutex);
        const auto found=contexts.find(connection.get());
        if (found!=contexts.end()) context=found->second.lock();
    }
    if (context) context->fail();
}
hsa_status_t createGPUSignalBacking(const std::shared_ptr<Connection> &connection,int64_t initial,const std::shared_ptr<Signal> &signal) {
    std::shared_ptr<GPUSignalContext> context;
    {
        std::lock_guard lock(contextsMutex);
        std::erase_if(contexts,[](const auto &entry) {return entry.second.expired();});
        context=contexts[connection.get()].lock();
        if (!context) {
            context=std::make_shared<GPUSignalContext>(connection);
            const auto status=context->initialize();
            if (status!=HSA_STATUS_SUCCESS) return status;
            contexts[connection.get()]=context;
        }
    }
    std::unique_lock lock(context->slotsMutex);
    if (context->faulted) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    unsigned slot=0;while (slot<context->used.size() && context->used[slot]) ++slot;
    if (slot==context->used.size()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    context->used[slot]=true;lock.unlock();
    std::shared_ptr<SignalSlot> backing;
    try {backing=std::make_shared<SignalSlot>(context,slot);}
    catch (...) {std::lock_guard rollback(context->slotsMutex);context->used[slot]=false;throw;}
    auto *abi=static_cast<SignalABI *>(context->arena.host)+slot;
    *abi={};abi->kind=1;abi->value=initial;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    signal->gpuAtomic=[context,slot](unsigned op,int64_t value,int64_t compare,int64_t &old) {
        return context->execute(slot,op,value,compare,old);
    };
    signal->sharedStorage=std::move(backing);signal->sharedABI=abi;
    {
        std::lock_guard publish(context->slotsMutex);
        if (context->faulted) {signal->alive=false;return HSA_STATUS_ERROR_OUT_OF_RESOURCES;}
        context->signals[slot]=signal;
    }
    return HSA_STATUS_SUCCESS;
}
}
