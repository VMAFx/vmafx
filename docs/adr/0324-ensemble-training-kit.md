<!-- markdownlint-disable MD060 -->
# ADR-0324: Ensemble training kit — portable Phase-A + LOSO retrain bundle

- **Status**: Accepted
- **Date**: 2026-05-06
- **Deciders**: Lusoris, Claude
- **Tags**: ai, fr-regressor, ensemble, tooling, fork-local

## Context

Lawrence (collaborator with different hardware than the lead user) has
volunteered to run a `fr_regressor_v2_ensemble_v1` retrain against his
own copy of the Netflix Public Dataset to provide a second
PROMOTE/HOLD verdict — independent confirmation of the gate result the
lead user emitted in PR #422 (real-corpus LOSO mean PLCC 0.997, spread
0.001) and PR #424 (full-corpus ONNX export + production flip).

The plumbing already exists in tree:

- `scripts/dev/hw_encoder_corpus.py` — the Phase-A producer.
- `ai/scripts/run_ensemble_v2_real_corpus_loso.sh` — the LOSO wrapper.
- `ai/scripts/train_fr_regressor_v2_ensemble_loso.py` — the trainer.
- `ai/scripts/validate_ensemble_seeds.py` — the gate verdict emitter.
- `ai/scripts/export_ensemble_v2_seeds.py` — the per-seed ONNX exporter
  (full-corpus fit; LOSO trainer discards models after computing PLCC).
- `docs/ai/ensemble-v2-real-corpus-retrain-runbook.md` — the operator
  doc.

