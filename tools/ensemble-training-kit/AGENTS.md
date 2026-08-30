# AGENTS.md — tools/ensemble-training-kit

Orientation for agents working on the portable Phase-A + LOSO retrain
training kit. Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

A self-contained shell-script kit that produces an FR-regressor v2
ensemble end-to-end on a single host:

```text
tools/ensemble-training-kit/
  01-prereqs.sh                    # gate: toolchain + GPU memory probe
  02-generate-corpus.sh            # Phase-A canonical-6 corpus generation (CUDA / SYCL / CPU per platform)
  03-train-loso.sh                 # 5-seed × 9-fold LOSO retrain
  04-validate.sh                   # ADR-0303 verdict emission
  05-bundle-results.sh             # tar.gz the trained ensemble for distribution
  run-full-pipeline.sh             # one-command orchestrator (chains 01..05)
  build-libvmaf-binaries.sh        # build kit-bundled libvmaf binaries per platform
  extract-corpus.sh                # corpus-archive helper for offline operators
  make-distribution-tarball.sh     # ship-the-kit-as-tarball helper
  prepare-gdrive-bundle.sh         # gdrive-bundler entry point (T6-7c)
  _platform_detect.sh              # shared platform-detection helpers (sourced by 01/02/run-full)
  binaries/<platform>/vmaf         # kit-bundled libvmaf binary (built locally via build-libvmaf-binaries.sh)
  pyproject.toml                   # Python deps for the training side (loaded by 03-train-loso.sh)
  requirements-frozen.txt          # frozen Python dep set
  README.md                        # operator-facing runbook
  tests/test_platform_detect.sh    # shellcheck + smoke for _platform_detect.sh
```

The kit is **fork-original** — there is no upstream Netflix/vmaf
equivalent. It bundles existing in-tree pieces unchanged
(`scripts/dev/hw_encoder_corpus.py`,
`ai/scripts/run_ensemble_v2_real_corpus_loso.sh`,
`ai/scripts/validate_ensemble_seeds.py`,
`ai/scripts/export_ensemble_v2_seeds.py`,
`scripts/ci/ensemble_prod_gate.py`). No engine changes; this is a
distribution + orchestration surface.

## Ground rules

- **Parent rules** apply (see [../../AGENTS.md](../../AGENTS.md)).
- **All kit shell scripts ship the dual Lusoris/Claude (Anthropic)
  copyright header** per [ADR-0025](../../../docs/adr/0025-copyright-handling-dual-notice.md).
- **`set -euo pipefail` at the top of every script.** Pipes carry
  errors; unset variables are fatal. The kit aims for one-fault
  visibility — operators should see one problem at a time, not a
  cascade.
- **Numbered-step contract**: `01-` through `05-` are usable
  individually for retries. `run-full-pipeline.sh` orchestrates them
  end-to-end. Renumbering or merging steps is a breaking change for
  any operator following the runbook from a previous bundle.
- **Operator-facing log lines are stable**. The `[prereqs] platform=...`
  / `[prereqs] repo_root=...` / `[prereqs] libvmaf_bin=...` triplet
  printed by `01-prereqs.sh` is grep'd by `run-full-pipeline.sh` for
  the smoke + by external diagnostics tooling; do not change the
  bracket prefix or the key=value spelling.

## Rebase-sensitive invariants

- **`_platform_detect.sh` returns a fixed set of platform tokens**
  (`linux-x86_64-cuda`, `linux-x86_64-sycl`, `linux-x86_64-vulkan`,
  `darwin-arm64-cpu`, `darwin-x86_64-cpu`, `unknown`). The
  `binaries/<platform>/vmaf` lookup in every numbered script
  depends on these tokens being lower-case-hyphen-separated.
  Adding a new token (e.g. for a future ROCm/HIP lane) requires
  the matching `binaries/<platform>/` directory.

- **`KIT_FAKE_*` env-var hooks are part of the public test contract**.
  `tests/test_platform_detect.sh` and any external consumer that
  wants to mock `uname` / `nvidia-smi` / `vainfo` / `ffmpeg
  videotoolbox` go through `KIT_FAKE_UNAME_S` / `_M`,
  `KIT_FAKE_HAS_NVIDIA_SMI`, `KIT_FAKE_HAS_IHD`,
  `KIT_FAKE_HAS_VIDEOTOOLBOX`. Renaming these silently breaks
  every external test harness.

- **`LIBVMAF_BIN` env override semantics**: when set explicitly,
  the kit bypasses the `binaries/<platform>/vmaf` auto-discovery.
  Operators use this to point at a system-wide libvmaf install
  or a fork-built binary outside the kit's tree. **Do not**
  introduce new env vars that override `LIBVMAF_BIN` or take
  precedence over it — operator surprise compounds.

- **`02-generate-corpus.sh` reuses `scripts/dev/hw_encoder_corpus.py`
  unchanged** (per ADR-0324). The kit imports it via path; renaming
  the producer or its CLI breaks every kit run. The corpus
  schema-version field is also load-bearing for `analyze_knob_sweep.py`
  consumers (see `ai/AGENTS.md` for the schema-version SCHEMA_VERSION=3
  follow-up notes).

- **Darwin gate skips CUDA/NVIDIA probes.** macOS runs the CPU
  path (NEON / AVX2) and probes ffmpeg's VideoToolbox availability
  instead. Adding a `darwin-arm64-metal` token (future Metal-on-Apple
  port) requires updating the Darwin gate so it continues to skip
  the NVIDIA probes — currently the gate matches `darwin-*` prefix.

- **The kit is a distribution surface, not a code surface.** Edits
  here that touch the in-tree producers (e.g. silently rewriting
  `hw_encoder_corpus.py` from inside the kit) are out of scope —
  send a separate PR against `scripts/dev/` instead.

## Twin-update awareness

The kit bundles in-tree scripts. When editing kit scripts, walk:

- `scripts/dev/hw_encoder_corpus.py` — corpus producer.
- `ai/scripts/run_ensemble_v2_real_corpus_loso.sh` — LOSO retrain.
- `ai/scripts/validate_ensemble_seeds.py` — verdict emission.
- `ai/scripts/export_ensemble_v2_seeds.py` — model export.
- `scripts/ci/ensemble_prod_gate.py` — production gate.
- `docs/development/ensemble-training-kit.md` — runbook (if it
  exists; otherwise the kit's `README.md` is canonical).

If the kit changes any of these consumers' invocation contracts,
the `README.md` operator runbook updates in the same PR.

## Build kit-bundled binaries

Operators build the per-platform bundled binary once via:

```bash
bash tools/ensemble-training-kit/build-libvmaf-binaries.sh --platform linux-x86_64-cuda
```

The script writes to `tools/ensemble-training-kit/binaries/<platform>/vmaf`.
The `binaries/` subtree is gitignored (per the project's git policy
on built artefacts) — operators rebuild for their own host.

## Governing ADRs

- [ADR-0025](../../../docs/adr/0025-copyright-handling-dual-notice.md) —
  dual-copyright policy.
- [ADR-0303](../../../docs/adr/0303-fr-regressor-v2-ensemble-prod-flip.md) —
  production-flip criteria for FR-regressor v2 (the verdict
  consumed by `04-validate.sh`).
- ADR-0309
  ([`0309-fr-regressor-v2-ensemble-real-corpus-retrain.md`](../../../docs/adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md))
  — ensemble retrain runbook (companion to this kit).
- [ADR-0324](../../../docs/adr/0324-ensemble-training-kit.md) —
  kit governance + scope (fork-original; no upstream equivalent).
