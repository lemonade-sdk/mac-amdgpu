# Lemon Seed Engine on mac_amdgpu

[Lemon Seed Engine (LSE)](https://github.com/Geramy/LSE) is the inference engine. [mac_amdgpu](https://github.com/lemonade-sdk/mac-amdgpu) supplies the macOS DriverKit driver, HSA runtime, and tracked macOS adapters for LSE and [HRX/Loom](https://github.com/ROCm/hrx-system). This guide reproduces the current build and small GPU workloads; it does not claim working full-model inference.

The tested path is native Apple Silicon → LSE → Loom → HRX → this HSA runtime → the external `gfx1201` GPU. It does not use Metal, an x86 emulation layer, or an installed Linux ROCr runtime. The LSE adapter explicitly selects HRX/Loom for its GPU checks and rejects CPU fallback.

## Requirements

Hardware validation covers the Radeon AI PRO R9700 (`gfx1201`, `1002:7551`) over Thunderbolt on Apple Silicon. Follow the repository's [hardware requirements](../README.md#hardware-requirements), [Apple Developer setup](../README.md#apple-developer-portal-setup), and [driver build/activation instructions](../dext/README.md). Development currently requires the documented SIP configuration, appropriate DriverKit entitlements, signing identity and provisioning profiles. A successful HSA/LSE build does not install or approve the driver.

For compilation, use the selected Xcode command-line tools/SDK, Git, Python 3, CMake, Ninja, Rust/Cargo, and an LLVM toolchain with AMDGPU support and matching libc++ libraries. The validated toolchain is LLVM **21.1.8**; the current development installation uses ELF LLD **23.1.1**, CMake **4.3.1**, and Ninja **1.13.2**. The tokenizer build was validated with Rust/Cargo **1.89.0**; omitting Cargo disables tokenizer-dependent CLI/server targets. The driver project also requires `xcodegen`. Apple's default linker is not an ELF `ld.lld` replacement. C++26 compilation for LSE uses the LLVM libc++ headers and library; the adapter removes the upstream reflection requirement.

The scripts select LLVM using [amdgpu-llvm-env.sh](../scripts/amdgpu-llvm-env.sh). To choose another installation of the validated compiler/linker, set their actual paths before running the commands below:

```sh
export AMDGPU_LLVM_BIN=/path/to/llvm-21.1.8/bin
export AMDGPU_LLD=/path/to/ld.lld
```

On the development machine the compiler is `/opt/homebrew/Cellar/llvm/21.1.8/bin`, and the linker is `/opt/homebrew/bin/ld.lld`. The helper also finds matching retained Z3 libraries for older Homebrew LLVM kegs. Keep the compiler's required runtime libraries installed; do not rename or symlink an incompatible Z3 ABI to satisfy a missing dylib. Compiler changes require revalidating the generated-code checks.

## Get the pinned sources

`upstream/` and `build/` are intentionally ignored. They are not populated by cloning this repository, and they are not submodules. From a new checkout:

```sh
git clone https://github.com/lemonade-sdk/mac-amdgpu.git
cd mac-amdgpu
mkdir -p upstream

git clone https://github.com/ROCm/hrx-system.git upstream/hrx-lse-pin
git -C upstream/hrx-lse-pin checkout --detach 5927b0e0fafdefb5c8b41aa71bca8fd28791ad7c

git clone https://github.com/Geramy/LSE.git upstream/lse
git -C upstream/lse checkout --detach b5637a7109d409c21f75586edb75e7631277bce8

git clone https://github.com/iree-org/hsa-runtime-headers.git upstream/hsa-runtime-headers
git -C upstream/hsa-runtime-headers checkout --detach 4285513114a70f7cf4830c89279c8cfa57b901bb
```

These exact revisions match the build scripts and tracked patches. The HRX and LSE scripts reject another revision. They create disposable source copies in `build/hrx-macos-source` and `build/lse-macos-source`, then apply [the HRX adapter](../patches/hrx/macos-coarse-host-adapter.patch) and [the LSE adapter](../patches/lse/macos-host-adapter.patch). Do not apply these patches to the original `upstream/` checkouts. Re-running a script accepts its already-applied exact patch; a stale or independently edited build copy must be reconciled before proceeding.

First-time HRX configuration may download dependencies such as flatcc using the pinned HRX `MODULE.cmake.lock` URLs and hashes. LSE also fetches fastokens revision `7973014e4f3a6028ac48f305704eacd64d0b4ef6` and Cargo dependencies for the tokenizer. Source clones and first-time dependency downloads need network access. The source and test audit here was performed locally; a brand-new network bootstrap has not been rerun as a separate qualification.

## Build and check without using the GPU

Run from the repository root. First prepare the embedded GPU signal service and the fixtures required by the host-only HSA tests:

```sh
bash scripts/build-signal-operations.sh
bash scripts/build-signal-mailbox-service.sh
bash scripts/test-hsa-code-objects.sh
bash scripts/build-hrx-compute-fixture.sh

cmake -S hsa -B build/hsa -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/hsa --parallel 4
ctest --test-dir build/hsa --output-on-failure
```

These steps compile GPU machine code but do not submit it. The HSA tests use mock GPU connections. Host virtual-memory tests need normal macOS Mach/shared-memory access and may fail inside a restrictive automation sandbox. The checked-in generated signal headers are sufficient for normal compilation; regeneration above confirms the selected compiler can reproduce the fixtures.

Next build actual HRX/Loom and LSE, followed by the compile-only checks and GPU smoke executables:

```sh
bash scripts/build-hrx-macos.sh
bash scripts/test-hrx-abi.sh
bash scripts/build-lse-macos.sh
bash scripts/test-lse-loom-compile.sh
bash scripts/build-lse-q6-smoke.sh
bash scripts/build-lse-conv-smoke.sh
```

The HRX script builds Loom by default; do not set `HRX_BUILD_LOOM=OFF` for this path. `HRX_BUILD_JOBS` and `LSE_BUILD_JOBS` control build parallelism. The HRX library check and LSE `--help`/CPU tests do not initialize the GPU. The compile-only Loom check includes small FP32/quantized operations and real model-shaped kernel compilation; successful compilation alone is not an inference result.

The resulting artifacts include:

| Artifact | Location |
| --- | --- |
| HSA runtime | `build/hsa/libhsa-runtime64.dylib` |
| HRX runtime | `build/hrx-macos-adapter/libhrx/src/libhrx/libhrx.dylib` |
| Loom compiler | `build/hrx-macos-adapter/loom/binding/c/libloomc.dylib` |
| LSE CLI | `build/lse-macos-adapter/lse` |
| LSE HTTP server | `build/lse-macos-adapter/lse-server` |
| LSE Q6 test | `build/lse-macos-adapter/mac-lse-q6-smoke` |
| LSE convolution test | `build/lse-macos-adapter/mac-lse-conv-smoke` |

The Linux release binary from the upstream LSE project is not the macOS adapter built here.

## Run the small GPU checks

Install and approve the freshly built driver through MacAMDGPUHost, turn on the enclosure, and initialize the GPU. Run these checks one at a time and stop if a step fails. They submit real GPU work:

```sh
DYLD_LIBRARY_PATH="$PWD/build/hsa" \
  build/hrx-macos-adapter/mac-hrx-smoke --run

DYLD_LIBRARY_PATH="$PWD/build/hsa" \
  build/lse-macos-adapter/mac-lse-q6-smoke --run

DYLD_LIBRARY_PATH="$PWD/build/hsa" \
  build/lse-macos-adapter/mac-lse-conv-smoke --run
```

The first check covers actual HRX streams, copy/fill, FP32 affine compute and matrix multiplication with exact readback and guards. The second builds and runs an actual LSE/Loom/HRX affine Q6 projection, checks 51 exact outputs and all four input buffers/guards, requires GPU dispatch without CPU fallback, and checks runtime shutdown. Neither test needs model downloads or LM Studio.

The convolution regression has passed six GPU cases, checking 680 exact outputs, causal padding, retained history, input preservation, guards, and clean shutdown.

## Current inference boundary

The Q6 projection and the small HRX workloads above have passed on hardware. CPU portability tests and offline Loom compilation also pass. The GPU-owned HSA signal mailbox is the default on the qualified shared-memory/driver profile, with verified idle retirement and preservation of all seven application queue slots; see [the signal service qualification](ATOMIC_MAILBOX_SERVICE.md).

Full model loading and token generation are still being qualified. The Qwen3.8-27B six-bit checkpoint loaded 1,847 text tensors into 11 VRAM slabs (21.731 GiB). The initial `causal_conv1d` lowering gap is fixed and the focused convolution test passes. The latest full-model retry progressed to a further strict rejection: `no Loom template for repeat`; that operator is under implementation. No full-model GPU token, model accuracy, chat-server generation, or token throughput is qualified yet. Small Q6 success does not establish arbitrary quantization support.

## Local model and HTTP server

Use [LOCAL_RUN.md](../LOCAL_RUN.md) for the short Terminal commands, installed-driver launch, monitor, local checkpoint preflight, bounded one-token attempt, and HTTP chat request. The server now accepts `--pool hrx:0 --dialect loom`; its option parsing and native build have been checked without opening a GPU or HTTP socket. Server generation remains experimental until full-model execution and shutdown are qualified.

`LSE_REQUIRE_DEVICE_KERNELS=1` rejects host execution of device groups. Keep it set for qualification so unsupported Loom operations cannot silently become CPU inference. The ordinary CLI's `--stats` reports prompt count/prefill time, decode tokens/s, and device/host/fallback group counts. Non-streaming HTTP responses include `timings.prompt_per_second` (prompt processing tokens divided by prefill duration) and `timings.predicted_per_second` (generated tokens divided by decode duration), as well as token counts and milliseconds. These are workload timings, not GPU utilization or an established performance result.

Use [the HRX validation notes](HRX_MACOS_VALIDATION.md), [HSA behavior status](../hsa/API_STATUS.md), and [the repository's current status](../README.md#working) for the supported boundaries. General native mixed CPU/GPU RMW, general fine-grained shared memory, multi-GPU LSE spanning, and hardware profiling remain separate unsupported or unqualified paths.
