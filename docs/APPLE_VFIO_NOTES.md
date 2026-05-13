# APPLE_VFIO_NOTES — Reading notes on `qemu-vfio-apple/contrib/apple-vfio/VFIOUserPCIDriver/`

Honest assessment, not a hagiography. The user described the apple-vfio dext as **"kind of working"** — it boots, claims the R9700, surfaces config + MMIO + DMA + MSI-X to QEMU, and traces flow. It also has known fragile paths (devcoredumps in `traces/`, "rejecting DMA mapping" log lines, telemetry counters for DMA-complete failures). Treat this as a **reference pattern**, not a proven complete one.

Read date: 2026-05-13. Author of apple-vfio: `scottjg` (created 2026-03-18 per file headers).

---

## File inventory

| Path | Lines | Purpose |
|---|---:|---|
| `Info.plist` | 31 | IOKitPersonalities — matches any PCI device (wildcard), declares TB tunnel compatibility |
| `VFIOUserPCIDriver.entitlements` | 15 | `com.apple.developer.driverkit.transport.pci` (wildcard) + `allow-any-userclient-access` |
| `VFIOUserPCIDriver.iig` | ~30 | Driver class — `Start`, `Stop`, `NewUserClient` |
| `VFIOUserPCIDriverUserClient.iig` | 43 | UserClient class — `Start`, `Stop`, `ExternalMethod`, `AsyncCompletion`, `CopyClientMemoryForType`, `InterruptOccurred` |
| `VFIOUserPCIDriver.cpp` | 2213 | All the logic |

## Architecture

```
IOPCIDevice (provider, framework-provided)
   │
   ▼ matches IOPCIPrimaryMatch in Info.plist
VFIOUserPCIDriver (IOService subclass, our driver class)
   │  • Holds providerClaimed flag + opener refcount in ivars
   │  • Caches BAR memory descriptors (g_barDescCache[6])
   │  • NewUserClient(type=0) creates one of these:
   ▼
VFIOUserPCIDriverUserClient (IOUserClient subclass, per-userspace-process)
   │  • DMA buffers, MSI-X vectors, IRQ shared-memory page
   │  • Dispatches userspace-issued selectors via ExternalMethod
   │  • Delivers MSI-X to userspace via shared memory + AsyncCompletion
   ▼
Userspace (QEMU `qemu-vfio-apple`)
```

### Two-class split is mandatory in DriverKit

Driver class (matches the hardware) is separate from UserClient class (per-process IPC endpoint). The Driver class registers via `RegisterService()`; userspace finds it via `IOServiceMatching` and calls `IOServiceOpen` which causes DriverKit to invoke `NewUserClient` on the driver class, returning a new UserClient instance.

## What the dext exposes (`ExternalMethod` selector dispatch table)

```c
enum {
    GetIdentity          = 0,   // bus/dev/fn + VID/DID/classCode
    Claim                = 1,   // Open() the IOPCIDevice (refcounted)
    Terminate            = 2,   // Close() and release
    AllocateDMABuffer    = 3,   // dext-allocated DMA buffer (returns bus addrs)
    FreeDMABuffer        = 4,
    RegisterDMARegion    = 5,   // client-VA-backed DMA region (returns bus addrs)
    UnregisterDMARegion  = 6,
    ProbeDMARegion       = 7,
    ConfigRead           = 8,   // 8/16/32-bit
    ConfigWrite          = 9,
    GetBARInfo           = 10,
    MMIORead             = 11,  // 8/16/32/64-bit
    MMIOWrite            = 12,
    SetupInterrupts      = 13,  // MSI-X preferred, MSI fallback
    CheckInterrupt       = 14,
    WaitInterrupt        = 15,
    SetIRQMask           = 16,
    ResetDevice          = 17,  // FLR → hot-reset fallback
};
```

Memory types (for `CopyClientMemoryForType`):

