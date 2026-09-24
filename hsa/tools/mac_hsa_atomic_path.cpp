#include "../src/transport.h"
#include <cstdio>
const char *evidence(mac_hsa::AtomicEvidence value) {
    switch(value) {
    case mac_hsa::AtomicEvidence::Unsupported:return "unsupported";
    case mac_hsa::AtomicEvidence::Advertised:return "advertised";
    default:return "unknown";
    }
}
int main() {
    std::vector<std::shared_ptr<mac_hsa::Connection>> connections;
    const auto status=mac_hsa::discover(connections);
    if(status) {std::fprintf(stderr,"Discovery failed: %#x\n",status);return 1;}
    if(connections.empty()) {std::puts("No bound MacAMDGPU device; original capabilities were not assessed.");return 2;}
    for(const auto &connection:connections) {
        mac_hsa::DeviceSnapshot device;
        if(connection->read(device)) return 1;
        const auto &a=device.originalAtomicCaps;
        std::printf("GPU registry=%#llx original cached AtomicOp hardware=%s configuration=%s\n",
            (unsigned long long)device.registryID,evidence(a.capabilities),evidence(a.configuration));
        std::printf("  missing-capabilities=%#x unknown-capabilities=%#x missing-configuration=%#x unknown-configuration=%#x\n",
            a.missingCapabilities,a.unknownCapabilities,a.missingConfiguration,a.unknownConfiguration);
        for(unsigned i=0;i<a.count;++i) {
            const auto &n=a.functions[i];
            std::printf("  %04x:%04x registry=%#llx PCIe=%s%#x DEVCAP2=%s%#x original-DEVCTL2=%s%#x\n",
                n.vendorID,n.deviceID,(unsigned long long)n.registryID,n.expressKnown ? "" : "unknown/",n.expressCapabilities,
                n.capabilities2Known ? "" : "unknown/",n.capabilities2,n.control2Known ? "" : "unknown/",n.control2);
        }
        std::printf("  Native prerequisite gate: %s; native backend/mapping qualification is separate.\n",
            mac_hsa::hasOriginalAtomicPrerequisites(a) ? "advertised" : "fallback");
    }
    std::puts("Read-only cached registry evidence captured before GPU initialization. No config writes or GPU work.");
    return 0;
}
