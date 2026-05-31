<!-- markdownlint-disable MD013 MD060 -->
# ADR-0569: SDK / Tool Version Bumps — 2026-05-18

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `build`, `container`, `ci`, `deps`, `pre-commit`, `fork-local`

## Context

The SDK audit documented in
[Research-0562](../research/0562-sdk-version-audit-2026-05-18.md) identified 8
low-risk version gaps across the dev container, pre-commit toolchain, GitHub
Actions, and Python requirements. Left unaddressed, these gaps accumulate into
larger migration jumps (e.g. ONNX Runtime falling further behind ROCm EP
compatibility) or expose the signing pipeline to unfixed bugs in an older
`cosign` version. The bumps are batched into a single PR to minimise CI
overhead while keeping the diff reviewable.

## Decision

Apply the following 8 bumps atomically in one PR:

| # | Component | File | Old | New | Rationale |
|---|---|---|---|---|---|
| 1 | ONNX Runtime (`ORT_VERSION`) | `dev/Containerfile` | `1.20.1` | `1.26.0` | ORT 1.26 adds ROCm 7.x EP + CUDA 13.x EP, matching the container's existing ROCm 7.2.3 + CUDA 13.x stack (ADR-0541/ADR-0542). Six minor releases of GPU EP improvements. |
| 2 | AMF headers (`AMF_VERSION`) | `dev/Containerfile` | `1.4.36` | `1.5.2` | FFmpeg's `>=1.4.36` probe is satisfied by 1.5.2. `--disable-filter=amf_capture` (already present) neutralises the `DisplayCapture.h` C++ change in 1.5.x. Adds AV1 ROI map + quality improvements. |
| 3 | VVenC (`VVENC_VERSION`) | `dev/Containerfile` | `1.12.0` | `1.14.0` | Semver-stable within 1.x; encoder quality improvements at medium presets, gcc-14 compatibility fixes. |
| 4 | clang-format mirror | `.pre-commit-config.yaml` | `v22.1.3` | `v22.1.5` | Two patch releases; deterministic output within major; prevents pre-commit cache misses from version drift. |
| 5 | black | `.pre-commit-config.yaml` | `26.3.1` | `26.5.1` | Two patch releases; formatting output is deterministic. |
| 6 | ruff-pre-commit | `.pre-commit-config.yaml` | `v0.15.10` | `v0.15.13` | Three patch releases; fixes false-positive suppressions. |
| 7 | `sigstore/cosign-installer` | `.github/workflows/supply-chain.yml` | `cad07c2e…` (v4.1.1) | `6f9f1778…` (v4.1.2) | Security tooling; always track latest patch; SHA re-pinned per SLSA SHA-pin policy. |
| 8 | `libsvm-official` ceiling | `python/requirements.txt` | `<=3.32` | `<=3.37` | Unlocks LIBSVM 3.33–3.37 solver bug fixes; LIBSVM 3.x API is stable by policy. |

Additionally, a version comment is added to `ai/pyproject.toml` noting that the
container tests against ORT 1.26.0 (the floor `>=1.20,<2.0` is unchanged, so
no host-install behaviour changes).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Bump each dependency in its own PR | Easier bisect if one bump breaks something | 8x CI overhead; each PR needs its own ADR + deliverables; total cost dominates benefit | Not chosen — all 8 are genuinely low-risk and independently validated in the audit |
| Hold all until Ubuntu 26.04 base-image PR merges | One fewer Containerfile conflict to manage | Blocks ROCm/CUDA EP improvements in ORT for weeks; linters drift further | Not chosen — Ubuntu 26.04 PR (#1330) touched different ARG lines; Containerfile conflicts were easily resolved |
| Only bump pre-commit tools, skip container ARGs | Zero container-rebuild cost | Misses the highest-value bump (ORT 1.26 ROCm EP support) | Not chosen — container ARG bumps are string-only changes that require no rebuild to merge |

## Consequences

- **Positive**: ORT 1.26 enables ROCm 7.x EP and CUDA 13.x EP inference in the
  container when the next rebuild fires; linter versions align with current
  upstream releases; LIBSVM solver fixes are accessible; Sigstore bundle
  signing uses the latest cosign version.
- **Negative**: Next container rebuild will pull ORT 1.26 — callers using the C
  API via `ort_api->CreateSession` should be unaffected (C API is stable
  across 1.x); Python callers using `onnxruntime.InferenceSession` have no
  breaking changes in the 1.20→1.26 range.
- **Neutral / follow-ups**:
  - Container rebuild required before GPU EP improvements take effect; no rebuild
    is needed to merge this PR.
  - AMF 1.5 AV1 ROI support is inert until vmaf-tune's `h264_amf`/`hevc_amf`/
    `av1_amf` probe validates the shared library at runtime.
  - VVenC 1.14 is a drop-in replace; no vmaf-tune adapter changes needed.
  - libsvm ceiling widened to 3.37; no API changes expected; the Netflix
    golden-data assertions (python/test/) remain untouched.

## References

- [Research-0562: SDK / Runtime / Library Version Audit — 2026-05-18](../research/0562-sdk-version-audit-2026-05-18.md)
- [ADR-0541](0541-dev-container-sycl-hip-runtime-fix.md) — ROCm 7.2.3 + NEO 26.18 pinning
- [ADR-0542](0542-dev-container-full-gpu-plumbing.md) — full GPU plumbing incl. CUDA 13
- [ADR-0221](0221-changelog-adr-fragment-pattern.md) — changelog fragment pattern
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six-deliverable rule
- cosign-installer v4.1.2 tag SHA verified: `6f9f17788090df1f26f669e9d70d6ae9567deba6`
  (source: `gh api repos/sigstore/cosign-installer/tags`)
- req: "Bundle 8 low-risk SDK / tool bumps from the SDK audit into ONE DRAFT PR."