```c
DMABuffer  = 0   // mapped via IOConnectMapMemory64
BAR0..BAR5 = 1..6
IRQState   = 7   // 16K shared page with irqPending[4]+irqEnabled[4] uint64s
```

## Concrete patterns to mirror in `mac_amdgpu/dext/`

### 1. Bundle layout
Three Xcode targets, parent/child bundle IDs (per qemu-vfio-apple README):
- Host app — SwiftUI shell, lives in `/Applications` (mandatory for staging)
- Dext — child of host app
- CLI — embedded in host app

For us: same shape, but the "host app" can be very minimal (or we could ship the dext via a different host).

### 2. Info.plist personality
```xml
<key>IOClass</key>            <string>IOUserService</string>
<key>IOProviderClass</key>    <string>IOPCIDevice</string>
<key>IOPCITunnelCompatible</key> <true/>
<key>IOUserClass</key>        <string>VFIOUserPCIDriver</string>
<key>IOUserServerName</key>   <string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
<key>IOPCIPrimaryMatch</key>  <string>0xFFFFFFFF&amp;0x00000000</string>
```

- `IOPCITunnelCompatible: true` is **mandatory for TB5/USB4 eGPUs**. Without it the dext won't bind through a Thunderbolt tunnel.
- `IOPCIPrimaryMatch` syntax is `value&mask`. apple-vfio uses `0xFFFFFFFF&0x00000000` — match anything (mask 0). **For amdgpu.dext we should use `0x75511002&0xFFFFFFFF`** to match exactly R9700 VID:DID, with mask 0xFFFFFFFF.

### 3. Entitlement
```xml
<key>com.apple.developer.driverkit.transport.pci</key>
<array>
  <dict>
    <key>IOPCIPrimaryMatch</key>
    <string>0xFFFFFFFF&amp;0x00000000</string>  <!-- match scope -->
  </dict>
</array>
<key>com.apple.developer.driverkit.allow-any-userclient-access</key>
<true/>
```

Apple grants this entitlement against a specific bundle ID + match scope. apple-vfio got wildcard scope (any device). **We'd request narrower scope (`0x75511002&0xFFFFFFFF`)** — easier review, also forces a clean architectural line.

### 4. PCI claim with refcounting (`vfio_user_retain_shared_claim`, line ~350)
- Primary client calls `IOPCIDevice::Open(this, 0)`.
- Subsequent clients increment `providerClaimRefs` without re-opening.
- **Critical:** the entity that called `Open()` must be the one that calls `Close()` — IOPCIFamily enforces this. apple-vfio tracks `openerClient` and routes Close through it.
- After Open, sets PCI command register bits 0x02 (Memory Space) + 0x04 (Bus Master) → `wanted = cmd | 0x06`.

For us: we likely don't need multi-client refcounting (one Vulkan ICD per process is plenty). Single-claim is simpler.

### 5. BAR mapping (`CopyClientMemoryForType`, line ~2106)
- Userspace calls `IOConnectMapMemory64(connection, kVFIOUserPCIDriverUserClientMemoryTypeBAR0..5, ...)`
- Dext calls `pciDevice->_CopyDeviceMemoryWithIndex(memoryIndex, &barMemory, opener)` where `opener` is the **primary client's `this` pointer** (else the call fails with `kIOReturnNotPermitted`).
- Result cached in `g_barDescCache[6]` so secondary clients can share.
- **AS quirk handled at line ~1100:** config-space reads of BAR registers (offsets `0x10..0x24`) return DART-rewritten addresses with bogus PCI type bits. apple-vfio synthesizes the correct value from `GetBARInfo` instead. **We need the same trick if any code reads BAR registers via config space.**

### 6. DMA — two paths
**Path A: dext-allocated buffer (`vfio_user_allocate_dma_buffer`, line ~515):**
```c
IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn, size, alignment, &buf);
IODMACommand::Create(pciDevice, ..., spec.maxAddressBits=64, &cmd);
cmd->PrepareForDMA(..., buf, 0, size, &dmaFlags, &segmentsCount, segments);
```
Up to 32 segments returned; bus addresses live in `segments[i].address`.

