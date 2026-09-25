# AGENTS.md — core/tools

Orientation for agents working on CLI binaries. Parent:
[../AGENTS.md](../AGENTS.md).

## Scope

Three C binaries built by libvmaf's Meson tree:

- `vmaf` — end-user scoring CLI
- `vmaf_bench` — micro-benchmark harness for extractors and backends
- `vmaf-perShot` — per-shot CRF predictor sidecar (T6-3b / ADR-0222)
- `vmaf_roi` — saliency-driven ROI sidecar emitter for x265 / SVT-AV1 (T6-2b)

```text
tools/
  vmaf.cpp            # main() + option dispatch for the vmaf CLI (C++23, ADR-0809)
  vmaf_bench.c        # main() + benchmark harness
  vmaf_per_shot.c     # main() + scan/predict for the perShot sidecar
  cli_parse.cpp/.h    # shared option parser (--precision, --tiny-model, …)
                      # cli_parse.c is the C twin compiled into the unit/fuzz
                      # tests only; keep it byte-for-byte behaviourally in sync
                      # with cli_parse.cpp.
  vmaf_roi.c          # main() + sidecar pipeline for vmaf-roi
  vmaf_roi_core.h     # pure helpers (per-CTU mean reduce, saliency->QP)
```

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md)).
- **Default numeric precision is `%.6f`** (Netflix-compatible — required by
  CLAUDE.md §8 golden gate). `--precision=max` (alias `full`) opts in to
  `%.17g` (IEEE-754 round-trip lossless). `--precision=N` overrides with
  `"%.<N>g"`; `--precision=legacy` is preserved as synonym for default.
  See [ADR-0119](../../docs/adr/0119-cli-precision-default-revert.md)
  (supersedes [ADR-0006](../../docs/adr/0006-cli-precision-17g-default.md)).
  Applies to both stderr and file outputs (XML / JSON / CSV / sub-XML).
- **`--tiny-model PATH`** loads ONNX checkpoint via
  [src/dnn/](../src/dnn/AGENTS.md). Path resolved via `realpath` inside
  loader; CLI passes string through unchanged. See
  [ADR-0023](../../docs/adr/0023-tinyai-user-surfaces.md).
- **No new hard dependencies** — CLI must still build when `enable_dnn=disabled`.
- **`--frame_skip_ref` / `--frame_skip_dist`** pre-loops in
  [vmaf.cpp](vmaf.cpp) MUST `vmaf_picture_unref()` each fetched picture
  immediately. Picture pool is always-on (see ADR-0104 below) and
  fixed-size; without unref pool exhausts after N skips, next
  fetch blocks indefinitely. Re-test with
  `python -m pytest python/test/command_line_test.py
  ::VmafexecCommandLineTest::test_run_vmafexec_with_frame_skipping` — if
  it hangs (timeout, no output), unref is missing or wrong.
- **EOF / read-error cleanup in `fetch_picture()` is load-bearing.**
  `fetch_picture()` reserves pooled `VmafPicture` before asking
  input reader for bytes. If reader returns EOF or error,
  reserved picture MUST be unrefed before `fetch_picture()` returns
  `1` (EOF) or `-1` (error). `run_frame_loop()` must also unref
  opposite picture when only one side read successfully. Otherwise
  CLI can finish writing output, then hang forever in `vmaf_close()`
  while picture pool waits for leaked unread slot.
