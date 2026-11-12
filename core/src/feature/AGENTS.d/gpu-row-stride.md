---
paths:
  - core/src/feature/cuda/*
  - core/src/feature/hip/*
  - core/src/feature/sycl/*
  - core/src/feature/metal/*
invariant: GPU code never advances a pointer to samples wider than a byte by a byte stride.
area: simd-and-gpu
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Row stride above 8 bits

`VmafPicture::stride`, device pitches and kernel stride arguments count
bytes. Address row through byte pointer, then cast row:
`reinterpret_cast<const T *>(plane + y * stride)[x]`. Never
`reinterpret_cast<const T *>(plane) + y * stride` (upstream CUDA 16-bit
motion defect: row `2 * y`, reads past plane). Stride counting elements
converts in expression (`stride / sizeof(T)`, `>> 1`) or names unit
(`_elems`, `_px`, `_samples`). `core/test/test_gpu_byte_stride_contract.py`
scans every source under `cuda`, `hip`, `sycl`, `metal` dirs of `core/src`,
fails on cast-then-stride and on wide pointer variable indexed by
`->stride[` / `.stride[`. Mixed-unit variable (bytes at scale 0, elements
above) -> local `*_elems` copy in element branch (`integer_adm_sycl.cpp`
`adm_dev_dwt_src()`, `integer_vif_sycl.cpp` `dev_read_pixel()`).
