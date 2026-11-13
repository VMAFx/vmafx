<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# Queues, device frames, and kernels: `core/src/feature/sycl/` agent invariants

integer*.cpp, float*.cpp, speed*, ssimulacra2_sycl.cpp, sycl_compat.h,
sycl_tile_index.h, core/src/sycl/dmabuf_import.cpp,
core/src/feature/cuda/integer*.c, core/test/vmafx_sycl_cells.h,
core/test/test_sycl_init_unwind.cpp, core/test/test_sycl_zero_copy*.c; device
pictures, VAAPI/dmabuf zero-copy, queue sync, init unwind, scratch/local
memory, sub-groups, tile borders, option sync.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `float_adm_sycl.cpp`, `float_motion_sycl.cpp`, `float_psnr_sycl.cpp`, `float_vif_sycl.cpp`, `integer_ciede_sycl.cpp`, `integer_motion_sycl.cpp`, `integer_ms_ssim_sycl.cpp`, `integer_ssim_sycl.cpp`, `speed_chroma_sycl.cpp`, `speed_sycl_pipeline.cpp`, `speed_sycl_pipeline.h`, `speed_temporal_sycl.cpp`, `ssimulacra2_sycl.cpp`, `/core/test/vmafx_sycl_cells.h` | [device-pictures](AGENTS.d/device-pictures.md) | Twins copy SYCL device pictures on the device via vmaf_sycl_picture_read_plane, never the host (ADR-2091). |
| `/core/src/sycl/dmabuf_import.cpp`, `integer_motion_sycl.cpp` | [dmabuf-vaapi](AGENTS.d/dmabuf-vaapi.md) | VAAPI / dmabuf zero-copy import — FFmpeg libvmaf_sycl pipeline. |
| `integer_motion_sycl.cpp`, `/core/test/test_sycl_init_unwind.cpp` | [init-unwind](AGENTS.d/init-unwind.md) | A failed extractor init owns its cleanup; calls NULL-safe local close callback. |
| `integer_*.cpp`, `float_*.cpp` | [kernel-identities](AGENTS.d/kernel-identities.md) | Kernel identities and output captures have explicit boundary. |
| `sycl_compat.h`, `integer_ssim_sycl.cpp` | [local-accessor](AGENTS.d/local-accessor.md) | Stencil/convolution SYCL kernels MUST use local_accessor for tap access. |
| `integer_*.cpp`, `/core/src/feature/cuda/integer_*.c` | [option-table-sync](AGENTS.d/option-table-sync.md) | Adding feature knob to any one backend must sync option tables across backends. |
| `*.cpp` | [queue-sync](AGENTS.d/queue-sync.md) | Per-step q.wait() in feature extractors forbidden — use in-order queues, barriers, or graph wait. |
| `sycl_compat.h`, `integer_adm_sycl.cpp`, `integer_vif_sycl.cpp`, `integer_ssim_sycl.cpp`, `integer_motion_pipeline_sycl.cpp`, `ssimulacra2_sycl.cpp` | [scratch-memory](AGENTS.d/scratch-memory.md) | No scratch memory in kernels; zero private_mem_size and spill_memory_size on xe. |
| `sycl_compat.h`, `float_motion_sycl.cpp`, `float_adm_sycl.cpp` | [sub-group-size](AGENTS.d/sub-group-size.md) | Kernel sub-group size: 16 or 32 only (ADR-1468); Xe2 AOT targets reject 8. |
| `sycl_tile_index.h`, `integer_vif_sycl.cpp` | [tile-index](AGENTS.d/tile-index.md) | Tile loaders clamp after reflecting (sycl_tile_index.h) around borders. |
| `integer_adm_sycl.cpp`, `integer_vif_sycl.cpp`, `integer_motion_sycl.cpp`, `integer_motion_v2_sycl.cpp`, `integer_cambi_sycl.cpp`, `integer_moment_sycl.cpp`, `integer_psnr_sycl.cpp`, `integer_psnr_hvs_sycl.cpp`, `/core/test/test_sycl_zero_copy_admission.c`, `/core/test/test_sycl_zero_copy_model_gate.c` | [zero-copy-admission](AGENTS.d/zero-copy-admission.md) | reads_shared_luma_only() true only if submit() reads no picture for these options (ADR-1688). |