- **`vmaf_roi` sidecar contract** (T6-2b / ADR-0247) is
  **rebase-sensitive** — encoder drivers depend on exact byte
  layouts:
  - `--encoder x265` emits ASCII per-row grid with two `#`-prefixed
    header lines (`# vmaf-roi qpfile (x265, --qpfile-style)` then
    `# frame=N ctu=S cols=C rows=R strength=F.FFF`), space-separated
    signed integers, one row per CTU row, `\n` terminator.
  - `--encoder svt-av1` emits exactly `cols * rows` bytes of `int8_t`,
    row-major, **no header**.
  - QP-offset clamp is `+-12` (`VMAF_ROI_CORE_QP_OFFSET_MAX`).
  - Reduction is per-CTU **mean** (not max — see ADR-0247 alternatives).
  - Pure helpers (`vmaf_roi_reduce_per_ctu`, `vmaf_roi_saliency_to_qp`)
    live in `vmaf_roi_core.h` so smoke test compiles them
    without dragging libvmaf's link surface in. **Never** move them
    into `.c` TU without revisiting test wiring.
  - Placeholder saliency map (when `--saliency-model` is absent)
    is for smoke-test plumbing only, explicitly documented as
    not-for-real-encodes in `docs/usage/vmaf-roi.md`.
  - `--bitdepth 8|10|12|16` is part of input contract. High-bit-depth
    planar YUV uses little-endian 16-bit containers; frame seeking must
    count chroma planes and sample width even though only luma enters
    saliency path. DNN-facing tensor remains luma8.
  - Private input helpers live in `vmaf_roi_input.h`, shared directly with
    `test_vmaf_roi_bounds`. Keep local depth/extent guards before shifts,
    allocation and reads; rounded high-bit-depth samples saturate at 255
    before `uint8_t` cast. `VMAF_ROI_MAX_DIM` remains existing
    16384 CLI limit. Placeholder traversal validates and uses caller's
    allocation count while preserving radial coordinate arithmetic.
    See [ROI boundary evidence](../../docs/research/roi-reader-bounds-2026-09-08.md).
- **Long-only options must not pass synthesised short-option
  chars to `error()`** (rebase-sensitive). Handlers for
  `ARG_THREADS`, `ARG_SUBSAMPLE`, `ARG_CPUMASK`, and any
  future `ARG_*` enum value `>= 256` MUST pass that enum value
  (not fabricated `'t'` / `'s'` / `'c'`) into
  `parse_unsigned()` / `parse_bitdepth()` / `error()`.
  `error()` table-walk over `long_opts[]` for non-existent
  short-option char trips `assert(long_opts[n].name)`,
  takes binary down with `SIGABRT`.
  `error()` `< 256` branch already handles long-only options
  via `--name` path; passing real enum value is
  required to reach it. See
  [ADR-0316](../../docs/adr/0316-cli-parse-long-only-error-fix.md);
  parked-then-promoted reproducer
  `core/test/fuzz/cli_parse_corpus/cli_threads_abbrev_assert.argv`
  protects rebase, and
  `core/test/test_cli_parse_long_only_args.c` protects
  unit-test path.
- **`cli_parse.cpp::usage()` discrete overloads** (rebase-sensitive).
  `usage()` provides discrete template overloads for 1, 2, and 3 arguments
  and no variadic parameter-pack fallback. This prevents zero-argument pack
  expansions that trip CodeQL
  `cpp/unused-local-variable` and `cpp/unused-static-variable` (Alerts 1002/1003).
  Do not collapse back into an unconstrained variadic pack without verifying
  CodeQL analysis. Adversarial regression coverage is pinned by
  `core/test/test_cli_parse_long_only_args.c`.
- **`y4m_convert_411_422jpeg` chroma-row write guards are
  load-bearing** (rebase-sensitive). 4:1:1 → 4:2:2-jpeg upsample
  in [y4m_input.c](y4m_input.c) writes both even and odd output
  sub-pixels per loop iteration. Destination chroma row width
  `dst_c_w` can be 1 (e.g. width-2 frame: `dst_c_w = (2 + 2 - 1) /
  2 = 1`), in which case writing `_dst[(x << 1) | 1]` = 1-byte
  heap-buffer-overflow. **All three sub-loops** in this routine must
  guard secondary write with `(x << 1 | 1) < dst_c_w`. Upstream
  Daala / Netflix carry same code shape; if `/sync-upstream`
  reintroduces unguarded write, re-apply fix. Regression
  test: `core/test/test_y4m_411_oob.c` (ASan-required to catch
  regression deterministically).

## Governing ADRs

- [ADR-0119](../../docs/adr/0119-cli-precision-default-revert.md) — `%.6f`
  default (Netflix-compat) + `--precision=max` for round-trip lossless.
  Supersedes ADR-0006.