**Path B: client-VA-backed region (`vfio_user_register_client_dma_region`, line ~616):**
- Userspace passes (clientVA, size, iova) into selector 5.
- Dext calls `CreateMemoryDescriptorFromClient(direction, count, &clientSegment, &memDesc)` — wraps the userspace pages into a descriptor.
- Then `IODMACommand::PrepareForDMA` to get bus addresses.
- **Chunks at 1.5 GB granularity** (`VFIO_USER_DMA_CHUNK_SIZE = 1536 MB`) because DriverKit/DART can't handle a single arbitrarily large prepare.
- **Rejects multi-segment mappings** (line ~795–841): only accepts `dmaSegmentsCount == 1 && length == chunkSize`. **This is a known fragile point.** If DART scatters the mapping, the dext refuses, and the guest probably gets a `kIOReturnNotAligned`. amdgpu wanting a large GART aperture could hit this.

For us: native amdgpu driver allocates its own GART page table in our own DMA buffer (Path A). We don't need Path B — we are the consumer. Drop the client-VA path entirely.

### 7. Config space (`vfio_user_config_read/write`, line ~1080)
Direct passthrough to `IOPCIDevice::ConfigurationRead{8,16,32}` and `Write{8,16,32}` — except the BAR register synthesis quirk above.

### 8. MMIO (`vfio_user_mmio_read/write`, line ~1170)
Direct passthrough to `IOPCIDevice::MemoryRead{8,16,32,64}` and `MemoryWrite{8,16,32,64}` — keyed by `memoryIndex` (which is `GetBARInfo`'s `memoryIndex` output, **not** the BAR index). The mapping from BAR index → memoryIndex is per-device.

### 9. MSI-X setup (`vfio_user_setup_interrupts`, line ~1555)
- `FindPCICapability(kIOPCICapabilityIDMSIX)` → `msgCtrl & 0x7FF + 1` = vector count.
- `pciDevice->ConfigureInterrupts(kIOInterruptTypePCIMessagedX, 1, requested, 0)`.
- For each vector: `IOInterruptDispatchSource::Create(pciDevice, i, irqQueue, &source)` + `CreateActionInterruptOccurred(sizeof(uint32_t), &action)` + `SetHandler` + `SetEnable(true)`.
- The per-action reference word stores the vector index, so `InterruptOccurred` knows which vector fired.

### 10. IRQ delivery via shared memory (line ~1591)
- Dext allocates a 16 KB `IOBufferMemoryDescriptor`. First 64 bytes used.
- `irqPending[0..3]` (uint64) — set by dext on interrupt, cleared by client. 4×64 = 256 bits = up to 256 vectors.
- `irqEnabled[0..3]` (uint64) — client writes, dext reads to gate delivery.
- Userspace `IOConnectMapMemory64` with `kVFIOUserPCIDriverUserClientMemoryTypeIRQState` maps this page.
- All ops atomic via `__atomic_*`. **No Mach IPC on the fast path** — userspace polls or sleeps on `WaitInterrupt`.
- `WaitInterrupt` (selector 15) registers an `OSAction`; `InterruptOccurred` swaps it out and calls `AsyncCompletion` to wake userspace.

This is a **good pattern** for our IH-ring drain → userspace wakeup. We'll likely keep it.

### 11. Reset (`vfio_user_reset_device`, line ~1518)
```c
pciDevice->Reset(kIOPCIDeviceResetTypeFunctionReset, 0);   // FLR
// on failure:
pciDevice->Reset(kIOPCIDeviceResetTypeHotReset, 0);        // secondary bus reset
```
PCIDriverKit handles config save/restore and link training internally. **Traces confirm `vfio_pci_reset_flr` fires successfully on the R9700** (see `traces/linux-pre-driver-init.txt`).

---

## What apple-vfio does NOT do (gaps for our amdgpu.dext)

The apple-vfio dext is **VFIO passthrough only**. The guest Linux kernel does all the AMD-specific work. For a native macOS amdgpu driver we add **everything below the dashed line**:

```
─── apple-vfio scope ────────────────────────────────────────────
  PCI claim, BAR map, DMA region, MSI-X plumbing, config R/W,
  MMIO R/W, FLR, IRQ shared-mem page
─── amdgpu-specific (we own this) ──────────────────────────────
  IP discovery table parse           (amdgpu_discovery.c)
  PSP v14 bootloader, SOS load       (psp_v14_0.c)
  SMU v14_0_3 mailbox + clocks       (smu_v14_0.c)
  GMC v12: VRAM detect, GART, VM PTE (gmc_v12_0.c)
  Firmware load (gc_12_0_1_*, etc.)  (amdgpu_ucode.c)
  IMU, RLC, CP bringup
  MES v12_1 queue manager            (mes_v12_0.c)
  NBIO v7_11 interrupt routing       (nbio_v7_11.c)
  IH v7 ring drain                   (ih_v7_0.c)
  GFX12 queue creation, doorbell map (gfx_v12_1.c)
  SDMA v7_1 ring                     (sdma_v7_0.c)
  Fence emission via RELEASE_MEM
  amdgpu UAPI (alloc_bo, submit_cs, wait_fence, info)
```

## Known fragile points (the "kind of working" parts)

1. **Multi-segment DMA rejection** (line ~795). DART can split a large client-VA region into multiple physical bus segments; the dext refuses anything that isn't a single contiguous bus segment. For amdgpu we sidestep by allocating in the dext (Path A) and giving the GPU a page-table-backed view via GART — DART contiguity in system memory doesn't matter once GART scatters them.
2. **`CompleteDMA` failures tracked but not handled** — `dmaCompleteFailureCount` counter only. Repeated failures eventually exhaust resources.
3. **Wildcard PCI match in both Info.plist and entitlement.** Works for them (Apple-approved) but is broad. We narrow it for amdgpu.
4. **No power-state handling** — beyond setting bus master at Open. No D0/D3 transitions, no PME. amdgpu needs this for runtime PM and idle clocks.
5. **`g_barDescCache` is a global static** — assumes one device per process. If a future user has multiple R9700s we'd need per-instance state.
6. **Devcoredumps in `traces/`** suggest GPU resets / hangs occurred during their bringup work. Read those before assuming the dext is stable for the full amdgpu lifecycle.

---

## Adapt-don't-fork plan

We are not reusing the apple-vfio dext binary or source. We are writing `mac_amdgpu/dext/` from scratch, **using apple-vfio as the architectural reference** for the PCIDriverKit layer.

Things we copy in concept (and reimplement):
- The `IOPCITunnelCompatible: true` personality
- The two-class Driver+UserClient split with `RegisterService`/`NewUserClient`
- The Open/Close refcount pattern (simplified to single-client)
- The BAR memory descriptor cache + `_CopyDeviceMemoryWithIndex(opener=primary)` trick
- The DMA buffer + segment list pattern (Path A only)
- The shared 16 KB IRQ state page with atomic pending/enabled bitmaps
- The `IOInterruptDispatchSource` + per-action vector-ref pattern
- The FLR-then-hot-reset reset strategy
- The BAR-register-synthesis trick in config space reads

Things we add on top (the actual amdgpu work):
- Everything below the dashed line in the diagram above
- Our own UAPI (different selectors), modeled on Linux's amdgpu UAPI subset rather than VFIO semantics

Things we drop:
- Multi-client refcounting (one Vulkan ICD per process is enough)
- The client-VA-backed DMA path (we manage all GPU memory in-dext)
- Wildcard PCI match (we target R9700 specifically)
- Most of the VFIO selector surface (Get/SetIRQMask, ProbeDMARegion, etc. — VFIO-specific, irrelevant for amdgpu)
