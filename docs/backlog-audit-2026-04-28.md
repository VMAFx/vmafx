<!-- markdownlint-disable MD013 MD036 MD060 -->
# Backlog audit — untracked follow-up items (2026-04-28)

> Audit of in-tree follow-up mentions (TODO / FIXME / "deferred" /
> "scaffold only" / "v2" / etc.) cross-referenced against the
> canonical backlog tracking surfaces:
> [`.workingdir2/OPEN.md`](../.workingdir2/OPEN.md),
> [`.workingdir2/BACKLOG.md`](../.workingdir2/BACKLOG.md),
> [`docs/state.md`](state.md), [`docs/rebase-notes.md`](rebase-notes.md),
> ADR Decision/Consequences blocks, and open GitHub issues / PRs.
>
> Goal: surface follow-up work that was promised in code or docs but
> never made it into a tracked backlog row, so the user can decide
> per-item whether to **promote** it (add a T-number / issue),
> **close** it (delete the stale comment), or **ignore** it (accept
> it as permanent context).

## Methodology

1. Mention corpus: case-insensitive `rg` for `TODO:`, `FIXME:`, `XXX:`,
   `HACK:`, `follow-up`, `follow up`, `followup`, `deferred`, `defer`,
   `not yet implemented`, `stub`, `scaffold`, `placeholder`, `v2`,
   `future work`, `out of scope`, `tbd`, `coming in a follow-up`,
   `next pr`, `feasibility spike`, `returns -ENOSYS`, `not yet wired`,
   `not yet live`, `Known follow-ups`, `Neutral / follow-ups` across
   all tracked files (excluding `build/` and `.workingdir2/` from
   keyword sweep, but using `.workingdir2/` for the tracked corpus).
2. Tracked corpus: read `.workingdir2/OPEN.md`,
   `.workingdir2/BACKLOG.md`, `docs/state.md`, `docs/rebase-notes.md`,
   `docs/adr/README.md`, plus `gh issue list --state open` and
   `gh pr list --state open`.
3. Cluster the raw hits by topic, drop near-duplicates, then mark
   each cluster against the tracked corpus: **Tracked** /
   **Partially tracked** / **Untracked** / **Resolved-but-stale**.

## Reproducer

```bash
# raw mention corpus (run from repo root)
rg -n -i 'TODO:|FIXME:|XXX:|HACK:|follow-up|follow up|followup|deferred|\bdefer\b|deferral|not yet implemented|stub|scaffold|placeholder|future work|out of scope|\btbd\b|coming in a follow-up|next pr|feasibility spike|returns -ENOSYS|not yet wired|not yet live|known follow-ups|neutral / follow-ups|\bv2\b' \
    --glob '!build/' --glob '!.workingdir2/'

# tracked corpus
gh issue list --state open --limit 200
gh pr list   --state open --limit 100
sed -n '1,$p' .workingdir2/{OPEN,BACKLOG}.md docs/state.md
```

## Summary

| Section | Count |
|---|---|
| Raw keyword hits | ~1 270 |
| Code-only hits (after excluding docs/ + ADRs) | 188 |
| Distinct clusters | 35 |
| **Section A — Untracked, action required** | 14 |
| **Section B — Partially tracked (in ADR text, no T-number)** | 5 |
| **Section C — Resolved-but-stale (work landed; comment lingers)** | 4 |
| Tracked clusters (out of scope for this audit) | 12 |

| Domain | Section A | Section B | Section C |
|---|---:|---:|---:|
| cuda | 1 | 1 | 0 |
| sycl | 1 | 0 | 0 |
| vulkan | 4 | 1 | 2 |
| dnn / tiny-AI | 2 | 1 | 0 |
| cli / docs | 4 | 0 | 0 |
| build | 0 | 0 | 1 |
| python (upstream) | 1 | 1 | 0 |
| simd | 1 | 1 | 1 |

---

## Section A — Untracked items needing decision

Items mentioned in code or docs as deferred / TODO / "follow-up PR"
with **no corresponding** T-number, GitHub issue, or PR pointer in
the canonical surfaces.

