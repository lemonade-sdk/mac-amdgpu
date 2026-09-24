#include "amdgpu_software_stats.h"
#include <cassert>
#include <cstring>
#include <cstdio>
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNoMemory=2;
constexpr int kMacAMDGPUMethodSoftwareSnapshot=61;
namespace amdgpu {
constexpr unsigned kPersistentAQLQueues=2;
enum class BringupStage {None,SDMAInit};
struct Queue {uint64_t handle=0,lastDoorbell=0,read=0;bool mapped=false,published=false;};
static unsigned cpuReads;
bool aql_queue_cpu_read_index(const Queue &q,uint64_t &value) {++cpuReads;value=q.read;return true;}
}
struct MacAMDGPU_IVars {
    bool pciOpen=true,stopping=false,shutdownBlocked=false,shutdownInProgress=false;
    struct {unsigned participants=2;} sessions;
    struct {amdgpu::BringupStage reached=amdgpu::BringupStage::SDMAInit;amdgpu::Queue aqlQueues[2];} bringup;
    amdgpu::software_stats::Counters softwareStats;
    amdgpu::software_stats::QueueProgress softwareQueues[2];
};
#include "software_queue_observer.inc"
static uint64_t now=100;
static uint64_t testClock(int) {return now;}
#define clock_gettime_nsec_np testClock
#define CLOCK_UPTIME_RAW 0
struct OSData {
    amdgpu::software_stats::Snapshot snapshot;
    static OSData *withBytes(const void *source,size_t bytes) {
        assert(bytes==sizeof(amdgpu::software_stats::Snapshot));
        auto *out=new OSData;memcpy(&out->snapshot,source,bytes);return out;
    }
};
struct Args {
    unsigned scalarInputCount=0,scalarOutputCount=0;
    void *structureInput=nullptr,*structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr;
    size_t structureOutputMaximumSize=sizeof(amdgpu::software_stats::Snapshot);
    OSData *structureOutput=nullptr;
};
struct Driver {MacAMDGPU_IVars *ivars;};
static int call(Driver *driver,unsigned selector,Args *arguments) {
    switch(selector) {
#include "software_snapshot_rpc.inc"
    default:return kIOReturnBadArgument;
    }
}
int main() {
    using namespace amdgpu::software_stats;
    MacAMDGPU_IVars state;Driver driver{&state};Args args;
    state.softwareStats.reset(now);
    args.scalarInputCount=1;
    assert(call(&driver,61,&args)==kIOReturnBadArgument && !amdgpu::cpuReads);
    args.scalarInputCount=0;args.structureOutputMaximumSize=sizeof(amdgpu::software_stats::Snapshot)-1;
    assert(call(&driver,61,&args)==kIOReturnBadArgument && !amdgpu::cpuReads);
    args.structureOutputMaximumSize=sizeof(amdgpu::software_stats::Snapshot);
    state.bringup.aqlQueues[0]={1,2,1,true,true};
    state.bringup.aqlQueues[1]={2,0,0,true,true};
    auto get=[&] {
        assert(call(&driver,61,&args)==0 && args.structureOutput);
        auto s=args.structureOutput->snapshot;delete args.structureOutput;args.structureOutput=nullptr;
        assert(valid(s));return s;
    };
    auto s=get();
    assert(s.activeQueues==2 && s.queuedPackets==3 && s.publishedPackets==4 && s.consumedPackets==1);
    assert(s.participants==2 && (s.flags&RuntimeReady));
    state.bringup.aqlQueues[0].read=3;++now;s=get();
    assert(s.consumedPackets==3 && s.queuedPackets==1 && !s.engines[AQL].completed);
    state.bringup.aqlQueues[0].read=UINT64_MAX;s=get();
    assert((s.flags&QueueSampleIncomplete) && s.consumedPackets==3 && s.queuedPackets==1);
    state.bringup.aqlQueues[0]={};state.pciOpen=false;state.shutdownBlocked=true;s=get();
    assert(s.activeQueues==1 && !(s.flags&RuntimeReady) && s.publishedPackets==4);
    assert(!(s.flags&QueueSampleIncomplete));
    puts("Software observer RPC: bounded CPU queue reads, ABI validation, queue consumption and closed-session snapshot pass");
}
