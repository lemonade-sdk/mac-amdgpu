# MES v12_1 port-order checklist

The MicroEngine Scheduler — RDNA4's queue manager. Upstream:
`drivers/gpu/drm/amd/amdgpu/mes_v12_0.c` (gfx1201 uses these).

End state: MES is alive, the kernel-mode SCHED ring is up, the
scheduler has been told what HQDs / VMIDs / doorbell index range
are available. Adding a user queue is a single `ADD_QUEUE` API
message after that.

## Dependencies

- PSP SOS + ring + TMR (done) + ring command submit (done).
- SMU alive enough to gate GFX/compute clocks.
- GMC + GART aperture configured.
- IMU + RLC + CP firmwares loaded (the autoload chain from PSP).
- IH ring up so MES completion events route somewhere.

## Steps

| # | Source                                | Touches          | Mem                | Cplx |
|---|---------------------------------------|------------------|--------------------|------|
| 1 | `amdgpu_mes_init_microcode` (667)    | none             | sysmem (FW blobs)  | S |
| 2 | `mes_v12_1_sw_init` (1531)           | none             | VRAM EOP 2K, MQD 4K; GTT ring  | M |
| 3a | `mes_v12_1_load_microcode` (1063) *  | CP_MES_IC/MD     | VRAM ucode+data    | M |
| 3b | `set_ucode_start_addr` (1037)        | CP_MES_PRGRM_CNTR | none              | S |
| 3c | `mes_v12_1_enable` (985)             | CP_MES_CNTL      | none               | S |
| 3d | `enable_unmapped_doorbell_handling` (779) | CP_UNMAPPED_DOORBELL | none       | S |
| 3e | `mes_v12_1_queue_init` (1374)        | CP_HQD_* (many)  | VRAM MQD           | M |
| 3f | `mes_v12_1_set_hw_resources` (647)   | via API msg      | none               | M |
| 3g | `init_aggregated_doorbell` (723)     | CP_MES_DOORBELL_CTL 1..5 | none      | S |
| 3h | query_sched_status                    | via API msg      | none               | S |
| 4 | (if not uni_mes) `kiq_hw_init` (1681) | mirrors step 3   | mirrors step 3     | M |
| 5 | `mes_v12_1_add_hw_queue` (283)       | via API msg      | none               | M |

\* Step 3a is **only** needed for AMDGPU_FW_LOAD_DIRECT. If MES
firmware was loaded via PSP `LOAD_IP_FW` already, skip 3a–3c.

## Firmware

Linux-firmware ships three relevant blobs for gfx1201:

```
gc_12_0_1_mes.bin       — scheduler pipe (dual-pipe mode)
gc_12_0_1_mes1.bin      — KIQ pipe (dual-pipe mode)
gc_12_0_1_uni_mes.bin   — unified, replaces both
```

If `enable_uni_mes = true`, use the uni firmware and skip step 4
(KIQ init). For RDNA4 the uni path is recommended.

## Hot points

- **PSP-driven firmware load is the simpler path on our dext** —
  use `psp_load_ip_fw` with `GFX_FW_TYPE_RS64_MES` (76) +
  `GFX_FW_TYPE_RS64_MES_STACK` (77). If uni: same but unified.
  This makes step 3a unnecessary.
- **Step 3f is the *real* gate.** The `SET_HW_RESOURCES` API message
  tells MES what VMIDs / HQD masks it owns. Wrong values here =
  every subsequent `ADD_QUEUE` rejects. Mirror upstream's struct
  field-for-field — see `mes_v12_api_def.h` for the wire format.
- **Doorbell index allocation:** Linux uses a per-device IDA; we
  need our own. A simple bitmap covering the doorbell page range
  is sufficient for first-PM4.
- **SRBM mutex protection (lines 997, 1085, 1045)**: GRBM_GFX_INDEX
  selects which pipe a register block writes touch. If two
  contexts race a select they corrupt each other. Our dext is
  single-client per IOServiceOpen, so a single in-driver lock is
  enough; document it explicitly.
- **VRAM allocations** (EOP, MQD, ucode cache, shared cmd buf) are
  the main consumers from MES. ~30–50 KB per XCC. Currently no
  VRAM allocator — substitute GTT-backed allocs (DART-mapped
  system memory) for everything except the ucode cache; those
  *might* need VRAM-resident at fixed address for the cache prime
  sequence (TBD when we get there).

## API message marshalling

`MESAPI__ADD_QUEUE` and `MESAPI_SET_HW_RESOURCES` are packed structs
that get written into the SCHED ring directly + ring's doorbell
kicked. The wait-for-completion is on a fence_value the driver
writes into the message and MES echoes back. Same general pattern
as PSP gfx commands. We'll have a generic
`mes_submit_pkt_and_poll_completion()` helper.