### A.1 — vulkan / cuda / sycl

**A.1.1 — `enable_lcs` option exposed but not implemented on GPU paths**

The `enable_lcs` config option is exposed on the GPU backends
(advertised in CLI option metadata) but the GPU kernels never use it,
and there is no T-number tracking the gap.

- [`core/src/feature/cuda/integer_ms_ssim_cuda.c:29`](../core/src/feature/cuda/integer_ms_ssim_cuda.c) —
  `implement enable_lcs (15 extra metrics) — defer to follow-up.`
- [`core/src/feature/vulkan/ms_ssim_vulkan.c:61`](../core/src/feature/vulkan/ms_ssim_vulkan.c) —
  `bool enable_lcs; /* Currently unused; kept for option parity. */`
- [`core/src/feature/vulkan/ms_ssim_vulkan.c:131`](../core/src/feature/vulkan/ms_ssim_vulkan.c) —
  `.help = "(reserved; not yet implemented in the GPU path)"`

Recommendation: either promote to a T-number under Tier 7 or
de-advertise the option on the GPU paths.

**A.1.2 — Cambi v2 GPU c-values phase (strategy III)**

ADR-0205 / Research-0020 explicitly defer the `calculate_c_values`
phase to a v2 strategy-III GPU port. The strategy is documented but
no T-number tracks the v2 phase.

- [`docs/research/0020-cambi-gpu-strategies.md:334`](research/0020-cambi-gpu-strategies.md) —
  `## Follow-up work for v2`
- [`docs/research/0020-cambi-gpu-strategies.md:372`](research/0020-cambi-gpu-strategies.md) —
  `A fully-on-GPU port is feasible via strategy III but is deferred to v2.`
- [`docs/rebase-notes.md:3327`](rebase-notes.md) — same.

Recommendation: open T7-NN once the v1 cambi GPU integration PR has
landed and bit-identical CPU↔GPU is verified.

**A.1.3 — Cambi GPU integration PR (host-glue follow-up to ADR-0205 spike)**

ADR-0205 ships the spike: shader scaffolds + dormant
`cambi_vulkan.c` + research digest. The spike file explicitly
points at an "integration PR" that wires the scaffold into
`feature_extractor.c` and replaces the `_stub` triple with the
real Vulkan-aware lifecycle. No T-number tracks the integration PR.

- [`core/src/feature/vulkan/cambi_vulkan.c:6-7`](../core/src/feature/vulkan/cambi_vulkan.c) —
  *"This file ships as an architectural reference for the cambi GPU port; it is not yet wired into the [registry]."*
- [`core/src/feature/vulkan/cambi_vulkan.c:130-131`](../core/src/feature/vulkan/cambi_vulkan.c) —
  *"The integration PR replaces this `init_stub`/`extract_stub`/`close_stub` triple with the real Vulkan-aware lifecycle."*
- [`core/src/feature/vulkan/shaders/cambi_filter_mode.comp:27`](../core/src/feature/vulkan/shaders/cambi_filter_mode.comp) —
  *"Status: scaffold only. See ADR-0205."* (and 2 more shader files identical)

Recommendation: promote to T-number; pairs naturally with the upstream
`cambi effective_eotf` port currently queued at PR #160.

**A.1.4 — Vulkan motion3 (5-frame window) on GPU path**

`integer_motion_v2`'s motion3 metric (Netflix#1486 substance) is
emitted by the CPU and the CPU paths only. The Vulkan
extractor's `provided_features[]` deliberately omits `motion3`.
T4-1 closed Netflix#1486 with a CPU-side verify-only PR; the GPU
gap was never tracked.

- [`core/src/feature/vulkan/motion_vulkan.c:23-28`](../core/src/feature/vulkan/motion_vulkan.c) —
  *"motion3_score (the 5-frame window) … `provided_features[]` deliberately omits motion3"*
- [`core/src/feature/vulkan/motion_vulkan.c:645`](../core/src/feature/vulkan/motion_vulkan.c) —
  *"DELIBERATE: motion3_score is omitted (5-frame window deferred)."*
