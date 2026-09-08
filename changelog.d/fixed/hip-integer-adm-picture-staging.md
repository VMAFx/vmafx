- **`integer_adm_hip` faulted the GPU and killed the process on the first
  frame.** `extract_fex_hip` passed `ref_pic->data[0]` straight to the DWT2
  device kernel, but the HIP backend is host-pic (ADR-0530): that pointer is
  HOST memory, so the GPU faulted with "Page not present or supervisor
  privilege" on a host heap address. The CUDA twin needs no staging because it
  gets a device picture from the pool; the HIP port inherited the call shape
  without the thing that made it valid. `integer_psnr_hip` already staged
  correctly, which is why only the ADM test cored. Fixed by allocating a device
  staging buffer per side and copying the luma plane across with
  `hipMemcpy2DAsync` before launch. On a gfx1030 `test_hip_adm_parity` goes from
  a GPU coredump to 3/3 passing, the HIP parity suite from 16/2 to 17/1, and
  `vmaf --backend hip --feature adm_hip` returns real scores instead of dying.
