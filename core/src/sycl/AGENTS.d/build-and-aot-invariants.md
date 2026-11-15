---
paths:
  - core/src/meson.build
  - core/src/sycl/check_aot_image.py
  - core/src/sycl/coff_add_anchor.py
  - core/src/sycl/common.cpp
  - core/src/sycl/queue_props.h
invariant: -fsycl link args, compile-time AOT + zebin/fatbin check, Windows MSVC device-link anchor, and immediate queue.
---
# SYCL link args, AOT zebin check, and MSVC device link

## Rebase-sensitive build invariants

- **`-fsycl` must be present at every link step pulling SYCL `.o`
  files from static archive.** Enforced by embedding `-fsycl` in
  `sycl_dependency.link_args` in `core/src/meson.build` (ADR-1099).
  Without `-fsycl` at link time: `icpx` skips `clang-offload-wrapper`,
  SYCL runtime's `ProgramManager` never registers device kernels,
  first `queue.submit()` crashes with null-dereference inside
  `getDeviceKernelInfo`. **On rebase**: branch splitting
  `sycl_dependency` into compile-time and link-time halves, or
  introducing new `declare_dependency` block for SYCL sub-target ->
  ensure `-fsycl` stays in link-time `link_args` of every dependency
  SYCL consumers inherit.

- **AOT images generated at compile time, checked after link
  (ADR-1360).** AOT path of `sycl_toolchain_args` (`sycl_icpx_aot_base_args`
  in `core/src/meson.build`) carries `-fno-sycl-rdc --offload-compress`.
  Default relocatable device code -> device codegen + ocloc run at LINK,
  link sees only `-fsycl` -> `spir64_gen` images silently dropped (bug
  ADR-0568 shipped with). Keep flags on compile line; keep
  `sycl_dependency.link_args` = `-fsycl` + fp32 precision pair, no targets,
  MSVC excepted (next bullet) (link-time targets = 167 s+ per link x 113
  test links of `libvmaf.a`, one ocloc crash aborts whole link). `spir64`
  JIT bitcode still device-linked at LINK under `-fno-sycl-rdc`: device
  flags go on compile line (AOT) AND link (JIT), ADR-1367.
  `find_program('ocloc')` error = intended fail-closed;
  never demote to warning. `sycl_aot_image_check`
  (`core/src/sycl/check_aot_image.py`) fails build unless `libvmaf.so` has
  image per requested GFX IP; `sycl_icpx_aot_igc_skip` = only allowed gap
  (`integer_psnr_hvs_sycl` minus Xe2, IGC 2.41.5 crash). Drop entry once
  reworked kernel from `fix/sycl-b580-psnr-hvs-adm-tiny` lands (compiles for
  Xe2 under ocloc 26.35). **On rebase**: keep all three pieces together.

- **AOT image check accepts both ocloc image forms.** ocloc writes one image
  per TU; form follows count of device acronyms in `-device`: >= 2 (even one
  shared IP, `dg2-g10,acm-g10`) -> fat binary, `ar` archive, members
  `<bits>.<arch.release.revision>`; exactly 1 -> bare zebin, plain ELF, no
  archive (`-Dsycl_icpx_aot_targets=dg2-g11`; TU that
  `sycl_icpx_aot_igc_skip` leaves one target). `check_aot_image.py` reads IP
  of bare zebin from `.note.intelgt.compat` (`SHT_NOTE`), IntelGT note type 6
  (`productConfig`, u32 laid out as NEO `HardwareIpVersion`: architecture
  31:22, release 21:14, revision 5:0) = token `ocloc ids` prints (upstream:
  intel/compute-runtime `zebin_elf.h`, `hw_ip_version.h`). Bare zebin = image
  carrying one IP -> same completeness + `--partial` rules as fat binary; IP
  no requested target uses -> fail; bytes neither `ar` nor ELF, or ELF
  without that note -> fail. Fat-binary-only check failed every
  single-target build. **On rebase**: keep both forms; never narrow check to
  `ar` archives. Guard: `core/test/test_sycl_aot_image_check.py` (synthetic
  ELF fixtures, no device).

