# AGENTS.md — core/tools

Orientation for agents working on the CLI binaries. Parent:
[../AGENTS.md](../AGENTS.md).

## Scope

Three C binaries built by libvmaf's Meson tree:

- `vmaf` — the end-user scoring CLI
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
  `"%.<N>g"`; `--precision=legacy` is preserved as a synonym for the default.
  See [ADR-0119](../../docs/adr/0119-cli-precision-default-revert.md)
  (supersedes [ADR-0006](../../docs/adr/0006-cli-precision-17g-default.md)).
  This applies to both stderr and file outputs (XML / JSON / CSV / sub-XML).
- **`--tiny-model PATH`** loads an ONNX checkpoint via
  [src/dnn/](../src/dnn/AGENTS.md). Path is resolved via `realpath` inside
  the loader; the CLI passes the string through unchanged. See
  [ADR-0023](../../docs/adr/0023-tinyai-user-surfaces.md).
- **No new hard dependencies** — the CLI must still build when `enable_dnn=disabled`.
- **`--frame_skip_ref` / `--frame_skip_dist`** pre-loops in
  [vmaf.cpp](vmaf.cpp) MUST `vmaf_picture_unref()` each fetched picture
  immediately. The picture pool is always-on (see ADR-0104 below) and
  fixed-size; without unref the pool exhausts after N skips and the next
  fetch blocks indefinitely. Re-test with
  `python -m pytest python/test/command_line_test.py
  ::VmafexecCommandLineTest::test_run_vmafexec_with_frame_skipping` — if
  it hangs (timeout, no output), the unref is missing or wrong.
- **EOF / read-error cleanup in `fetch_picture()` is load-bearing.**
  `fetch_picture()` reserves a pooled `VmafPicture` before asking the
  input reader for bytes. If the reader returns EOF or an error, the
  reserved picture MUST be unrefed before `fetch_picture()` returns
  `1` (EOF) or `-1` (error). `run_frame_loop()` must also unref the
  opposite picture when only one side read successfully. Otherwise the
  CLI can finish writing output and then hang forever in `vmaf_close()`
  while the picture pool waits for the leaked unread slot.
- **`vmaf_roi` sidecar contract** (T6-2b / ADR-0247) is
  **rebase-sensitive** — encoder drivers depend on the exact byte
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
    live in `vmaf_roi_core.h` so the smoke test compiles them
    without dragging libvmaf's link surface in. **Do not** move them
    into a `.c` TU without revisiting the test wiring.
  - The placeholder saliency map (when `--saliency-model` is absent)
    is for smoke-test plumbing only and explicitly documented as
    not-for-real-encodes in `docs/usage/vmaf-roi.md`.
  - `--bitdepth 8|10|12|16` is part of the input contract. High-bit-depth
    planar YUV uses little-endian 16-bit containers; frame seeking must
    count the chroma planes and sample width even though only luma enters
    the saliency path. The DNN-facing tensor remains luma8.
- **Long-only options must not pass synthesised short-option
  chars to `error()`** (rebase-sensitive). Handlers for
  `ARG_THREADS`, `ARG_SUBSAMPLE`, `ARG_CPUMASK`, and any
  future `ARG_*` enum value `>= 256` MUST pass that enum value
  (not a fabricated `'t'` / `'s'` / `'c'`) into
  `parse_unsigned()` / `parse_bitdepth()` / `error()`. The
  `error()` table-walk over `long_opts[]` for a non-existent
  short-option char trips `assert(long_opts[n].name)` and
  takes the binary down with `SIGABRT`. The
  `error()` `< 256` branch already handles long-only options
  via the `--name` path; passing the real enum value is
  required to reach it. See
  [ADR-0316](../../docs/adr/0316-cli-parse-long-only-error-fix.md);
  the parked-then-promoted reproducer
  `core/test/fuzz/cli_parse_corpus/cli_threads_abbrev_assert.argv`
  protects the rebase, and
  `core/test/test_cli_parse_long_only_args.c` protects
  the unit-test path.
