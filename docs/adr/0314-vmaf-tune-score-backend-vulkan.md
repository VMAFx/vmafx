<!-- markdownlint-disable MD060 -->
# ADR-0314: vmaf-tune `--score-backend=vulkan` (vendor-neutral GPU scoring)

- **Status**: Accepted
- **Date**: 2026-05-05
- **Deciders**: lusoris, lawrence (raised the non-NVIDIA scoring question)
- **Tags**: tooling, vmaf-tune, vulkan, gpu, fork-local

## Context

`vmaf-tune` ([ADR-0237](0237-quality-aware-encode-automation.md))
already plumbs a `--score-backend` selector through to the libvmaf CLI
([ADR-0299](0299-vmaf-tune-gpu-score.md)). The harness ships with
`auto`, `cpu`, and `cuda` paths exercised end-to-end; the `vulkan` and
`sycl` values were declared in `score_backend.ALL_BACKENDS` but never
wired through `score.build_vmaf_command` / `corpus.CorpusOptions` /
the `cli.py` argparse surface — a regression introduced when
post-#378 PRs rebased over the GPU-score branch and dropped its
`cli.py` hunks (the lost wiring is documented in #382's diff against
HEAD).

The cost shows up on non-NVIDIA developer boxes. AMD RDNA3, Intel Arc
Alchemist/Battlemage, and any Mesa-anv/RADV/lavapipe host has no
`nvidia-smi` and no CUDA toolkit; the CUDA backend is unreachable on
those platforms by construction. The libvmaf CLI's Vulkan backend
([ADR-0127](0127-vulkan-compute-backend.md) /
[ADR-0175](0175-vulkan-backend-scaffold.md) /
[ADR-0186](0186-vulkan-image-import-impl.md)) is the vendor-neutral
answer — it runs against any conformant Vulkan 1.2 driver — but
`vmaf-tune` users had to drop down to a hand-rolled `vmaf` invocation
to use it. The score-axis floor stayed at CPU's ~1–2 fps on 1080p,
which is the bottleneck PR #378 set out to remove.

## Decision

Restore the lost `--score-backend` argparse wiring and admit `vulkan`
(alongside `cpu` / `cuda` / `sycl`) as a strict-mode value. No new
fallback semantics, no new probe heuristics — the existing
`score_backend.select_backend` and `_probe_vulkan` helpers already
handle Vulkan correctly; we only re-attach the seam.

Concretely:

1. `score.build_vmaf_command` accepts a new `backend: str | None`
   kwarg; when set it appends `--backend $name` to the libvmaf argv.
2. `score.run_score` forwards the same kwarg.
3. `corpus.CorpusOptions` gains a `score_backend: str | None` field;
   `corpus.iter_rows` passes it into `run_score`.
4. `cli.py` registers `--score-backend {auto,cpu,cuda,sycl,vulkan}`
   on both the `corpus` and `recommend` subparsers and resolves it to
   a concrete value via `select_backend(prefer=…)` before any
   encode is dispatched. `BackendUnavailableError` becomes a clean
   exit-2 with the diagnostic the helper produces.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Extend argparse choices to include `vulkan` (chosen) | One-line argparse change; reuses existing `score_backend.py` probe infrastructure; matches libvmaf CLI vocabulary 1:1; consistent with `cuda` / `sycl` precedent. | Inherits the existing strict-mode UX for non-auto values (no silent CPU downgrade) — operators without a Vulkan host must opt into `auto` or pin `cpu`. | — chosen. |
| Ship a SDR-only Vulkan flag (e.g. `--vulkan-sdr`) | Avoids exposing all three GPU backends symmetrically; smaller decision surface. | Asymmetric with the existing `--score-backend cuda` pattern; users would need to learn a second flag for the vendor-neutral path; libvmaf already gates HDR support via the `--backend` selector itself, so a SDR-only flag would lie about the binary's capability. | Rejected — symmetry beats specialness. |
| Defer to direct libvmaf invocation (no change) | Zero code; vmaf-tune stays small. | Locks AMD / Intel Arc / MoltenVK users out of the harness. Breaks the Phase A → Phase B → Phase C corpus pipeline for non-NVIDIA developer boxes. Regresses the user's ADR-0299 commitment to "the fastest available backend" on the majority of contributor hardware. | Rejected — the harness exists to insulate users from the hand-rolled CLI. |

## Consequences

- **Positive**: AMD, Intel Arc, and MoltenVK hosts can drive
  `vmaf-tune corpus` end-to-end on a GPU. `auto` mode keeps walking
  `cuda → vulkan → sycl → cpu`, so existing NVIDIA boxes see no
  behaviour change. Restoring the lost wiring also re-enables the
  pre-existing `--score-backend cuda` happy path that was silently
  broken between PR #378 and HEAD.
- **Negative**: Strict-mode failures on non-Vulkan hosts now surface
  as `BackendUnavailableError` exit-2 instead of the previous "flag
  ignored, ran on CPU" silent downgrade. This is intentional — the
  ADR-0299 strict guarantee is load-bearing for operator wall-clock
  expectations — but it means CI lanes that pin `--score-backend
  vulkan` need a Vulkan-capable runner (lavapipe is sufficient).
- **Neutral / follow-ups**: No new ADR-0214 cross-backend parity work
  required; ADR-0214 already covers Vulkan parity to `places=4` from
  the libvmaf side. The `tools/vmaf-tune/AGENTS.md` invariant note is
  extended to call out that argparse choices and
  `score_backend.ALL_BACKENDS` must stay in sync with libvmaf's
  `--backend NAME` vocabulary.

## References

- Parent: [ADR-0299](0299-vmaf-tune-gpu-score.md) — original
  `--score-backend` wiring (CUDA happy path).
- Backend scaffold: [ADR-0127](0127-vulkan-compute-backend.md),
  [ADR-0175](0175-vulkan-backend-scaffold.md),
  [ADR-0186](0186-vulkan-image-import-impl.md).
- Cross-backend gate: [ADR-0214](0214-gpu-parity-ci-gate.md).
- Source: per-user direction (`req`, 2026-05-05) — lawrence asked
  about non-NVIDIA GPU VMAF scoring; the Vulkan backend is the
  vendor-neutral answer.

### Status update 2026-05-08: Accepted (resolves merge-conflict markers)

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

The 2026-05-06 bulk Status flip (`changelog.d/changed/adr-bulk-status-flip-2026-05-06.md`)
flipped this ADR to Accepted. A subsequent rebase reintroduced
unresolved Git conflict markers
(`<<<<<<< HEAD` / `=======` / `>>>>>>> 599bb187`) around the
front-matter Status line, leaving HEAD `0a8b539e` carrying both
candidate values. This audit appendix resolves the markers in
favour of Accepted; the implementation has been verified live in
tree.

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `tools/vmaf-tune/src/vmaftune/cli.py` admits
  `--score-backend {auto,cpu,cuda,sycl,vulkan}` on both `corpus`
  and `recommend` subparsers (line 114 onward; help text and
  resolution via `select_backend(prefer=…)` at line 594).
- `score.build_vmaf_command` accepts the backend kwarg;
  `corpus.CorpusOptions` carries `score_backend`.
- `auto` mode walks `cuda → vulkan → sycl → cpu`; strict-mode
  failures raise `BackendUnavailableError` and exit 2 with the
  helper diagnostic — no silent CPU downgrade.
- Verification command:
  `grep -nE "vulkan" tools/vmaf-tune/src/vmaftune/cli.py | head`.