- [ADR-0006](../../docs/adr/0006-cli-precision-17g-default.md) — *Superseded.*
  Original `%.17g`-default decision; kept for history.
- [ADR-0023](../../docs/adr/0023-tinyai-user-surfaces.md) — `--tiny-model`
  as one of four tiny-AI surfaces.
- [ADR-0222](../../docs/adr/0222-vmaf-per-shot-tool.md) — `vmaf-perShot`
  per-shot CRF predictor sidecar (T6-3b).
  - **Sidecar invariant**: this binary is **standalone** —
    does not link libvmaf metric path; its output is
    encoder hint, not quality score. Any future
    integration must keep per-shot prediction outside
    `vmaf_score_*` to preserve roadmap §2.4's separation.
  - **Schema invariant**: CSV / JSON columns
    (`shot_id`, `start_frame`, `end_frame`, `frames`,
    `mean_complexity`, `mean_motion`, `predicted_crf`)
    stable across v1; v2's trained MLP must reuse
    them to avoid downstream encoder churn.
  - **Input invariant**: `--pixel_format 420|422|444` only changes
    planar chroma-byte skipping. Per-shot detector and predictor
    remain luma-only, and high-bit-depth inputs use little-endian
    16-bit sample containers for `--bitdepth 10|12|16`.
  - **`--help` short-option is `-H`, NOT `-?`** (rebase-sensitive).
    getopt returns `'?'` for any unrecognised option; if `--help` maps
    to `'?'` two cases become indistinguishable, unknown flags
    silently succeed. `per_shot_long_opts` table maps `--help` to
    `'H'`; `per_shot_parse_args` handles `'H'` for help and `'?'` for
    error path. Never change short-option value.
  - **Scan stops at `VMAF_PER_SHOT_MAX_FRAMES` or `--frames` ceiling.** `per_shot_scan_loop`
    tracks frames in a `uint64_t` and `per_shot_record_frame` stores that
    index. An explicit operator ceiling `-F, --frames <N>` (with aliases
    `--frame_cnt` and `--max-frames`) bounds scans on FIFOs, streams, or
    synthetic inputs, exiting cleanly with code 0 on reaching N frames
    ([ADR-1318](../../docs/adr/1318-pershot-frames-ceiling.md)). The default
    is `0U` (unbounded), preserving full scans on finite files up to
    `VMAF_PER_SHOT_MAX_FRAMES` (`UINT32_MAX`), where exhaustion reports `-EFBIG`.
    Never restore a bare `for (;;)`. At the built-in boundary, the reader probes
    for one additional *complete* frame and checks that read before indexing it:
    an input of exactly `UINT32_MAX` frames is accepted when EOF is reached,
    reporting `-EFBIG` only if input strictly exceeds `UINT32_MAX` complete
    frames, resolving the off-by-one check from
    [ADR-1287](../../docs/adr/1287-cli-tool-unbounded-loop-ceilings.md).
  - **Raw-frame reads consume every luma and chroma byte** (rebase-sensitive).
    `vmaf_per_shot_read_luma` treats EOF as clean only before the first luma
    byte of a new frame. A short luma plane, short chroma planes, or `ferror`
    fails closed. Never restore seek-based chroma skipping: ISO C permits a
    regular file seek beyond EOF, so seek success does not prove that the raw
    frame is complete and can create a phantom final frame.
- `vmaf_vpl.c` — VPL decode -> SYCL pipeline (fork-local, not upstream).
  - **`vpl_decode_frame` retries under `VPL_DECODE_MAX_ATTEMPTS`.** The
    ceiling is *derived*: `VPL_SYNC_TIMEOUT_MS` / `VPL_DECODE_RETRY_US`, i.e.
    the 60 s timeout the same function already hands
    `MFXVideoCORE_SyncOperation()` at the 1 ms back-off it already used.
    Change one of the three macros and the other two must still describe the
    same wall clock. Never restore bare `for (;;)`, and never widen the
    ceiling without re-deriving it from a measured busy-loop distribution
    ([ADR-1287](../../docs/adr/1287-cli-tool-unbounded-loop-ceilings.md)).
  - **`VplFallbackState` flags are load-bearing, not defensive.**
    `vpl_fallback_release()` reads `have_ref_img` / `have_dis_img` /
    `have_ref_map` / `have_dis_map` / `have_ref_pic` / `have_dis_pic` to
    release exactly what the acquisition stages managed to take, in the order
    the retired `cleanup:` label used. Setting a flag without a matching
    release branch (or vice versa) leaks or double-frees; this is what
    replaced the `clang-analyzer-deadcode.DeadStores` NOLINT that used to sit
    on `have_dis_pic`.
