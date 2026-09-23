// LLVM 21.1.8 output for dispatch_gfx1201.cl, checked byte-for-byte by the test.
// GFX12 workgroup X is ttmp9. Kernarg pointer is s[0:1].
// Keep compiler-inserted s_delay_alu/s_wait_alu dependency handling.
.text
.p2align 8
.globl dispatch_vector
dispatch_vector:
	s_load_b96 s[0:2], s[0:1], 0x0
	v_lshl_or_b32 v0, ttmp9, 5, v0
	v_mov_b32_e32 v1, 0
	s_delay_alu instid0(VALU_DEP_1) | instskip(SKIP_1) | instid1(VALU_DEP_1)
	v_lshlrev_b64_e32 v[2:3], 2, v[0:1]
	v_add_nc_u32_e32 v0, 0x100, v0
	v_lshlrev_b64_e32 v[0:1], 2, v[0:1]
	s_wait_kmcnt 0x0
	s_delay_alu instid0(VALU_DEP_3) | instskip(NEXT) | instid1(VALU_DEP_1)
	v_add_co_u32 v2, vcc_lo, s0, v2
	v_add_co_ci_u32_e64 v3, null, s1, v3, vcc_lo
	s_delay_alu instid0(VALU_DEP_3)
	v_add_co_u32 v0, vcc_lo, s0, v0
	s_wait_alu 0xfffd
	v_add_co_ci_u32_e64 v1, null, s1, v1, vcc_lo
	global_load_b32 v2, v[2:3], off
	s_wait_loadcnt 0x0
	v_add_nc_u32_e32 v2, s2, v2
	global_store_b32 v[0:1], v2, off
	s_endpgm