- **MSVC builds register images through one explicit device link
  (ADR-1364).** Meson links MSVC-syntax toolchains (`icx-cl`) with
  `link.exe` directly; it drops `-fsycl` (LNK4044) and never wraps device
  code -> empty kernel registry, every submit `No kernel named ... was
  found`. Under `sycl_msvc_device_link` (`core/src/meson.build`): TUs compile
  RDC (`-fsycl -fsycl-targets=spir64_gen,spir64`, no `-fno-sycl-rdc`); one
  `icpx -fsycl -fsycl-link` (AOT `-device` list, `--offload-compress`,
  whole `sycl_strict_fp_args` incl. fp32 precision pair (ADR-1367),
  parallel link jobs) over `common_sycl_objects +
  sycl_feature_objects` -> `sycl_device_link_raw.obj`;
  `coff_add_anchor.py` adds external `vmaf_sycl_device_images` ->
  `sycl_device_link.obj`, appended to `sycl_feature_objects`;
  `common.cpp` `#pragma comment(linker, "/include:vmaf_sycl_device_images")`
  under `VMAF_SYCL_MSVC_DEVICE_LINK` pulls it out of `vmaf.lib`;
  `sycl_link_args` = `/IGNORE:4078` there, `-fsycl` everywhere else.
  `-fsycl-link` crashes on `-fno-sycl-rdc` objects (oneAPI 2025.1), so do
  not re-add it on this path. `sycl_icpx_aot_igc_skip` non-empty = configure
  error here. Guards: `test_sycl_kernel_registration` (no GPU; runs on
  `Windows MSVC+SYCL` leg), `test_sycl_coff_anchor`. **On rebase**: keep all
  pieces together; Linux path stays ADR-1360.

- **Feature-name aliasing when querying scores from non-default SYCL
  extractors.** Any `VMAF_OPT_FLAG_FEATURE_PARAM` option set to
  non-default value -> `vmaf_feature_name_from_opts_dict` stores
  scores under aliased name (not raw `VMAF_*_score` name). Aliased
  name: `<alias_of_raw_name>_<opt1_alias>_<opt2_alias>...` in
  alphabetical option order. Example: `motion_sycl` with
  `motion_add_uv=true` stores `VMAF_integer_feature_motion2_score` as
  `integer_motion2_mau`. Test code querying
  `vmaf_feature_score_at_index` must use aliased name, not raw name.
  (ADR-1099)

- **Primary queue stays on immediate command lists (ADR-3359).**
  `sycl_queue_props()` in `common.cpp` adds
  `sycl::ext::intel::property::queue::immediate_command_list` to the primary
  queue through `vmaf_sycl_in_order_props()` (`queue_props.h`, the one
  spelling, shared with `library_queue_props()`), which
  runs the VA import in `dmabuf_import.cpp`. Reason: with
  `UR_L0_USE_IMMEDIATE_COMMANDLISTS=0` an Arc A380 (compute-runtime 26.35)
  on i915 silently drops the import from a random frame on once the per-frame
  DMA-BUF import reuses the freed import's GPU address; zero-copy scores go
  stale, no error. Moving the import to its own immediate queue does NOT fix
  it (the primary queue's mode matters). **On rebase / refactor**: keep the
  property on the queue `vmaf_sycl_get_queue_ptr()` returns, and keep the
  import on that queue. Remove only after a driver fix, re-tested with
  `scripts/test/zerocopy-e2e.sh --stage 1 --repeat 10 --cases cambi` under
  `UR_L0_USE_IMMEDIATE_COMMANDLISTS=0` with the property removed (must show
  `PASS`, 0 of 10 runs differing, 8 and 10 bit).

- **`vmaf_sycl_print_timing()` logs through `vmaf_log` at INFO.** Every
  SYCL flush calls it, so it must follow the caller's log level (FFmpeg
  maps `-loglevel` to it). Do not turn it back into `fprintf(stderr, ...)`.