- [`core/src/feature/vulkan/shaders/motion.comp:44`](../core/src/feature/vulkan/shaders/motion.comp) —
  *"average post-processing. Defer to a follow-up PR."*

Recommendation: T-number under Tier 3 GPU coverage; same kernel-side
gap likely exists on CUDA / SYCL — confirm before opening.

**A.1.5 — `picture_vulkan` upload path is luma-only**

PSNR Vulkan v1 emits luma-only because the upload path can't carry
chroma. Cited in three places, no T-number.

- [`core/src/feature/vulkan/psnr_vulkan.c:9-12`](../core/src/feature/vulkan/psnr_vulkan.c) —
  *"Single dispatch per channel; this v1 emits luma-only (`psnr_y`). … `picture_vulkan` upload path is luma-only today."*
- [`core/src/feature/vulkan/psnr_vulkan.c:448`](../core/src/feature/vulkan/psnr_vulkan.c) —
  *"Provided features — luma-only v1. Chroma is a focused follow-up …"*
- [`core/src/feature/cuda/integer_psnr/psnr_score.cu:22-24`](../core/src/feature/cuda/integer_psnr/psnr_score.cu) —
  same scope note (CUDA mirrors Vulkan).
- [`scripts/ci/cross_backend_vif_diff.py:74`](../scripts/ci/cross_backend_vif_diff.py) —
  *"follow-up (the picture_vulkan upload path is luma-only today)."*
- [`docs/metrics/features.md:60`](metrics/features.md) — *"Chroma support on GPU is a focused follow-up …"*

Recommendation: single T-number; un-blocks `psnr_cb`/`psnr_cr`,
chroma SSIM, etc.

### A.2 — dnn / tiny-AI

**A.2.1 — QAT trainer hook scaffold**

`ai/scripts/qat_train.py` ships a deliberate `NotImplementedError` so
the rest of the harness loads. ADR-0173 / T5-3 covers post-training
quantization; quantization-aware training is mentioned only inline.

- [`ai/scripts/qat_train.py:62-80`](../ai/scripts/qat_train.py) —
  *"QAT integration is scaffolded but not yet wired"*, raises
  `NotImplementedError("QAT trainer hook not yet wired (see message above)")`.

Recommendation: T-number under Tier 5 (governance) or close-and-delete
if QAT is intentionally deferred indefinitely (PTQ already shipping).

**A.2.2 — `build_bisect_cache.py` synthetic-fixture replacement**

The bisect-quality cache fixture is explicitly placeholder data.
`/bisect-model-quality` is wired, but a real DMOS-aligned fixture is
not tracked.

- [`ai/scripts/build_bisect_cache.py:6`](../ai/scripts/build_bisect_cache.py) —
  *"The fixture is a small synthetic placeholder for a future DMOS-aligned [fixture]."*

Recommendation: pairs with T6-1a (Netflix Public Dataset access);
either fold in or open T6-NN.

### A.3 — cli / docs

**A.3.1 — `docs/benchmarks.md` is full of `TBD` cells**

The benchmarks page ships with `TBD` in nearly every cell of the
score-vs-precision table and the per-backend score table. There is
explicit copy asking the reader to "Fill in `TBD` via a benchmark
rerun on the `ryzen-4090` profile."

- [`docs/benchmarks.md:29-47`](benchmarks.md) — 12+ `TBD` cells.
- [`docs/benchmarks.md:35`](benchmarks.md) —
  *"Fill in `TBD` via a benchmark rerun on the `ryzen-4090` profile and …"*

Recommendation: T-number to schedule one bench run + commit the
filled tables; this is a user-facing docs page and "TBD" reads
poorly.

**A.3.2 — `docs/usage/ffmpeg.md` async-overlap follow-up**

The ffmpeg page documents synchronous fence-wait import as v1; async
overlap is described as a follow-up but no T-number.

- [`docs/usage/ffmpeg.md:317`](usage/ffmpeg.md) —
  *"fence wait); async overlap is a follow-up. Same-device …"*

