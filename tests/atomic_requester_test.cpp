#include "amdgpu_atomic_requester.h"
#include "amdgpu_client_lifecycle.h"
#include "amdgpu_ip.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNotReady=2,kIOReturnBusy=3,kIOReturnNotAttached=4;
constexpr unsigned kMacAMDGPUMethodAtomicRequesterExperiment=60;
#define MACAMDGPU_LOG(...) ((void)0)
struct PCI {
    uint32_t identity=0x75511002;uint16_t flags=0x12,control=0x420,transactionStatus=0;
    uint64_t capability=0x80;unsigned reads=0,writes=0;
    bool refuseEnable=false,refuseRestore=false,readFailure=false;
    amdgpu::atomic_requester::Experiment *experiment=nullptr;
    int FindPCICapability(unsigned id,unsigned,uint64_t *cap) {assert(id==0x10);*cap=capability;return 0;}
    void ConfigurationRead32(uint64_t offset,uint32_t *out) {assert(offset==0);++reads;*out=identity;}
    void ConfigurationRead16(uint64_t offset,uint16_t *out) {
        ++reads;
        if(offset==capability) *out=0x10;
        else if(offset==capability+2) *out=flags;
        else if(offset==capability+0x0a) *out=transactionStatus;
        else {assert(offset==capability+0x28);*out=readFailure ? UINT16_MAX : control;}
    }
    void ConfigurationWrite16(uint64_t offset,uint16_t value) {
        assert(offset==capability+0x28 && !((value^control)&~0x40));
        assert(experiment && experiment->owner && experiment->pending); // recovery saved before write
        ++writes;
        if((value&0x40) ? !refuseEnable : !refuseRestore) control=value;
    }
};
namespace amdgpu { enum class BringupStage { None,SDMAInit }; }
struct State {
    bool pciOpen=true,shutdownInProgress=false,shutdownBlocked=false;
    uint16_t deviceID=0x7551;PCI *retainedPCI=nullptr;
    amdgpu::ClientSessions sessions;amdgpu::ClientSubmission submission;
    amdgpu::atomic_requester::Experiment atomicRequester;
    struct {
        struct {PCI *pci=nullptr;amdgpu::IPBaseTable ip;} device;
        amdgpu::BringupStage reached=amdgpu::BringupStage::SDMAInit;
        struct Queue {void *owner=nullptr;uint64_t handle=0;bool mapped=false,retained=false;};
        std::array<Queue,1> aqlQueues;
    } bringup;
};
struct Driver {State *ivars;};
struct ClientState {bool claimed=false,mappedBAR=false;void *irqQueue=nullptr,*pendingInterruptNotify=nullptr,*interruptSources[2]{};};
struct Args {
    uint64_t *scalarInput,*scalarOutput;unsigned scalarInputCount=1,scalarOutputCount=8;
    void *structureInput=nullptr,*structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr;
    size_t structureOutputMaximumSize=0;
};
struct Client {
    ClientState *ivars;
    int call(Driver *driver,PCI *pci,Args *arguments) {
        switch(kMacAMDGPUMethodAtomicRequesterExperiment) {
#include "atomic_requester_rpc.inc"
        default:return kIOReturnBadArgument;
        }
    }
};
struct Fixture {
    PCI pci;State state;Driver driver{&state};ClientState clientState;Client client{&clientState};
    uint64_t enable=1;amdgpu::atomic_requester::Snapshot output{};Args args{&enable,output.values};
    Fixture() {
        state.retainedPCI=state.bringup.device.pci=&pci;pci.experiment=&state.atomicRequester;
        state.bringup.device.ip.setVersion(amdgpu::IPBlock::GC,{12,0,1});
        assert(state.sessions.attach(&client,clientState.claimed,true));
    }
    int call() {return client.call(&driver,&pci,&args);}
};
int main() {
    using namespace amdgpu::atomic_requester;
    {
        Fixture f;assert(f.call()==0 && !f.output.values[Status] && valid(f.output));
        assert(f.output.values[Before]==0x420 && f.output.values[Requested]==0x460 && f.output.values[Observed]==0x460);
        assert(f.output.values[Original]==0x420 && f.output.values[Active] && f.output.values[RestorePending]);
        assert(f.state.sessions.exclusiveClient==&f.client);
        const auto before=f.pci.writes;assert(f.call()==kIOReturnBadArgument && f.pci.writes==before);
        bool peerAttached=false;assert(!f.state.sessions.attach(&f,peerAttached,true));
        f.pci.control|=0x100;f.enable=0;assert(f.call()==0 && !f.output.values[Status]);
        assert(f.pci.control==0x520 && f.output.values[Original]==0x420 && !f.output.values[Active] && !f.output.values[RestorePending]);
        assert(!f.state.atomicRequester.owner && !f.state.sessions.exclusiveClient);
        assert(f.state.sessions.attach(&f,peerAttached,true));
    }
    // An existing independent exclusive lease must survive explicit restoration.
    {
        Fixture f;assert(f.state.sessions.claimExclusive(&f.client,f.clientState.claimed));
        assert(f.call()==0);f.enable=0;assert(f.call()==0);
        assert(f.state.sessions.exclusiveClient==&f.client);
    }
    for(unsigned bad=0;bad<20;++bad) {
        Fixture f;std::memset(&f.output,0xa5,sizeof(f.output));const auto unchanged=f.output;
        switch(bad) {
        case 0:f.enable=2;break;case 1:f.args.scalarInputCount=0;break;case 2:f.args.scalarOutputCount=7;break;
        case 3:f.args.structureInput=&f;break;case 4:f.clientState.claimed=false;break;
        case 5:f.state.pciOpen=false;break;case 6:f.state.shutdownInProgress=true;break;
        case 7:f.state.retainedPCI=nullptr;break;case 8:f.state.bringup.device.pci=nullptr;break;
        case 9:f.state.bringup.reached=amdgpu::BringupStage::None;break;case 10:f.state.deviceID=0x7550;break;
        case 11:f.state.bringup.device.ip.setVersion(amdgpu::IPBlock::GC,{12,0,0});break;
        case 12:f.state.shutdownBlocked=true;break;case 13:f.state.sessions.participants=2;break;
        case 14:f.clientState.mappedBAR=true;break;case 15:f.clientState.irqQueue=&f;break;
        case 16:f.clientState.pendingInterruptNotify=&f;break;case 17:f.clientState.interruptSources[1]=&f;break;
        case 18:f.state.bringup.aqlQueues[0].owner=&f;break;case 19:f.state.submission.pending=true;break;
        }
        assert(f.call()!=0 && !f.pci.reads && !f.pci.writes && !f.state.atomicRequester.owner);
        assert(std::memcmp(&f.output,&unchanged,sizeof(f.output))==0);
    }
    for(unsigned malformed=0;malformed<6;++malformed) {
        Fixture f;
        switch(malformed) {
        case 0:f.pci.identity=UINT32_MAX;break;case 1:f.pci.identity=0x75521002;break;
        case 2:f.pci.flags=0x11;break;case 3:f.pci.flags=0x42;break;
        case 4:f.pci.capability=0xd8;break;case 5:f.pci.readFailure=true;break;
        }
        const auto status=f.call();
        if(malformed==5) assert(status==0 && f.output.values[Status] && valid(f.output));
        else assert(status==kIOReturnNotReady);
        assert(!f.pci.writes && !f.state.atomicRequester.owner && !f.state.sessions.exclusiveClient);
    }
    for(bool ending:{false,true}) for(uint16_t pending:{uint16_t(0x20),uint16_t(UINT16_MAX)}) {
        Fixture f;if(ending) {assert(f.call()==0);f.enable=0;}
        f.pci.transactionStatus=pending;const auto before=f.pci.writes;
        assert(f.call()==(pending==UINT16_MAX ? kIOReturnNotAttached : kIOReturnBusy));
        assert(f.pci.writes==before && !f.state.shutdownBlocked);
        assert(bool(f.state.atomicRequester.owner)==ending);
        if(ending) {f.pci.transactionStatus=0;assert(f.call()==0 && !f.output.values[Status]);}
    }
    for(bool failAtBegin:{false,true}) {
        Fixture f;f.pci.refuseEnable=failAtBegin;assert(f.call()==0);
        assert(bool(f.output.values[Status])==failAtBegin);
        assert(f.state.atomicRequester.owner && f.state.atomicRequester.pending);
        f.pci.refuseEnable=false;f.pci.refuseRestore=!failAtBegin;f.enable=0;
        assert(f.call()==0);
        if(!failAtBegin) {
            assert(f.output.values[Status] && f.output.values[Active] && f.output.values[RestorePending]);
            assert(f.state.shutdownBlocked && f.state.sessions.exclusiveClient==&f.client);
            f.pci.refuseRestore=false;assert(f.call()==0 && !f.output.values[Status]);
        }
        assert(!f.state.atomicRequester.owner && !f.state.sessions.exclusiveClient && f.state.shutdownBlocked);
        assert(f.pci.control==0x420); // requires reset before allowing new submissions after a failed write
    }
    puts("Requester experiment: production RPC admission/exclusivity, endpoint identity, one-bit RMW/readback, recovery-before-write and restore failure retention passed");
}
