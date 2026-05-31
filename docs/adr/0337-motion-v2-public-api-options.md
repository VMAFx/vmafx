# ADR-0337: motion_v2 inherits motion v1's public option surface (duplicate registration)

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude (Opus 4.7)
- **Tags**: upstream-port, motion, feature-extractor, cli, public-api, fork-local

## Context

Netflix/vmaf upstream landed four motion_v2 commits between
2026-05-07 and 2026-05-08:

| SHA | Subject | Touches |
| --- | --- | --- |
| `856d3835` | fix mirroring behaviour | scalar + AVX2 + AVX-512 |
| `c17dd898` | add `motion_max_val` | `integer_motion_v2.c` |
| `a2b59b77` | add `motion_five_frame_window` | `integer_motion_v2.c`, `feature_extractor.h`, `libvmaf.c`, `tools/vmaf.c` |
| `4e469601` | port remaining options | `integer_motion_v2.c` |

Together they extend `motion_v2`'s public option surface to match
the existing motion v1 surface (`motion_force_zero`,
`motion_blend_factor`, `motion_blend_offset`, `motion_fps_weight`,
`motion_max_val`, `motion_five_frame_window`,
`motion_moving_average`) and add a third per-frame feature
(`VMAF_integer_feature_motion3_v2_score`).

PR #460 (the fork-side cluster port, still draft) and PR #453
(an earlier, narrower attempt) **both deferred `a2b59b77` +
`4e469601`** with the rationale that the option surface "would
duplicate against motion v1, which already exposes the same knobs
via [ADR-0158](0158-netflix-1486-motion-updates-verified-present.md)."
The deferral note in each PR called for an architectural ADR before
either commit lands, because once the duplication ships it is
behaviourally observable: a single VMAF model can name `motion_v2`
**and** `motion` in the same feature list, and a downstream model
parser must decide whether the same option string (e.g.
`motion_blend_factor=0.5`) targets one extractor, the other, or
both.

A second concern is the `motion_five_frame_window=true` mode in
`a2b59b77`. Upstream wires it through a new
`fex->prev_prev_ref` field on `VmafFeatureExtractor`, plus
matching picture-pool sizing in `vmaf_read_pictures` (n‑threads × 2

- 2). The fork's `read_pictures_*` decomposition (ADR-0152
monotonic-index gate) and existing `dnn`-block additions to
`VmafContext` diverge from upstream's layout; porting the
`prev_prev_ref` plumbing surfaces a four-conflict-region merge in
`core/src/libvmaf.c` plus one in `core/tools/vmaf.c`. The
3-frame default mode (`motion_five_frame_window=false`) does not
need that plumbing — it touches only `prev_ref`, which the fork
already provides.

NASA/JPL Power-of-10 rule 6 (declared scope) and CERT C "fail
loud, fail early" both argue for accepting the 3-frame option
surface and rejecting `motion_five_frame_window=true` with
`-ENOTSUP` at `init()` until the picture-pool refactor lands as
its own PR. This mirrors the precedent set in
[ADR-0219](0219-motion3-gpu-coverage.md) §Decision for the GPU
motion3 extractors.

## Decision

We will **register the full motion v1 option surface a second time
on `motion_v2`**, as separate `VmafOption[]` entries owned by
`motion_v2`'s `MotionV2State`. v1 and v2 remain independent
extractors with independent option tables; the duplication is
deliberate and documented. `motion_v2`'s option help strings and
aliases match upstream `4e469601` byte-for-byte so future
`/sync-upstream` runs find no behavioural delta.

`motion_five_frame_window=true` is rejected at `init()` with
`-ENOTSUP` and a log message pointing at this ADR. The 3-frame
default mode (the only mode any shipped VMAF model uses) is fully
supported and bit-exact against the CPU motion v1 emission for
`motion3_v2_score` (modulo the `motion_v2`-specific pipelined SAD
that already meets `places=4` against v1 in the existing snapshot
gate).

