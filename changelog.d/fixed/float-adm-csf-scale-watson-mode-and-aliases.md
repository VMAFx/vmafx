- **`float_adm` GPU twins applied `adm_csf_scale` / `adm_csf_diag_scale` in
  Watson mode, and named the options differently from the CPU.** In the only
  CSF mode the CUDA/SYCL/HIP/Metal twins support, the CPU reference
  (`adm_tools.c::adm_csf_rfactor_s`) computes `rfactor = 1 / quant_step` and
  never reads those two options — they belong to the Barten branch — but every
  twin multiplied them in, so `adm_csf_scale=2.0` doubled the GPU's CSF weights
  while the CPU ignored it. CUDA, SYCL and HIP also aliased the options `cs` /
  `cds` (max 100) where the CPU uses `scf` / `scfd` (max 50); because feature
  names derive from aliases, the same request produced `adm2_scf_2` on the CPU
  and `adm2_cs_2` on the GPU. Both fixed to match the CPU; each backend's
  `float_adm` parity test gains a variant that sets the options and reads back
  under the derived key.
