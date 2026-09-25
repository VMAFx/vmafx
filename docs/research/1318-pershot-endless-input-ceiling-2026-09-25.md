<!-- markdownlint-disable MD013 MD060 -->
# Research-1318: Bounded vmaf-perShot scan loop with operator frame ceiling

## Scope

This digest covers `T-PER-SHOT-ENDLESS-INPUT-NOT-A-TIMEOUT-2026-09-21`: resolving the
operator hang on endless/stream inputs in `vmaf-perShot` (`core/tools/vmaf_per_shot.c`),
providing the smallest backward-compatible operator-visible bound, and fixing the
`UINT32_MAX` off-by-one boundary check from ADR-1287 under [ADR-1318](../adr/1318-pershot-frames-ceiling.md).

## Reproducer and root cause

In ADR-1287, `per_shot_scan_loop()` was bounded using `VMAF_PER_SHOT_MAX_FRAMES` (`UINT32_MAX`).
While this satisfied NASA/JPL Power of 10 Rule 2 for static loop boundedness and prevented
`uint32_t` counter overflow in shot records, it did not offer a practical operator escape
hatch for endless streams:

1. **Endless input hang**: A reference path that never reaches end of file (such as a FIFO
   held open by a live writer, or `/dev/zero`) required reading ~4.29 billion frames
   (~1.2 PB at 576x324 YUV420) before hitting `UINT32_MAX`. In interactive use or CI/CD
   pipelines, this appeared as an unescapable infinite hang.
   Reproducer:

   ```bash
   mkfifo /tmp/endless.yuv
   yes 2>/dev/null | tr -d '\n' > /tmp/endless.yuv &
   core/build/tools/vmaf-perShot --reference /tmp/endless.yuv \
     --width 16 --height 16 --pixel_format 420 --bitdepth 8 \
     --output /tmp/plan.csv
   # Hangs indefinitely until cancelled or killed
   ```

2. **Off-by-one boundary error**: ADR-1287 tested `ctx->frame_idx >= VMAF_PER_SHOT_MAX_FRAMES`
   after incrementing and exiting the loop. Consequently, an input containing exactly
   `UINT32_MAX` frames was rejected with `-EFBIG` even though all frames fit into `uint32_t`
   without wrapping, limiting the largest accepted input to `UINT32_MAX - 1` frames.

3. **ADR-1287 alternative rejection**: ADR-1287 rejected `--max-frames` under the assumption
   that it would default to an arbitrary small number that would silently truncate valid
   content.

## Options

| Approach | Preserves finite streams | Bounded on FIFOs | Backward compatible | Decision |
| --- | --- | --- | --- | --- |
| Wall-clock timeout on scan loop | Contention-sensitive | Yes | Breaks long legitimate scans | Rejected: nondeterministic under load. |
| Small default frame ceiling | Truncates long clips | Yes | No: breaks existing workflows | Rejected: silent data loss on long videos. |
| Reject non-regular files (`S_ISFIFO`) | Yes | Partial (misses `/dev/zero`) | Breaks UNIX pipeline composition | Rejected: pipes from ffmpeg are legitimate. |
| **Explicit `--frames` flag defaulting to 0 (unbounded)** | **Yes** | **Yes (when specified)** | **Yes (100% backward compatible)** | **Selected (ADR-1318).** |

## Implementation

1. **Settings and CLI Options**:
   `struct vmaf_per_shot_settings` gains `uint32_t max_frames;` defaulting to `0U`.
   Option `-F, --frames <N>` is added with aliases `--frame_cnt <N>` and `--max-frames <N>`.
   Parsing via `per_shot_parse_uint(optarg_, 0U, VMAF_PER_SHOT_MAX_FRAMES, &uv)` ensures
   strictly non-negative values within `[0, UINT32_MAX]`.

2. **HISS-04 LOC Compliance**:
   `per_shot_apply_opt()` combined the `'w'`/`'h'` and `'m'`/`'M'` switch cases, reducing
   function length from 57 LOC to 56 LOC (well within the HISS-04 cap of 60 LOC).

3. **64-bit Frame Indexing and Off-by-One Fix**:
   `ctx->frame_idx` in `struct per_shot_scan_ctx` is widened to `uint64_t`.
   The scan loop ceiling is evaluated as:

   ```c
   const uint64_t ceiling =
       (s->max_frames > 0U) ? (uint64_t)s->max_frames : ((uint64_t)VMAF_PER_SHOT_MAX_FRAMES + 1ULL);
   ```

   At `ctx->frame_idx >= VMAF_PER_SHOT_MAX_FRAMES`, an attempt to read the next frame is made.
   If EOF is reached, the scan completes cleanly with exit code 0, accepting an input of
   exactly `UINT32_MAX` frames. Only if another frame actually exists is `-EFBIG` returned.

4. **Deterministic Testing**:
   Sections 7–12 added to `core/tools/test/test_vmaf_per_shot.sh`:
   - Section 7: `--frames 10` on 48-frame fixture terminates at 10 frames.
   - Section 8: `--frames 0` preserves full 48-frame scan.
   - Section 9: Aliases `-F 10`, `--frame_cnt 10`, and `--max-frames 10` yield identical output.
   - Section 10: Bounded read on `/dev/zero` with `--frames 6` terminates in milliseconds.
   - Section 11: Bounded read on live endless FIFO terminates cleanly for 5 frames without hanging.
   - Section 12: Negative and non-numeric `--frames` values fail during option parsing.

## Evidence

### Red Phase (prior to implementation)

```text
$ core/build/tools/vmaf-perShot --reference testdata/src.yuv --width 576 --height 324 \
    --pixel_format 420 --bitdepth 8 --output /tmp/plan.csv --frames 10
vmaf-perShot: unrecognized option '--frames'
Usage: vmaf-perShot ...
```

### Green Phase (post implementation)

```text
$ meson test -C core/build test_vmaf_per_shot
1/1 test_vmaf_per_shot OK   0.07s

$ meson test -C core/build --suite=fast
158/158 test OK   5.00s
```

### Governance and Static Analysis

```text
$ clang-format -n --Werror core/tools/vmaf_per_shot.c
(zero warnings / violations)

$ praetorctl audit
[PASS] HISS invariant scan verified: 213 active violations within 229 baselined limit (10 touched files clean).
Audit Summary: configured governance gates passed for vmafx/vmafx.
```

## Limits and retained behavior

- Unbounded mode (`--frames 0`, the default) retains the full-scan behavior up to `UINT32_MAX` frames.
- Endless sources without `--frames` will continue reading; operators should supply `--frames` when reading from pipes or synthetic devices.
- Netflix golden assertions and public libvmaf C API remain 100% untouched.
