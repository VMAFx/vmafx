<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1121: SYCL QSV zero-copy — P010 pixel normalization and separate-session decode contract

- **Status**: Accepted
- **Date**: 2026-06-30
- **Deciders**: Lusoris Dev (<dsp@mvdnet.org>)
- **Tags**: `sycl`, `gpu`, `ffmpeg`, `correctness`, `perf`, `dispatch`

## Context

The FFmpeg `libvmaf_sycl` filter runs VMAF directly on VA-API surfaces from a
QSV-decoded reference/distorted pair (Intel Arc), importing the decoder's
dmabuf into Level Zero with no host round-trip. On a matched 4K HEVC→AV1 pair
it returned `VMAF score: nan` with `integer_motion` in the thousands and a
varying NaN count between runs — while the host-upload `libvmaf` filter on the
same machine and pair was exact. The defect had been mis-scoped across two
prior fix attempts (a single-slot orchestration theory, then a slot-toggle fix
"FIX-01") that did not move the numbers. A fresh scientific-method
investigation (see [research digest](../research/2026-06-30-sycl-qsv-zerocopy-two-bug-investigation.md))
eliminated 16 hypotheses and isolated **two independent root causes**, neither
of which was the orchestration loop:

1. **Shared QSV/VA surface pool (contamination).** When both decoders are
   created against the same `-hwaccel_device`, FFmpeg gives them a shared
   `AVHWFramesContext` → one `mfxSession` → one `vaCreateSurfaces` pool. The
   two decoders hold distinct `mfxFrameSurface1` wrappers that map to the *same*
   physical `VASurfaceID`s. `Data.Locked` on the AV1 (dis) wrapper does not stop
   HEVC (ref) from decoding `ref[N±1]` into that same surface in decode order,
   so the filter reads ref content where it expected dis. This produced the
   `sycl_dis[N] == sycl_ref[N±1]` checksum signature that earlier sessions read
   as a libvmaf buffer-aliasing bug.
2. **P010 MSB/LSB pixel-range mismatch (NaN + 64× motion).** VA-API stores
   10-bit (P010) / 12-bit (P012) samples MSB-aligned: `V_MSB = V_LSB << (16−bpc)`.
   The VMAF feature kernels expect LSB-aligned `bpc`-bit integers (`[0, 2^bpc−1]`).
   The CPU `libvmaf` path never sees this because FFmpeg auto-converts
   `P010LE → YUV420P10LE` (a `>>6` shift) to satisfy the filter's `FILTER_PIXFMTS`
   (which does not list `P010LE`). Every SYCL import path copied the raw
   MSB-aligned bytes, inflating `integer_motion` by exactly `2^(16−bpc)` (64× at
   10-bit) and driving downstream ADM/VIF to `-nan`.

## Decision

We will fix the zero-copy path with three coordinated changes and one baseline
update:

1. **Normalize P010/P012 MSB→LSB in the import (FIX-04).** Right-shift each luma
   `uint16_t` by `(16−bpc)` so the SYCL feature kernels see the same LSB-aligned
   range as the CPU path. For the tiled paths (Tile4 / Y-tiled — the QSV/DG2 hot
   path) the shift is **fused into the de-tile store kernel**: each sample is
   shifted as it is written, with no extra kernel launch and no second
   full-plane memory pass. The rare non-tiling paths (readback memcpy, DMA-BUF
   LINEAR D2D) have no per-sample store to fuse into, so they keep a standalone
   `launch_p010_normalize()` kernel whose event is threaded into
   `vmaf_sycl_set_detile_event()`. No-op for 8-bit NV12. Only luma is imported
   and only luma feeds VMAF, so chroma needs no normalization.
2. **Require separate per-decoder QSV sessions (FIX-03), documented not
   auto-enforced.** The supported invocation gives each decoder its own
   `-init_hw_device qsv=…` so VA surface IDs never overlap. This is a usage
   contract documented in [docs/backends/sycl/overview.md](../backends/sycl/overview.md),
   not a code-side guard (see Alternatives). This — not any cache flush — is the
   actual contamination fix.
3. **No DMA-BUF coherency sync.** A `DMA_BUF_IOCTL_SYNC(READ)` flush around the
   Level Zero import was tried as a candidate fix and proven insufficient (it
   only shifted the stale-frame offset). Because its `SYNC_START` is a blocking
   decoder-fence wait that serialises decode→compute and measurably cut 4K
   throughput, it is **not** kept: `vaSyncSurface()` already establishes decode
   completion upstream and the in-order SYCL queue orders the de-tile after the
   import. (Earlier revisions of this branch retained it as "hardening"; the
   perf cost with zero correctness benefit removed that justification.)
4. **Update the D-03 oracle baseline.** The previous baseline (VMAF 96.133894,
   `integer_motion` max 26.627813) was itself measured under the shared-session
   bug and is wrong. The de-contaminated CPU oracle (separate sessions) is
   **VMAF 97.2350, `integer_motion` max 26.6935**; the SYCL path matches it to
   three significant figures (Δ 0.0014 VMAF, Δ 1e-6 motion, 0 NaN over 500
   frames; 0 NaN and same-column parity over the 30-frame spot check). The
   Netflix CPU golden assertions are untouched — this baseline lives only in the
   fork's verification artifacts.