Picture-pool plumbing (`prev_prev_ref` field, `n_threads * 2 + 2`
sizing, `vmaf_read_pictures` 2-frame ring) is **deferred** to a
follow-up PR. That PR will flip the `-ENOTSUP` guard to a
`prev_prev_ref` lookup and add a Netflix-golden five-frame fixture
once the picture-pool refactor is reviewed in isolation.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **A1 — Duplicate option surfaces (chosen)** | v1 and v2 stay structurally independent; no cross-extractor coupling; matches upstream byte-for-byte; docs duplicate per-extractor (which every backend variant already does); bit-exactness against Netflix golden trivially preserved (v1 untouched) | Option-table source duplicates (~80 LOC of `VmafOption[]` rows + 7 struct fields); two `motion_blend_factor` help strings live in two `.c` files | Best ratio of correctness to risk; duplication cost is purely textual and the fork already accepts it for `motion` vs `motion_cuda` vs `motion_sycl` vs `motion_vulkan` |
| A2 — Shared option-parser table | Zero duplication; single help-string source | Couples v1 and v2 internals; introduces a new shared header (`motion_options.h`) that has to thread `offsetof()` calls across two struct definitions; option parsing is *already* centralised by the framework's `VmafOption[]` plumbing — only the table content duplicates, which is the cheap part | Over-engineering; the duplication objection isn't load-bearing once the framework's existing parser is examined |
| A3 — v2 deprecates v1 | Cleanest long-term; eliminates the legacy 3-frame blur ring buffer | Breaks the user-visible CLI (`--feature motion` resolves to v1 today); breaks every shipped VMAF model that names `motion`; triggers a CLAUDE.md §1 / [ADR-0024](0024-netflix-golden-preserved.md) golden-data discussion (the goldens are CPU-only and pin against v1's exact output); separate ADR scope | Out of budget for this PR; would need its own ADR + a `--legacy-motion` migration path; defer indefinitely |
| A4 — Defer everything until upstream coverage gate | Zero risk | Fork falls behind upstream on a publicly-discoverable CLI surface; future model files referencing `motion_v2=motion_blend_factor=…` would error with "unknown option"; PR #460 has been draft for 1 day and #453 longer | Pure procrastination |

## Consequences

- **Positive**:
  - `motion_v2` accepts the same option strings as v1, so downstream
    VMAF models or `/sync-upstream` runs that pull upstream model
    files do not error on unknown options.
  - The four-commit upstream cluster lands as a coherent set; future
    `/sync-upstream` audits will not flag any of `856d3835`,
    `c17dd898`, `a2b59b77`, or `4e469601` as pending ports.
  - `motion3_v2_score` is now emitted, closing a coverage gap
    against motion v1's `motion3_score`.
  - The `-ENOTSUP` guard on `motion_five_frame_window=true` mirrors
    [ADR-0219](0219-motion3-gpu-coverage.md) §Decision for the GPU
    twins — every `motion_v2*` consumer (CPU, CUDA, SYCL, HIP,
    Vulkan) now reports the same error for the same input, which
    is easier to reason about than per-backend silent fallbacks.

- **Negative**:
  - ~80 lines of `VmafOption[]` duplicate between
    `integer_motion.c` and `integer_motion_v2.c`. Touching the
    help string for one extractor requires touching it for the
    other; ADR-0141 (touched-file lint-clean) catches drift on the
    next edit.
  - `motion_five_frame_window=true` returns `-ENOTSUP` where the
    user might expect a fall-back to 3-frame mode. This is
    deliberate (CERT C fail-loud) and matches ADR-0219's GPU
    precedent.

- **Neutral / follow-ups**:
  - Picture-pool refactor (`prev_prev_ref` + `n_threads * 2 + 2`
    sizing) tracked as a separate follow-up. The follow-up PR
    flips the `-ENOTSUP` guard and adds a five-frame-window
    fixture once the picture-pool refactor passes review in
    isolation.
  - GPU twins (CUDA, SYCL, HIP, Vulkan) of `motion_v2` do **not**
    need the option surface in this PR. The 3-frame post-process
    is host-side scalar (per ADR-0219); whether GPU twins gain the
    same options will be decided when each twin needs to emit
    `motion3_v2_score`. Until then GPU twins keep their existing
    options table.
  - Netflix-golden gate (CPU, places=4, ADR-0024) is unaffected:
    motion v1 is untouched and the goldens pin v1's output. The
    `motion_v2` snapshot under `testdata/scores_cpu_motion_v2_*.json`
    is not regenerated by this PR — the `MIN(score,
    motion_max_val)` clip and motion3 emission are additive at the
    default option values, and existing snapshots were taken
    against scores well below the 10000.0 default cap.

## References

- Upstream commits:
  [`856d3835`](https://github.com/Netflix/vmaf/commit/856d3835),
  [`c17dd898`](https://github.com/Netflix/vmaf/commit/c17dd898),
  [`a2b59b77`](https://github.com/Netflix/vmaf/commit/a2b59b77),
  [`4e469601`](https://github.com/Netflix/vmaf/commit/4e469601).
- Fork PRs preceding this one: PR #453 (narrow attempt; mirror fix
  citation backfill; deferred a2b59b77 + 4e469601), PR #460
  (cluster port; deferred via PR-body §1 "Public-API surface
  change in `4e469601` was NOT ported"). Both surfaced the
  architectural question this ADR resolves.
- Sister ADRs:
  - [ADR-0158](0158-netflix-1486-motion-updates-verified-present.md)
    — motion v1 option surface (the duplicated source of truth).
  - [ADR-0219](0219-motion3-gpu-coverage.md) — GPU motion3
    `-ENOTSUP` precedent for `motion_five_frame_window=true`.
  - [ADR-0145](0145-motion-v2-neon-bitexact.md) — fork-local
    NEON `motion_v2` twin (mirror fix propagates to it).
  - [ADR-0152](0152-vmaf-read-pictures-monotonic-index.md) —
    fork-local `read_pictures` decomposition that drives the
    picture-pool deferral.
  - [ADR-0024](0024-netflix-golden-preserved.md) — Netflix
    golden gate.
- Source: `req` — agent brief 2026-05-09 (PR #460 / #453
  follow-up): "ADR is needed before the option surface duplicates
  between v1 and v2."