Recommendation: pairs with T7-29 part 2/3 (Vulkan zero-copy
implementation) — likely already implicit in T7-29's "v2 async
pending-fence" deferral. Verify before opening.

**A.3.3 — `docs/research/0017-ssimulacra2-ptlr-simd.md` SVE2 port**

- [`docs/research/0017-ssimulacra2-ptlr-simd.md:101`](research/0017-ssimulacra2-ptlr-simd.md) —
  *"SVE2 port — deferred."*
- [`docs/research/0016-ssimulacra2-iir-blur-simd.md:82`](research/0016-ssimulacra2-iir-blur-simd.md) —
  *"SVE2 port — deferred pending CI hardware."*

Recommendation: backlog row "SVE2 SIMD parity" (touches all SSIMULACRA 2
phase-1/2/3 + arm64); blocks on CI hardware.

**A.3.4 — `docs/research/0006-tinyai-ptq-accuracy-targets.md` open
questions section**

- [`docs/research/0006-tinyai-ptq-accuracy-targets.md:190-194`](research/0006-tinyai-ptq-accuracy-targets.md) —
  *"## Open questions (for follow-up iterations) … Defer to a future
  ADR once we see an actual user on a non-CPU [accelerator]."*

Recommendation: leave deferred; no action unless a user surfaces.
Flagged here so the row is not lost in the next research-digest sweep.

### A.4 — python (upstream-mirror, but pre-CLAUDE.md §12 r12)

**A.4.1 — `libvmaf.c` callback FIXMEs (predate ADR-0122 / 0123)**

ADR-0122 calls these out as predating the CUDA hardening but does
not add them to the backlog. ADR-0122's note: *"`libvmaf.c:1447`
`//^FIXME: move to picture callback` — predates [the hardening]"*.

- [`core/src/libvmaf.c:309`](../core/src/libvmaf.c) —
  `//TODO: preallocate host pics`
- [`core/src/libvmaf.c:1428,1437`](../core/src/libvmaf.c) —
  `//^FIXME: move to picture callback` (×2)
- [`core/src/libvmaf.c:1957`](../core/src/libvmaf.c) —
  `//TODO: dedupe, vmaf_bootstrap_predict_score_at_index()`
- [`core/src/predict.c:473`](../core/src/predict.c) —
  `//TODO: dedupe, vmaf_score_pooled_model_collection()`

These are upstream-Netflix TODOs and would normally be exempt under
the rebase-fidelity carve-out. **However**, CLAUDE.md §12 r12
("touched files lint-clean to the strictest profile") implies
sweep-ability is on the table for the next session that touches the
file. Recommendation: wait for natural touch — do not open a
backlog row.

---

## Section B — Partially tracked (in ADR / digest text, no T-number / issue / PR)

Items mentioned in an ADR or research digest with a clear scope but
**without** a backlog row pointing back. These are different from
Section A: an audit trail exists, but the backlog has no execution
hook.

> **Triage status (2026-04-28):** all five items have been evaluated
> against the canonical backlog (`.workingdir2/BACKLOG.md`):
> B.1 confirmed already-tracked transitively (T3-9 → closed as T7-21
> "AVX2 ceiling" per ADR-0180); B.2–B.5 promoted to T7-31..T7-34
> respectively. No further row-promotion action required.

### B.1 — `psnr_hvs` AVX-512 follow-up footnote (ADR-0160)

Cited in [`docs/rebase-notes.md:2783`](rebase-notes.md) and ADR-0160.
The matrix entry **was** opened as **T3-9** (`psnr_hvs` AVX-512 port)
and then **closed as T7-21** ("AVX2 ceiling" verdict, ADR-0180). The
audit trail is consistent — listing here as confirmation that ADR
text → T-number linkage is complete for this item, not as an
untracked item. **Move to "tracked".**

### B.2 — `iqa_convolve` AVX-512 ADR-0138 follow-up