- [ADR-0104](../../docs/adr/0104-picture-pool-always-on.md) — picture
  pool is always compiled in and sized for live-picture set; this
  is what makes `--frame_skip_*` unref invariant load-bearing.
- [ADR-0247](../../docs/adr/0247-vmaf-roi-tool.md) — `vmaf-roi`
  sidecar (per-CTU QP offsets for x265 / SVT-AV1). Encoder format
  contract + per-CTU-mean reduction are rebase-sensitive.
- [ADR-0461](../../docs/adr/0461-cli-validate-dimensions-chroma.md) —
  CLI rejects non-positive and chroma-misaligned input dimensions.
  **Validation invariant**: `validate_video_info()` and
  `validate_chroma_alignment()` = canonical per-stream and
  chroma-alignment gates; if upstream Netflix adds similar checks to
  `validate_videos()` in sync, merge rather than duplicate — keep
  fork's helpers, call them from merged body.
- [ADR-0977](../../docs/adr/0977-core-tools-input-reader-safety.md) —
  input-reader safety in vendored Daala YUV / Y4M parsers
  (`y4m_input.c`, `yuv_input.c`) and bench binary
  (`vmaf_bench.c`).
  **malloc-return invariant**: `y4m_input_open_impl` must check
  return of every `malloc()`, return -1 on NULL, freeing any
  partial allocation. Pre-fix code returned 0 on OOM and
  caller surfaced NULL `dst_buf` to next `fread`, crashing.
  Upstream Netflix/vmaf still carries unchecked variant; on
  `/sync-upstream` keep fork's NULL check + cleanup block.
  **size_t-precision invariant**: both readers compute `dst_buf_sz`
  with `(size_t)` cast applied to `pic_w` / `pic_h` (Y4M) and
  `width` / `height` (YUV) **before** multiply. 4:4:4 paths
  in `y4m_input.c` already cast for same reason. If upstream
  re-introduces `pic_w * pic_h` in `int` precision on sync, keep
  fork's cast. In `yuv_input.c` that cast now lives in
  `yuv_input_set_plane_geometry()`, which `yuv_input_open` calls in place
  of upstream's `switch` plus `goto fail` label. Helper returns -1 for
  unsupported `pix_fmt`; caller frees reader state and returns NULL, same
  as label did. Sync conflict here resolves to fork's helper, not
  upstream's label — cast must not follow label back.
  **bench GPU-state lifetime invariant**:
  `BenchGpuState` owns CUDA / SYCL handles. `bench_feature()` and
  `run_feature_collect()` execute guarded stages, then call
  `bench_cleanup_resources()` exactly once; `run_sycl_gpu_profile()` does the
  same through `cleanup_sycl_profile()`. Keep context close before GPU-state
  free and do not add early returns after ownership begins.
- [ADR-0520](../../docs/adr/0520-cli-no-reference-wiring.md) —
  `--no-reference` wiring.
  **CLI gate invariant**: reference-required gate at end of
  `cli_parse()` must remain conditional on `!settings->no_reference`;
  NR branch must require `tiny_model_path`, force
  `no_prediction = true` so built-in `vmaf_v0.6.1` SVM is not
  auto-injected (SVM consumes FR feature columns, would always
  fail downstream). If `/sync-upstream` reintroduces unconditional
  `if (!settings->path_ref)` block, restore `no_reference` guard.
  **Frame-loop invariant**: in NR mode `vmaf.cpp::open_cli_inputs` opens
  distorted source twice (two `video_input` handles) so
  `vmaf_read_pictures` receives non-null picture pair; this
  satisfies public-API contract without exposing new entry
  point. Never collapse two opens into single handle —
  per-frame `vmaf_picture_unref` cleanup walks both slots
  independently, single-slot reuse would cause use-after-free.
  Rank-4 DNN dispatch in `libvmaf.c::vmaf_ctx_dnn_run_frame_nchw`
  reads picture data exclusively from `ref` argument, is
  *only* downstream consumer that legitimately observes that slot in
  NR mode.