- **`y4m_convert_411_422jpeg` chroma-row write guards are
  load-bearing** (rebase-sensitive). The 4:1:1 → 4:2:2-jpeg upsample
  in [y4m_input.c](y4m_input.c) writes both even and odd output
  sub-pixels per loop iteration. The destination chroma row width
  `dst_c_w` can be 1 (e.g. a width-2 frame: `dst_c_w = (2 + 2 - 1) /
  2 = 1`), in which case writing `_dst[(x << 1) | 1]` is a 1-byte
  heap-buffer-overflow. **All three sub-loops** in this routine must
  guard the secondary write with `(x << 1 | 1) < dst_c_w`. Upstream
  Daala / Netflix carry the same code shape; if `/sync-upstream`
  reintroduces the unguarded write, re-apply the fix. Regression
  test: `core/test/test_y4m_411_oob.c` (ASan-required to catch
  the regression deterministically).

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
    it does not link the libvmaf metric path; its output is
    an encoder hint, not a quality score. Any future
    integration must keep the per-shot prediction outside
    `vmaf_score_*` to preserve roadmap §2.4's separation.
  - **Schema invariant**: CSV / JSON columns
    (`shot_id`, `start_frame`, `end_frame`, `frames`,
    `mean_complexity`, `mean_motion`, `predicted_crf`)
    are stable across v1; v2's trained MLP must reuse
    them to avoid downstream encoder churn.
  - **Input invariant**: `--pixel_format 420|422|444` only changes
    planar chroma-byte skipping. The per-shot detector and predictor
    remain luma-only, and high-bit-depth inputs use little-endian
    16-bit sample containers for `--bitdepth 10|12|16`.
  - **`--help` short-option is `-H`, NOT `-?`** (rebase-sensitive).
    getopt returns `'?'` for any unrecognised option; if `--help` maps
    to `'?'` the two cases become indistinguishable and unknown flags
    silently succeed. The `per_shot_long_opts` table maps `--help` to
    `'H'`; `per_shot_parse_args` handles `'H'` for help and `'?'` for
    the error path. Do not change the short-option value.
  - **Chroma skip uses `fseeko` / `_fseeki64`** (rebase-sensitive).
    `per_shot_read_luma` skips chroma bytes via `fseeko` (POSIX) or
    `_fseeki64` (WIN32). Do not revert to `fseek((long)...)` — the
    `long` cast silently truncates on 32-bit targets for frames
    larger than 2 GiB, seeking to the wrong position without error.
- [ADR-0104](../../docs/adr/0104-picture-pool-always-on.md) — picture
  pool is always compiled in and sized for the live-picture set; this
  is what makes the `--frame_skip_*` unref invariant load-bearing.
- [ADR-0247](../../docs/adr/0247-vmaf-roi-tool.md) — `vmaf-roi`
  sidecar (per-CTU QP offsets for x265 / SVT-AV1). Encoder format
  contract + per-CTU-mean reduction are rebase-sensitive.
- [ADR-0461](../../docs/adr/0461-cli-validate-dimensions-chroma.md) —
  CLI rejects non-positive and chroma-misaligned input dimensions.
  **Validation invariant**: `validate_video_info()` and
  `validate_chroma_alignment()` are the canonical per-stream and
  chroma-alignment gates; if upstream Netflix adds similar checks to
  `validate_videos()` in a sync, merge rather than duplicate — keep the
  fork's helpers and call them from the merged body.
- [ADR-0977](../../docs/adr/0977-core-tools-input-reader-safety.md) —
  input-reader safety in the vendored Daala YUV / Y4M parsers
  (`y4m_input.c`, `yuv_input.c`) and the bench binary
  (`vmaf_bench.c`).
  **malloc-return invariant**: `y4m_input_open_impl` must check the
  return of every `malloc()` and return -1 on NULL, freeing any
  partial allocation. The pre-fix code returned 0 on OOM and the
  caller surfaced a NULL `dst_buf` to the next `fread`, crashing.
  Upstream Netflix/vmaf still carries the unchecked variant; on
  `/sync-upstream` keep the fork's NULL check + cleanup block.
  **size_t-precision invariant**: both readers compute `dst_buf_sz`
  with the `(size_t)` cast applied to `pic_w` / `pic_h` (Y4M) and
  `width` / `height` (YUV) **before** the multiply. The 4:4:4 paths
  in `y4m_input.c` already cast for the same reason. If upstream
  re-introduces `pic_w * pic_h` in `int` precision on a sync, keep
  the fork's cast.
  **bench GPU-state lifetime invariant**:
  `vmaf_bench::bench_feature` declares `cu_state` / `sycl_state` at
  function scope and routes every exit through the `bench_cleanup`
  label so `vmaf_*_state_free` always runs. Mirrors the T5
  state-leak audit pattern in the same file's `run_feature_collect`.
- [ADR-0520](../../docs/adr/0520-cli-no-reference-wiring.md) —
  `--no-reference` wiring.
  **CLI gate invariant**: the reference-required gate at the end of
  `cli_parse()` must remain conditional on `!settings->no_reference`;
  the NR branch must require `tiny_model_path` and force
  `no_prediction = true` so the built-in `vmaf_v0.6.1` SVM is not
  auto-injected (the SVM consumes FR feature columns and would always
  fail downstream). If `/sync-upstream` reintroduces an unconditional
  `if (!settings->path_ref)` block, restore the `no_reference` guard.
  **Frame-loop invariant**: in NR mode `vmaf.cpp::main` opens the
  distorted source twice (two `video_input` handles) so
  `vmaf_read_pictures` receives a non-null picture pair; this
  satisfies the public-API contract without exposing a new entry
  point. Do NOT collapse the two opens into a single handle — the
  per-frame `vmaf_picture_unref` cleanup walks both slots
  independently and a single-slot reuse would cause a use-after-free.
  The rank-4 DNN dispatch in `libvmaf.c::vmaf_ctx_dnn_run_frame_nchw`
  reads picture data exclusively from the `ref` argument and is the
  *only* downstream consumer that legitimately observes that slot in
  NR mode.
