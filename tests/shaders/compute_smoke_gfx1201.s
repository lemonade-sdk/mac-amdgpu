// One wave32 workgroup. s[0:1] = buffer GPU VA, s2 = addend,
// v0 = local invocation X. No scratch, LDS, barriers or kernel descriptor.
// The direct PM4 launcher supplies three user SGPRs and enables only TID X.
// Input: 32 dwords at byte 0. Output: 32 dwords at byte 256.
.text
.p2align 8
.globl compute_smoke
compute_smoke:
    v_lshlrev_b32 v1, 2, v0
    global_load_b32 v2, v1, s[0:1]
    s_wait_loadcnt 0
    v_add_u32 v2, s2, v2
    global_store_b32 v1, v2, s[0:1] offset:256
    s_wait_storecnt 0
    s_endpgm