- [ADR-1155](../../docs/adr/1155-tools-upstream-mirror-rework.md) —
  **Upstream-mirror tool TUs lint rework (0 clang-tidy warnings)**.
  `cli_parse.cpp`, `cli_parse.h`, `y4m_input.c`, `vmaf.cpp`,
  and `vmaf_bench.c` reworked to fork lint profile.
  **Rebase invariant**:
  - `cli_parse.c` was resolved as dead twin under ADR-1153 precedent
    (zero unique behavior vs `cli_parse.cpp`), deleted; `test_cli_parse`,
    `test_cli_parse_long_only_args`, and `fuzz_cli_parse` compile `cli_parse.cpp`.
    Never reintroduce `cli_parse.c`.
  - C translation units (`y4m_input.c`, `vmaf_bench.c`) MUST keep `NULL`
    (ADR-1138), suppress `modernize-use-nullptr` using file-scoped
    NOLINTBEGIN/NOLINTEND brackets to preserve MSVC `/std:clatest` Windows portability.
  - In `y4m_input.c`, all plane dimensions, strides, and buffer index
    calculations use `ptrdiff_t` / `size_t` precision to avoid 32-bit
    multiplication overflow.
  - In `vmaf.cpp`, `CliRunState` owns files, input readers, context, GPU
    handles and model arrays. `CliRunGuard` invokes one ordered cleanup path on
    every return after parsing: context, GPU handles, readers, files, CLI
    settings, then model arrays. Internal declarations live in short, reopened
    anonymous-namespace blocks so clang-tidy sees internal linkage while every
    scanner-visible block remains within the 60-line HISS limit. Do not merge
    those blocks back into one file-wide namespace.

- [ADR-1190](../../docs/adr/1190-cli-option-string-escape-grammar.md) —
  **Escape-aware `--model` / `--feature` option-string splitting.**
  `cli_parse.cpp` no longer contains `strsep` (nor `vmaf_cli_strsep`
  shim or its `#ifndef HAVE_STRSEP` fork); nine split sites all go
  through `cli_split()` plus `cli_unescape()`.
  **Rebase invariants**:
  - Splitting and unescaping are two passes. `cli_split()` must leave
    backslash sequences intact — escape written for `:` pass stays
    literal at `=` pass. `cli_unescape()` must run exactly once per
    token, after last split that token undergoes.
    Unescaping earlier eats user's literal backslash; unescaping twice
    eats it again.
  - Key/value pair's value is whole remainder after first
    unescaped `=` — never second split. Removed second split =
    silent-truncation bug (`path=/a/dir=eq/m.json` became `/a/dir`), so if
    `/sync-upstream` restores `strsep(&key_val, "=")` pair, drop it.
  - `apply_model_opt()` splits overload key on `.` **before**
    unescaping it, compares *raw* key against `path` / `name` /
    `version` / `disable_clip` / `enable_transform` (none of which contain
    escapable byte, so comparison is unambiguous).
  - `cli_is_drive_colon()` is ergonomics affordance, not
    optimisation: dropping it makes every Windows `path=C:\...` require
    `C\:`, which is user-visible complaint Netflix/vmaf#766 filed.
  - Go escaper `pkg/cliopt.EscapeValue` = same grammar in another
    language; change both (and its round-trip test) together.

## Progress-line rendering is console-capability-driven (ADR-1166)

`spinner.h` now carries two glyph tables and two selectors, and `vmaf.cpp`
resolves them from console's **actual** capabilities:

- `spinner[]` — upstream UTF-8 braille table. Byte-for-byte unchanged;
  `core/test/test_spinner.cpp` pins first and last entries, asserts
  every entry is exactly 6 bytes, so well-meaning re-encode (universal
  character names, different braille range) fails fast suite. Never
  rewrite these literals as `\uXXXX` escapes: MSVC's narrow execution charset
  is ANSI code page, where they would not round-trip.
