; group: 64,1,1
; rsrc2: 0x8088
; fssm-at: 0x150
; fssm-data: 0x4c505055 0x44414552
; M8: a compute shader reads an UPLOAD-heap buffer through a raw SRV V# in a descriptor table and writes a raw UAV.
; Run: d3d12_upload_arena_test. D3D12 build: LEONIDA_USER_DATA_SHIFT=4 (root arguments from s4, as cs_uav3d_store).
; User data: [0] two 32-bit constants (s0 = tag), [1] a descriptor table at s2 (s3 zero); group index s4.
;   slot 0 (+0x00): V#, raw SRV of the upload buffer (read)    slot 1 (+0x20): V#, raw UAV (written)
; g = 64 * group + lane:   out[g] = 3 * in[g] + tag
; Before M8 an upload buffer's VA was its device address minus heap_base, negative here, and a V# keeps 48 bits of it:
; the load went to a wild address (docs/d3d12_vulkan.md, "Measured (D3D-53)").
s_load_dwordx4 s[8:11], s[2:3], 0x0
s_load_dwordx4 s[12:15], s[2:3], 0x20
s_lshl_b32 s5, s4, 6
s_waitcnt lgkmcnt(0)
v_add_nc_u32_e32 v1, s5, v0
v_lshlrev_b32_e32 v2, 2, v1
buffer_load_dword v3, v2, s[8:11], 0 offen
s_waitcnt vmcnt(0)
v_mul_lo_u32 v3, v3, 3
v_add_nc_u32_e32 v3, s0, v3
buffer_store_dword v3, v2, s[12:15], 0 offen
s_endpgm