- [`core/src/feature/x86/convolve_avx512.h:37`](../core/src/feature/x86/convolve_avx512.h) —
  *"See docs/adr/0138-iqa-convolve-avx2-bitexact-double.md §Follow-up."*

The cited "§Follow-up" section in ADR-0138 promises an AVX-512
follow-up to the convolve kernel; rebase-notes 0035 mentions it
peripherally. No T-number.

Recommendation: T-number under Tier 3 (SIMD gap-fill).

**→ promoted as T7-31 (2026-04-28).**

### B.3 — `motion_v2` AVX2 srlv_epi64 negative-diff audit (rebase-notes 0038)

[`docs/rebase-notes.md:2049-2051`](rebase-notes.md) carries an
explicit *"Follow-up T-N"* placeholder with no number assigned:
*"audit the fork's AVX2 `motion_v2` variant against scalar on a
negative-diff corpus."*

Recommendation: pick up the placeholder and assign T7-NN (low effort,
single bench run).

**→ promoted as T7-32 (2026-04-28).**

### B.4 — Tiny-AI `tiny-vmaf-v2` prototype Pearson-drop budget (Research-0006)

[`docs/research/0006-tinyai-ptq-accuracy-targets.md:120`](research/0006-tinyai-ptq-accuracy-targets.md)
references "the `tiny-vmaf-v2` prototype" as the largest tiny-AI
model for budget purposes. Whether that name maps to a concrete
checkpoint that ships under `model/tiny/` is unclear. ADR-0173 /
ADR-0174 cover `learned_filter_v1` and `nr_metric_v1`; "v2" of
"tiny-vmaf" is not identified.

Recommendation: clarify in the research digest or open a B-row to
ratify the model identity.

**→ promoted as T7-33 (2026-04-28).**

### B.5 — Python (`python/vmaf/routine.py`) `feature_option_dict` FIXME

- [`python/vmaf/routine.py:937,1109`](../python/vmaf/routine.py) —
  *"FIXME: as set to None, potential bug with inconsistent behavior
  with VmafQualityRunner"* (×2).

Upstream-Netflix code, but flagged as a *potential bug*. Not in
state.md / BACKLOG.md. Likely benign (the upstream caller passes
`None` deliberately) — but the comment specifically says "potential
bug".

Recommendation: open a verify-only row under Tier 1 (correctness
sweep) that confirms whether the inconsistency reproduces. Closes
either by adding a regression test or by deleting the FIXME.

**→ promoted as T7-34 (2026-04-28).**

---

## Section C — Resolved-but-stale (the comment lingers; the work landed)

The TODO / "scaffold" / "follow-up" note is still in source, but the
referenced work was completed. Comment-only fix.

> **Status (2026-04-28):** all four C-items cleared via PR #164
> (`chore/section-c-stale-comments`). Listed below for the audit
> trail; no further action.

### C.1 — `libvmaf_vulkan.h:141-142` says "scaffold only (T7-29 part 1)"

T7-29 parts 2 + 3 landed via ADR-0186 (Vulkan VkImage zero-copy
import implementation + `libvmaf_vulkan` FFmpeg filter,
`ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`). The
header still advertises the entry points as `-ENOSYS` stubs.

- [`core/include/libvmaf/libvmaf_vulkan.h:141-142`](../core/include/libvmaf/libvmaf_vulkan.h) —
  *"Status: scaffold only (T7-29 part 1). Every function returns
  -ENOSYS pending the real implementation"*

Action: update the doc comment to reflect ADR-0186 status (or
remove the note entirely).

### C.2 — `libvmaf_vulkan.h:14-22` "scaffolded by ADR-0175 / T5-1"