But the runbook still requires the operator to chain ten files and
five env-var conventions by hand. The same set of scripts lives at
five different paths under three different roots
(`scripts/dev/`, `ai/scripts/`, `scripts/ci/`). Asking a collaborator
to clone the whole vmaf fork (including `.workingdir2/netflix/*.yuv`
gitignored — they don't exist on his disk) and read the runbook front-
to-back to figure out invocation order is hostile to the goal of
"send him a tarball, he runs one command, he sends back a verdict".

## Decision

We will ship a self-contained kit at `tools/ensemble-training-kit/`
with:

- A one-command orchestrator (`run-full-pipeline.sh`) that chains
  prereqs → corpus generation → LOSO training → verdict → bundling
  with sane error handling and `--ref-dir` / `--encoders` / `--cqs` /
  `--out-dir` flags.
- Five numbered step scripts (`01-prereqs.sh` … `05-bundle-results.sh`)
  the operator can invoke individually for retries.
- An operator-facing `README.md` with the prereq table, troubleshooting
  matrix, and the verdict-file shape.
- A `make-distribution-tarball.sh` that bundles the kit + every
  in-repo script it invokes + the runbook into a portable tar.gz the
  collaborator can untar without cloning the fork.
- Pinned Python deps (`pyproject.toml` + `requirements-frozen.txt`).

The bundling on PROMOTE re-uses the existing
`ai/scripts/export_ensemble_v2_seeds.py` (which already fits the
final-production weights on the FULL corpus and writes ONNX + sidecar
artefacts) — no changes to `_train_one_seed` are required.

### Status update 2026-05-06: kit extended for multi-platform

The original kit assumed an NVIDIA-only Linux box (lead user's
hardware). Lawrence's collaborator workstation has four boxes that
need to participate: an NVIDIA CUDA card, an Intel Arc 310, an Intel
iGPU, and a Mac. The kit now supports four host families:

- `linux-x86_64-cuda` — `h264_nvenc,hevc_nvenc,av1_nvenc` (existing)
- `linux-x86_64-sycl` — `h264_qsv,hevc_qsv,av1_qsv` (Intel Arc / iGPU)
- `linux-x86_64-vulkan` — `libx264` CPU baseline (no GPU detected)
- `darwin-{arm64,x86_64}-cpu` — `h264_videotoolbox,hevc_videotoolbox`

A new `_platform_detect.sh` helper auto-defaults the encoder list per
box; `01-prereqs.sh` skips NVIDIA-specific gates on non-CUDA platforms
and probes ffmpeg-VideoToolbox availability on Darwin instead. A new
`build-libvmaf-binaries.sh` script lets each operator build a libvmaf
binary for their box once and rsync it into `binaries/<platform>/`;
binaries are not bundled in source control. The
`scripts/dev/hw_encoder_corpus.py` encoder vocabulary now covers
`{h264,hevc}_videotoolbox` with the canonical adapter's argv shape
(`-c:v <name> -q:v <quality> -realtime 0`) verified against
`tools/vmaf-tune/src/vmaftune/codec_adapters/_videotoolbox_common.py`.

Each box generates its own canonical-6 corpus shard; the lead user
merges shards via `ai/scripts/merge_corpora.py` (de-duplicates by
`src_sha256`) before the cross-platform LOSO retrain.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| In-tree kit at `tools/ensemble-training-kit/` (chosen) | Single place to look; ships in PRs alongside the trainer; lints / changelog / ADR entry land together; tarball generator produces a self-contained bundle for collaborators with no checkout. | Adds ~500 LOC of bash + markdown to the tree. | — |
| Git submodule pointing at a separate `VMAFx/vmafx-ensemble-kit` repo | Cleanly separates the operator-facing surface from the engine. | Submodule overhead (extra clone step, separate CI, version-pin drift). The kit's scripts must stay in lockstep with `ai/scripts/` — a separate repo guarantees drift when the trainer's argv changes (already happened twice: PR #422 dropped `--corpus-root`, PR #424 added `--out-dir`). | Drift risk dwarfs the cleanliness benefit. |
| Docker image bundling the toolchain | One-step setup; reproducible Python + CUDA env. | NVENC inside a container needs `--device /dev/nvidia*` + matching driver versions; libvmaf-CUDA build inside the image costs 10+ minutes; image is multi-GiB to ship. The CLAUDE.md constraint explicitly states "Docker is NOT a requirement". | Higher friction than a tarball + prereq check. |

## Consequences

- **Positive**: collaborators can run the retrain in one command after
  unpacking a < 50 MiB tarball. The kit lives next to the trainer, so
  CLI drift between LOSO wrapper / exporter / kit fails CI within a
  single PR rather than silently. The README's troubleshooting matrix
  doubles as an FAQ for future operators.
- **Negative**: ~500 LOC of bash + markdown to maintain. If the LOSO
  wrapper changes argv conventions, both the wrapper and the kit's
  pass-through must update.
- **Neutral / follow-ups**: The kit assumes seeds 0..4 because the LOSO
  wrapper hard-codes them; if a future PR parameterises the wrapper's
  seed list, the kit's `--seeds` flag (currently advisory) becomes
  authoritative.

## References

- [`docs/ai/ensemble-v2-real-corpus-retrain-runbook.md`](../ai/ensemble-v2-real-corpus-retrain-runbook.md)
  — the operator doc this kit extends.
- [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md) — the gate
  this kit's verdict step applies.
- [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md) —
  the real-corpus retrain workflow this kit packages.
- [ADR-0319](0319-ensemble-loso-trainer-real-impl.md) — LOSO trainer
  real impl + the wrapper's argv contract.
- [ADR-0321](0321-fr-regressor-v2-ensemble-full-prod-flip.md) — the
  full-corpus ONNX export script the bundle step invokes on PROMOTE.
- Source: `req` — direct user request to ship a portable kit so a
  collaborator on different hardware can run the retrain end-to-end.

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `tools/ensemble-training-kit/` — present, with
  `run-full-pipeline.sh`, the five numbered step scripts
  (`01-prereqs.sh` through `05-bundle-results.sh`),
  `make-distribution-tarball.sh`, `_platform_detect.sh`,
  `build-libvmaf-binaries.sh`, `extract-corpus.sh`, and a
  `binaries/` per-platform staging directory.
- The multi-platform extension (Linux CUDA / Linux SYCL / Linux
  Vulkan-baseline / Darwin CPU) was already documented in the
  original ADR's `### Status update 2026-05-06` block; nothing
  to re-verify here beyond the four families remaining live.
- Verification command:
  `ls tools/ensemble-training-kit/`.