- `spinner_ascii[]` + `spinner_table_for_codepage()` + `spinner_erase_eol()` —
  fallback for console reporting non-UTF-8 code page or refusing VT
  processing.

On POSIX both selectors called with `SPINNER_CODEPAGE_UTF8` and
`vt_enabled = 1`, so emitted bytes are identical to pre-ADR-1166 form.
Keep it that way — golden-gate CLI invocations parse this stream.

`WindowsConsoleGuard` in `vmaf.cpp` has static storage and is initialised
before `cli_parse()`. That is load-bearing: `cli_parse()` calls `exit()` for
help, version and parse errors, which skips automatic destructors but runs
static destructors. `CliRunGuard` is created immediately after successful
parsing and owns all ordinary-return cleanup.

## Windows CLI arguments are strict UTF-8 (ADR-1182 follow-up)

The Windows `vmaf` and `vmafx` targets enter through `wmain`, convert every
UTF-16 token with `WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ...)`,
and only then call the parser shared with POSIX `main`. Keep the conversion
before `cli_parse()`: reference, distorted, output, and model paths must reach
the existing UTF-8 path layer before any option handler can copy them. Invalid
UTF-16 must fail closed, not use replacement characters.

GNU-style Windows linkers need `-municode` on both CLI targets so CRT startup
selects `wmain`; MSVC-style linkers infer the entry point. Do not apply that
flag to unrelated tools with narrow `main`. The Windows-only
`test_vmaf_windows_utf8_argv` regression launches the built binary through
`CreateProcessW` and checks the exact accented+CJK output path. POSIX entry and
argument bytes remain unchanged.

## `parse_unsigned` rejects negatives on purpose (ADR-1209)

`parse_unsigned` refuses leading `'-'` before calling `strtoul`, because
POSIX `strtoul` silently converts `"-1"` to `ULONG_MAX` without setting
`errno`. Upstream relies on that wraparound — its own
`test_vmaf_cuda_gpumask.sh` passes `--gpumask -1`, expects it to mean "all
bits set". Never loosen check to make inherited script pass; fix
caller instead. `--gpumask 1` means same thing, says so.

More generally, `--gpumask` is not per-op bitmask despite `$bitmask`
placeholder: passing flag opts into GPU backend selection, any non-zero
value then disables GPU feature extractors, so run falls back to CPU.
`--gpumask 0` = use GPU, `--gpumask 1` = use CPU.

## Read failures exit 102, short streams exit 0 (ADR-1262)

`run_frame_loop()` returns `FrameLoopResult { frames, exit_code }`, not a bare
count. Returning only count is why `vmaf` used to exit 0 on every read
failure and still write a report over truncated prefix.

`classify_frame_fetch()` tests error **before** end of stream. Order is
load-bearing: `fetch_picture()` gives `1` at EOF, `-1` on error, so
`ret1 && ret2` is true when both sides FAIL. Testing it first classifies two
corrupt inputs as clean end of stream — silent, exit 0. Upstream still has that
order (Netflix/vmaf#1604, known, unfixed), so rebase conflict offers it as
"theirs". Keep ours.

A stream that ENDS earlier than partner is not an error: keeps `ended before`
warning, keeps report, exits 0. Scoring common prefix of shorter clip is
supported use. Do not fold two cases together.

`core/tools/test/test_vmaf_read_error_exit.sh` pins all four cases, `fast` suite.

## GPU-tagged tool tests run exclusively

Every test under `core/tools/test/` carrying the Meson `gpu` suite tag must
also set `is_parallel : false`. These shell-driven CLI tests consume the same
physical accelerator as the kernel tests under `core/test/`; leaving either
`test_vmaf_cuda_gpumask` or a `test_vmaf_<backend>_threads` registration
parallel defeats the shared-device scheduling contract. The global
`check_gpu_test_serialization` test reads Meson introspection across the whole
project, so keep these registrations visible to it and do not replace the
suite tag with a local-only convention.
