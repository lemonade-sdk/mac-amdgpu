# MacAMDGPU DriverKit extension

The extension binds to AMD PCI display devices through PCIDriverKit. The
current development target is the Radeon R9700 (RDNA4 / gfx1201) over
Thunderbolt on Apple Silicon. Binding is broader than the implemented
hardware support; RDNA3 bringup is not complete.

Build the host and embedded extension from the root project:

```sh
scripts/build.sh
```

For a compile-only build without signing or installation:

```sh
xcodebuild -project MacAMDGPU.xcodeproj -scheme MacAMDGPUHost \
  -configuration Debug -destination 'platform=macOS' \
  -derivedDataPath build/review CODE_SIGNING_ALLOWED=NO build
```

`MacAMDGPU.cpp` implements driver lifecycle and the user-client selectors.
`amdgpu/` contains the ported firmware, memory-controller, ring, and IP
initialization code. The `.iig` files generate the DriverKit RPC interfaces.

BAR0 is the CPU-visible VRAM aperture, BAR2 contains doorbells, and BAR5
contains registers. Always use `GetBARInfo` memory indexes: an index is not
a BAR number. ReBAR selector 41 accepts a BAR number and returns six scalars:
capability offset, raw capability, raw control, supported-size bitmap,
selected size in bytes, and OS-assigned size in bytes. Bitmap bit n means
`2^(20+n)` bytes. Version 1 capability entries are supported; absent BARs,
unsupported capability versions, malformed data, and failed reads report
errors. This selector does not resize resources.

See [the root README](../README.md) for activation and signing instructions,
and [the progress review](../PROGRESS.md) for verified status and limitations.
The older `macamdgpu_ping.swift` utility has legacy ABI paths; use
`macamdgpu_status.swift` for current status and the host UI for bringup.
