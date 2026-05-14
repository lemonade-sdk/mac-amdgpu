# Firmware Blobs

Microcode for AMD GPUs supported by mac_amdgpu. Sourced verbatim
from [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git).

License: see `LICENSE.amdgpu`. Provenance metadata (release date,
upstream commit) for each binary is in `WHENCE.amdgpu`.

## Coverage

The repo currently vendors microcode for the GFX11 (RDNA3) and
GFX12 (RDNA4) families:

| Family   | GC prefix      | PSP prefix    | SMU prefix    | SDMA prefix   |
|----------|----------------|---------------|---------------|---------------|
| RDNA3    | `gc_11_*`      | `psp_13_*`    | `smu_13_*`    | `sdma_6_*`    |
| RDNA4    | `gc_12_*`      | `psp_14_*`    | `smu_14_*`    | `sdma_7_*`    |

## Target hardware mapping

- **R9700** (Radeon AI PRO R9700, PCI 0x1002:0x7551, gfx1201) →
  `gc_12_0_1_*`, `psp_14_0_3_*`, `smu_14_0_3.bin`, `sdma_7_0_1.bin`.
- Other GFX12 cards → `gc_12_0_0_*` or other IP subversions.
- RX 7900-class (gfx1100) → `gc_11_0_0_*`, `psp_13_0_0_*`,
  `smu_13_0_0.bin`, `sdma_6_0_0.bin`.

## How they're loaded

The host app's "Pick Firmware Folder…" button points at this
directory (`firmware/`). The bring-up ladder then calls
LoadFirmware for the SOS + PSP-loaded IP firmware blobs in the
order the dext's PSP code expects.

## Updating

Re-pull linux-firmware and re-copy the same prefixes:

```bash
git clone --depth 1 \
  https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git \
  /tmp/linux-firmware
cp /tmp/linux-firmware/amdgpu/gc_1{1,2}_*.bin firmware/
cp /tmp/linux-firmware/amdgpu/psp_1{3,4}_*.bin firmware/
cp /tmp/linux-firmware/amdgpu/smu_1{3,4}_*.bin firmware/
cp /tmp/linux-firmware/amdgpu/sdma_{6,7}_*.bin firmware/
cp /tmp/linux-firmware/LICENSE.amdgpu firmware/
```
