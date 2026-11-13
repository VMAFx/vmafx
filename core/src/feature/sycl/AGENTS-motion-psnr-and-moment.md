<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# Motion, PSNR, and moment twins: `core/src/feature/sycl/` agent invariants

integer_motion*, float_motion_sycl.cpp, integer_psnr_sycl.cpp,
float_psnr_sycl.cpp, integer_psnr_hvs_sycl.cpp, integer_moment_sycl.cpp,
core/test/test_sycl_motion*, core/test/test_sycl_float_motion_parity.c,
core/test/test_sycl_twin_option_parity.c, core/test/test_sycl*psnr*.c,
core/test/test_sycl_float_moment_parity.c; SAD, 5-frame window, PSNR-HVS.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `integer_moment_sycl.cpp`, `/core/test/test_sycl_float_moment_parity.c` | [float-moment](AGENTS.d/float-moment.md) | float_moment_sycl = CPU float_moment, bit for bit past 2^53 units too; integers only; no scratch. |
| `float_motion_sycl.cpp`, `/core/test/test_sycl_float_motion_parity.c`, `/core/test/test_sycl_twin_option_parity.c` | [float-motion](AGENTS.d/float-motion.md) | float_motion_sycl.cpp SAD = CPU order, bit for bit; emits through motion_clip() / motion_blend_clip(), motion3 included. |
| `float_psnr_sycl.cpp`, `/core/test/test_sycl_float_psnr_parity.c` | [float-psnr](AGENTS.d/float-psnr.md) | float_psnr_sycl = CPU float_psnr, bit for bit past 2^53 units too; 64-bit int row-segment sums. |
| `integer_psnr_sycl.cpp`, `/core/test/test_sycl_psnr_parity.c` | [integer-psnr](AGENTS.d/integer-psnr.md) | integer_psnr_sycl.cpp = full CPU psnr option table, bit-exact; ceiling division for chroma plane geometry. |
| `integer_motion_sycl.cpp`, `/core/test/test_sycl_motion_add_uv_parity.c` | [motion-add-uv](AGENTS.d/motion-add-uv.md) | integer_motion_sycl.cpp::motion_add_uv GPU contract; queue-sync invariant, no host wait in submit. |
| `integer_motion_sycl.cpp`, `integer_motion_v2_sycl.cpp`, `/core/test/test_sycl_motion_five_frame_window.c` | [motion-five-frame-window](AGENTS.d/motion-five-frame-window.md) | SYCL motion twins keep n-2 outside graph; CPU window function per frame (advance) and at flush; no new kernel. |
| `integer_motion_pipeline_sycl.*`, `/core/test/test_sycl_motion*` | [motion-sad-pipeline](AGENTS.d/motion-sad-pipeline.md) | Motion SAD = one shared kernel, difference first; sum \|blur(prev - cur)\|. |
| `integer_motion_v2_sycl.cpp`, `/core/test/test_sycl_motion_v2_parity.c` | [motion-v2](AGENTS.d/motion-v2.md) | integer_motion_v2_sycl.cpp reads shared frame; flush derives motion2_v2 / motion3_v2 via CPU window function. |
| `integer_motion_sycl.cpp`, `/core/test/test_sycl_motion*` | [motion](AGENTS.d/motion.md) | motion_sycl output set = CPU motion, and motion_force_zero lives in submit / collect. |
| `integer_psnr_hvs_sycl.cpp`, `/core/test/test_sycl_psnr_hvs_parity.c` | [psnr-hvs](AGENTS.d/psnr-hvs.md) | psnr_hvs_sycl = CPU scores bit for bit; integer_psnr_hvs_sycl.cpp DCT lives in local memory. |