T5-1b (full runtime) landed (ADR-0178); kernel matrix is now full
(VIF + ADM + motion + motion_v2 + ssimulacra2 + cambi (spike) + the
GPU-long-tail batches landing in PR #122–#159). The header still
says *"every entry point currently returns -ENOSYS unconditionally."*

- [`core/include/libvmaf/libvmaf_vulkan.h:14-22`](../core/include/libvmaf/libvmaf_vulkan.h) —
  *"**Status: scaffold only.** Every entry point currently returns
  -ENOSYS [pending the kernels]."*

Action: rewrite header doc-comment.

### C.3 — `core/src/feature/ssimulacra2.c:38` "AVX2/AVX-512/NEON SIMD variants are follow-up PRs"

T3-1 (AVX2) → ADR-0161, T3-2 (AVX-512+NEON) → ADR-0162, T3-3
(snapshot gate) → ADR-0164 all landed 2026-04-25.

- [`core/src/feature/ssimulacra2.c:38`](../core/src/feature/ssimulacra2.c) —
  *"Scalar-only for now; AVX2/AVX-512/NEON SIMD variants are
  follow-up PRs."*

Action: update line 38 to state the SIMD paths ship under
ADR-0161 / 0162 / 0163.

### C.4 — `core/src/meson.build:47-59` "Vulkan compute backend, scaffold-only"

Same Vulkan-runtime delta as C.1 / C.2: meson option blurb still
calls Vulkan a scaffold-only backend. ADR-0175 (scaffold) was
followed by ADR-0178 (T5-1b runtime) and ADR-0193 (kernel matrix
complete for the default model).

- [`core/src/meson.build:47`](../core/src/meson.build) —
  `# ADR-0175 / T5-1: Vulkan compute backend, scaffold-only for now.`
- [`core/src/meson.build:59`](../core/src/meson.build) —
  same.
- [`core/meson_options.txt:69`](../core/meson_options.txt) —
  `description: 'Build Vulkan compute backend (scaffold only; ADR-0127 / ADR-0175). … flip to enabled when the kernels land.'`

Action: rewrite the three blurbs to reflect post-T5-1b reality.

---

## Heuristic limitations & known false-positives

- **`v2` keyword** matches genuine versioning (`vmaf_feature_v2.py`,
  `motion_v2`, `Level Zero v2`, libvmaf release tags `v2.x`, AOM
  CTC sets `v2.0`, etc.) far more often than it matches deferred
  work. The audit deliberately drops those — only kept "v2" when
  paired with "deferred" or "follow-up" in the same comment.
- **`stub` / `scaffold`** keywords appear extensively in the
  intentionally-permanent stub-build path of `core/src/dnn/`
  (real-ORT vs disabled-ORT branch). All such hits were dropped as
  not-deferred-work.
- **ADR `Neutral / follow-ups` blocks** are template-driven and
  often contain "no action; this is documented elsewhere" rather
  than untracked work. Only flagged when the prose names a
  concrete deliverable.
- **Upstream-Netflix TODOs** (in `core/src/feature/iqa/`,
  `python/vmaf/matlab/strred/`, `python/vmaf/core/`) were dropped
  unless the comment described a fork-affecting issue. The fork
  preserves these for rebase fidelity per CLAUDE.md §12 r7.
- **Closed PR references** (e.g., `branches v2`, `simd-v2`
  workstream branches in rebase-notes.md) all pointed to PRs that
  were merged 2026-04-22 or earlier; not flagged.
- The audit doc path-pinning uses the line numbers as of
  `origin/master` at audit time. Subsequent edits will drift line
  numbers; the path + context phrase are the load-bearing
  references.

---

## Recommended next steps

1. Promote Section A items to **T-numbered backlog rows** in
   `.workingdir2/BACKLOG.md` (one row each; group A.1.1–A.1.5 under
   a new "GPU coverage long-tail" sub-tier if desired). Keep A.3.4
   and A.4.1 on the watch list (no row yet). **Open** — needs
   per-item user decision.
2. For **Section B**, stop short of opening rows for B.1 (already
   tracked transitively) and open rows for B.2–B.5 each. **Done
   2026-04-28** — B.2 → T7-31, B.3 → T7-32, B.4 → T7-33,
   B.5 → T7-34.
3. For **Section C**, sweep the four files in a single
   chore PR — comment-only, no behaviour change. Pairs naturally
   with the next non-trivial PR that touches each file. **Done
   2026-04-28** — PR #164 (`chore/section-c-stale-comments`).