5. **Default the zero-copy path to direct dispatch, not the combined graph.**
   The SYCL combined graph (ADR-0483) is a net throughput loss on the VA-import
   path: its output is **byte-identical** to direct dispatch (verified by sha256)
   but the per-frame de-tile import plus the graph's compute barrier serialise
   decode→compute, costing ~15–25% at 4K (e.g. 33→42 fps on Arc A380). The
   area-threshold heuristic that selects "graph at ≥720p" was tuned for the
   host-upload path. `vmaf_sycl_select_strategy()` gains a `va_import_path` flag
   (passed `state->has_imported`) that returns DIRECT — placed *after* the env
   overrides, so an explicit `VMAF_SYCL_USE_GRAPH=1` / `VMAF_SYCL_DISPATCH=…:graph`
   still forces graph. No user-visible numeric change; users no longer need the
   deprecated `VMAF_SYCL_NO_GRAPH=1` to get good zero-copy throughput.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Fuse shift into de-tile + standalone for LINEAR/readback (chosen)** | No extra kernel launch or memory pass on the hot tiled path; keeps feature kernels identical to the CPU/host-upload path | Two code spots for the shift (fused + standalone) | Selected — correct *and* avoids the per-frame perf regression a separate pass caused |
| Standalone `launch_p010_normalize()` on every path | One code spot | A second full-plane kernel + memory pass per frame per plane, outside the replayed graph; ~15% throughput loss at 4K | Rejected — measurable regression (39→33 fps at 4K); superseded by fusion |
| Normalize inside each feature kernel | No extra submit | Touches every SYCL feature kernel; diverges them from CPU scalar reference; high bit-exactness risk against the golden gate | Rejected — multiplies surface area and parity risk |
| Auto-detect shared QSV session in code and error/repair | Saves the user a flag | libvmaf cannot see FFmpeg's `AVHWFramesContext`/`mfxSession` topology from inside the filter; would be a heuristic on surface-ID collisions, fragile | Rejected — wrong layer; documented contract is honest and robust |
| Keep `DMA_BUF_IOCTL_SYNC` as coherency hardening | Defensive flush | `SYNC_START` is a blocking fence-wait that serialises decode→compute; no correctness benefit (FIX-03 is the real fix) | Rejected — pure throughput cost, removed |
| Zero-copy defaults to direct dispatch (chosen) | ~15–25% faster at 4K; byte-identical output; no deprecated env var needed | One more `bool` param on `vmaf_sycl_select_strategy` | Selected — graph is a net loss on this path; env override still available |
| Keep graph default, document `VMAF_SYCL_USE_GRAPH=false` for zero-copy | No code change | Every zero-copy user must set an env var for good throughput; the deprecated `NO_GRAPH` is the muscle-memory knob | Rejected — the default should be the fast, correct one |
| Investigate/fix the graph slowness to keep graph on zero-copy | Retains graph launch-overhead savings | Deep Level-Zero/graph-barrier investigation; output is already byte-identical so direct loses nothing | Deferred — no upside given identical output; revisit if a future feature needs graph on this path |

## Consequences

- **Positive**: `libvmaf_sycl` zero-copy now matches the CPU oracle on
  QSV-decoded 10-bit pairs (0 NaN, VMAF parity to 3 sig figs); the P010/P012
  normalization also covers any future VA-API 10/12-bit producer, not just QSV.
- **Negative**: the correct invocation now *requires* separate
  `-init_hw_device qsv=…` per input — a single shared device silently
  reintroduces contamination. This is a documented usage constraint, surfaced in
  the backend doc and the reproducer, but not enforced by the binary.
- **Positive (perf)**: the zero-copy path now defaults to direct dispatch and is
  ~15–25% faster at 4K (33→42 fps on Arc A380) with byte-identical output; users
  no longer need the deprecated `VMAF_SYCL_NO_GRAPH=1`.
- **Neutral / follow-ups**: the MSB→LSB shift adds no measurable per-frame cost
  on the tiled hot path (fused into the de-tile store; the standalone kernel
  remains only on the rare LINEAR/readback fallbacks); `VMAF_SYCL_IMPORT_DEBUG=1`
  added as a diagnostic env knob; the host-upload path's graph default is
  unchanged; the fork's D-03 verification targets are rebased onto the
  de-contaminated oracle. The 8-bit NV12 path is unchanged (guarded no-op).

## References

- req: user directive to land the verified fix as a full PR ("Full PR-landing now").
- Original defect: the `libvmaf_sycl` zero-copy NaN bug (HANDOFF.md); supersedes
  the FIX-01 slot-toggle attempt, which was verified incomplete.
- Verification: 500-frame D-03 gate + 30-frame same-column parity on Shadow's
  Edge 4K HEVC→AV1 (Intel Arc A380); reproducer
  `.planning/phases/03-verify/verify-fix04.sh`.
- Research digest: [docs/research/2026-06-30-sycl-qsv-zerocopy-two-bug-investigation.md](../research/2026-06-30-sycl-qsv-zerocopy-two-bug-investigation.md).