- [ADR-1155](../../docs/adr/1155-tools-upstream-mirror-rework.md) —
  **Upstream-mirror tool TUs lint rework (0 clang-tidy warnings)**.
  `cli_parse.cpp`, `cli_parse.h`, `y4m_input.c`, `vmaf.cpp`,
  and `vmaf_bench.c` are reworked to the fork lint profile.
  **Rebase invariant**:
  - `cli_parse.c` was resolved as a dead twin under ADR-1153 precedent
    (zero unique behavior vs `cli_parse.cpp`) and deleted; `test_cli_parse`,
    `test_cli_parse_long_only_args`, and `fuzz_cli_parse` compile `cli_parse.cpp`.
    Do not reintroduce `cli_parse.c`.
  - C translation units (`y4m_input.c`, `vmaf_bench.c`) MUST keep `NULL`
    (ADR-1138) and suppress `modernize-use-nullptr` using file-scoped
    NOLINTBEGIN/NOLINTEND brackets to preserve MSVC `/std:clatest` Windows portability.
  - In `y4m_input.c`, all plane dimensions, strides, and buffer index
    calculations use `ptrdiff_t` / `size_t` precision to avoid 32-bit
    multiplication overflow.
  - In `vmaf.cpp`, `ModelArrays` is encapsulated with private members and RAII
    accessors; all internal helpers reside in an anonymous namespace.

- [ADR-1190](../../docs/adr/1190-cli-option-string-escape-grammar.md) —
  **Escape-aware `--model` / `--feature` option-string splitting.**
  `cli_parse.cpp` no longer contains `strsep` (nor the `vmaf_cli_strsep`
  shim or its `#ifndef HAVE_STRSEP` fork); the nine split sites all go
  through `cli_split()` plus `cli_unescape()`.
  **Rebase invariants**:
  - Splitting and unescaping are two passes. `cli_split()` must leave
    backslash sequences intact so an escape written for the `:` pass is
    still literal at the `=` pass, and `cli_unescape()` must run exactly
    once per token, after the last split that token will undergo.
    Unescaping earlier eats a user's literal backslash; unescaping twice
    eats it again.
  - A key/value pair's value is the whole remainder after the first
    unescaped `=` — never a second split. The removed second split is the
    silent-truncation bug (`path=/a/dir=eq/m.json` became `/a/dir`), so if
    `/sync-upstream` restores a `strsep(&key_val, "=")` pair, drop it.
  - `apply_model_opt()` splits the overload key on `.` **before**
    unescaping it, and compares the *raw* key against `path` / `name` /
    `version` / `disable_clip` / `enable_transform` (none of which contain
    an escapable byte, so the comparison is unambiguous).
  - `cli_is_drive_colon()` is an ergonomics affordance, not an
    optimisation: dropping it makes every Windows `path=C:\...` require
    `C\:`, which is the user-visible complaint Netflix/vmaf#766 filed.
  - The Go escaper `pkg/cliopt.EscapeValue` is the same grammar in another
    language; change both (and its round-trip test) together.

## Progress-line rendering is console-capability-driven (ADR-1166)

`spinner.h` now carries two glyph tables and two selectors, and `vmaf.cpp`
resolves them from the console's **actual** capabilities:

- `spinner[]` — the upstream UTF-8 braille table. Byte-for-byte unchanged;
  `core/test/test_spinner.cpp` pins the first and last entries and asserts
  every entry is exactly 6 bytes, so a well-meaning re-encode (universal
  character names, a different braille range) fails the fast suite. Do not
  rewrite these literals as `\uXXXX` escapes: MSVC's narrow execution charset
  is the ANSI code page, where they would not round-trip.
- `spinner_ascii[]` + `spinner_table_for_codepage()` + `spinner_erase_eol()` —
  the fallback for a console that reports a non-UTF-8 code page or refuses VT
  processing.

On POSIX both selectors are called with `SPINNER_CODEPAGE_UTF8` and
`vt_enabled = 1`, so the emitted bytes are identical to the pre-ADR-1166 form.
Keep it that way — the golden-gate CLI invocations parse this stream.

`WindowsConsoleGuard` in `vmaf.cpp` is declared in `main()` **before every
`goto cleanup` target**, which is what makes the restore run on the error
paths. Moving its declaration below a jump target is ill-formed C++ and would
silently leave the user's console in UTF-8 + VT mode after an error exit.
