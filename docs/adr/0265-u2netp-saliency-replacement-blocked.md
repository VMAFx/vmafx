<!-- markdownlint-disable MD013 MD060 -->
# ADR-0265: U-2-Net `u2netp` saliency replacement blocked on weights distribution + op allowlist

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, mobilesal, u2netp, saliency, license, op-allowlist, fork-local, docs

## Context

[ADR-0257](0257-mobilesal-real-weights-deferred.md) (T6-2a-followup
blocker, PR #328) deferred the MobileSal real-weights swap because
upstream `yuhuan-wu/MobileSal` is CC BY-NC-SA 4.0, distributes weights
through Google Drive viewer URLs, and is RGB-D where the fork's C
contract is RGB-only. ADR-0257's recommended replacement path was to
switch the underlying model family to U-2-Net's `u2netp` variant
(Apache-2.0, ~4.7 MB, pure RGB, drop-in compatible with the
`saliency_mean` I/O contract); the work was filed as backlog row
T6-2a-replace-with-u2netp.

This ADR records the result of attempting that replacement. Two
independent blockers came up that PR #326's FastDVDnet pattern (pin
upstream commit + curl `.pth` from raw GitHub URL + wrap with adapter

- export to allowlist-safe ONNX with `< 1e-5` parity) cannot resolve:

1. **Weights distribution — Google Drive only.** The upstream
   repository [`xuebinqin/U-2-Net`](https://github.com/xuebinqin/U-2-Net)
   carries a clean SPDX `LICENSE` file (Apache-2.0; verified via
   `gh api repos/xuebinqin/U-2-Net/license` on 2026-05-03), but it
   has **zero GitHub releases** and ships **no `.pth` checkpoints in
   the repository tree** (recursive listing of HEAD `ac7e1c81` returns
   no `*.pth` paths). The `README.md` links the `u2netp.pth` (~4.7 MB)
   exclusively through a Google Drive viewer URL that requires an
   authenticated browser session and a "Download anyway" click —
   the same distribution-channel blocker that ruled out MobileSal in
   ADR-0257. There is no stable raw URL the `export_u2netp.py`
   script could pin by SHA, so the FastDVDnet pattern is not
   reproducible in CI.
2. **ONNX op allowlist — `Resize` is not on the list.** U-2-Net's
   `u2net.py` builds its U^2 architecture with bilinear up-sampling
   (`F.upsample(src, size=tar.shape[2:], mode='bilinear')` —
   verified by reading `model/u2net.py` at upstream HEAD) at every
   decoder stage. PyTorch's ONNX exporter lowers
   `F.upsample(..., mode='bilinear')` to the `Resize` op; the
   `core/src/dnn/op_allowlist.c` list does **not** include
   `Resize` (it includes `Conv`, `ConvTranspose`, `MaxPool`,
   `AveragePool`, the BN/activation set, `Reshape`/`Transpose`/
   `Concat`/`Slice`/`Gather`, but no scale-changing op). Replacing
   `F.upsample` with a hand-written reshape+transpose decomposition
   (the trick PR #326 used for `nn.PixelShuffle` →
   `Reshape/Transpose/Reshape`) does not work for *bilinear*
   resampling — bilinear interpolation is not a pure shape op, it
   requires a 2-tap filter tree that would either widen the
   allowlist (security review surface) or pre-compute the bilinear
   weights into a dynamic-stride `Conv` (a much larger graph and a
   research project on its own).

These two blockers are independent: even if upstream tomorrow cut a
GitHub release with the weights as an artefact, the allowlist
mismatch would still force a separate scope decision (widen the
allowlist to include `Resize` under bounded-attribute constraints,
or rewrite `u2net.py` in-tree against allowlist-safe primitives).

This is the second saliency replacement attempt to hit a
distribution-channel blocker (after ADR-0257). The pattern is
becoming load-bearing for tiny-AI imports: any model whose weights
live behind Google Drive cannot be swapped in via the FastDVDnet
pattern, regardless of license, until upstream cuts a stable raw
artefact or the fork ships an authenticated-fetch unblock.

## Decision

We will **defer the T6-2a-replace-with-u2netp model-family swap**
indefinitely and keep the smoke-only synthetic placeholder
(`mobilesal_placeholder_v0`, `smoke: true`) at
`model/tiny/mobilesal.onnx` shipped unchanged. This PR ships
[Research-0054](../research/0055-u2netp-saliency-replacement-survey.md)
recording the upstream survey, the two blockers, and the unblock
paths considered, plus updates to `docs/ai/models/mobilesal.md`
and `model/tiny/registry.json` (notes-only) so the user-facing
record points at this ADR alongside ADR-0257. The C-side
`feature_mobilesal.c` extractor and its smoke test are not
touched — the `input` / `saliency_map` tensor names and NCHW
shapes carry over to any future drop-in unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Defer u2netp swap; keep placeholder; document blocker (this ADR)** | Honest record of what's actually shipped; corrects ADR-0257's recommendation in light of the second blocker (op allowlist); aligns with the task brief's escalation directive ("if u2netp weights download fails, ship a docs-only blocker PR ... don't push fake weights"); zero C-side surface change | T6-2a-followup remains open with no near-term unblock | **Chosen** — see Research-0054 |
| Vendor `u2netp.pth` via an in-tree authenticated-fetch helper (gdown) | Real saliency signal; clean Apache-2.0 license | Adds `gdown` (or equivalent Google-Drive-scraper) to the fork's runtime deps; Google can break the unauth path at any time; CI cannot reproduce the export deterministically; license-clean weights from a fragile distribution channel are still a supply-chain risk | Rejected — distribution-channel hardening is a separate scope decision (and the task brief explicitly forbids baking gdown into the PR) |
| Mirror `u2netp.pth` through the fork's own release artefacts | Stable raw URL the export script can pin; license permits redistribution under Apache-2.0 §4 with NOTICE attribution | Requires Apache-2.0 NOTICE + license-text bundling step in the release pipeline; opens a precedent for re-hosting every upstream tiny-AI weight blob in our own releases (storage + audit cost); does not solve the op-allowlist blocker | Filed as long-shot follow-up (`T6-2a-mirror-u2netp-via-release`); blocked behind the op-allowlist decision regardless |
| Widen the ONNX op allowlist to include `Resize` (bilinear-only, fixed-attribute) | Unblocks every model that uses bilinear up-sampling (U-2-Net, U-Net, most decoders); a single security-review pass amortises across future imports | `Resize` has 6+ optional attributes (mode, coordinate_transformation_mode, nearest_mode, antialias, axes, keep_aspect_ratio_policy) any of which could change graph semantics; bounding the attribute set requires fork-local op-validator logic; security review is an independent ADR scope | Filed as `T6-2a-widen-allowlist-resize`; not bundled here because the licence + distribution blocker is independent and would still block |
| Rewrite `u2net.py` in-tree against allowlist-safe primitives (no `Resize`) | Fork-owned graph with verifiable lineage to upstream weights | Bilinear upsampling has no exact decomposition into `{Conv, Reshape, Transpose, Slice}` without either pre-computing the bilinear-kernel `Conv` (graph blow-up, dynamic stride is hard) or accepting nearest-neighbour upsampling (re-trains from scratch) | Rejected — engineering effort comparable to from-scratch retrain, with worse provenance |
| Train a from-scratch saliency student on a permissive corpus (DUTS / DUT-OMRON) | Fully fork-owned weights with clean provenance; sidesteps every distribution-channel question; can be designed within the existing op allowlist from the start | Engineering effort comparable to the rest of T6-2a put together; no in-tree SOD training harness; quality-vs-baseline calibration is a research project on its own | Deferred — too large for a "swap weights" follow-up; revisit if both blockers above remain unresolved at the next tiny-AI roadmap planning round |
| Ship u2netp anyway with a fake/random `.pth` proxy and document it as a placeholder | Pattern matches the original placeholder | Conflates "smoke" and "real weights" — already what `mobilesal_placeholder_v0` is; adds nothing; explicitly forbidden by the task brief ("don't push fake weights") | Rejected — duplicate of the existing placeholder |

### Status update 2026-05-08: path B scaffold landed

[ADR-0412](0412-u2netp-fork-mirror-scaffold.md) lands the
**fork-local release-artefact mirror scaffold** named in this
ADR's "Neutral / follow-ups" §`T6-2a-mirror-u2netp-via-release`.
The scaffold ships the licence text
(`LICENSES/Apache-2.0-u2netp.txt`), the model-card stub
(`docs/ai/models/u2netp_mirror_card.md`), the operator workflow
doc (`docs/ai/u2netp-mirror.md`), and an idempotent
release-pipeline guard in `.github/workflows/supply-chain.yml`
that activates when the binary is uploaded. The binary itself is
deferred to a sibling PR pending three open compliance answers
from the user.

Independently, the **op-allowlist axis** of this ADR was already
resolved by [ADR-0258](0258-onnx-allowlist-resize.md), and the
**primary saliency path** is now
[`saliency_student_v1`](../ai/models/saliency_student_v1.md)
([ADR-0286](0286-saliency-student-fork-trained-on-duts.md), a
fork-trained DUTS student) — the mirror exists as a *fallback*
for upstream-lineage citation / comparative evaluation, not as
the recommended path.

This ADR's body remains immutable per
[ADR-0028](0028-adr-maintenance-rule.md); this appendix records
status only.

## Consequences

- **Positive**:
  - The user-facing record (this ADR + Research-0054 +
    `docs/ai/models/mobilesal.md` update) corrects ADR-0257's
    recommendation in light of the op-allowlist mismatch.
  - Future researchers reaching the same task land on a written
    survey that covers both axes (distribution + op allowlist)
    instead of repeating each walk.
  - The C-side `feature_mobilesal.c` extractor and its smoke test
    are not touched — `saliency_mean` continues to flow end-to-end
    as a content-independent value, and the I/O contract stays
    stable for any future drop-in.
  - Aligns with the task brief's "don't fake it" directive — we
    record the real reasons real weights aren't shipping rather
    than producing a graph that would look like real weights but
    couldn't be.
- **Negative**:
  - `saliency_mean` remains content-independent (~0.5 regardless of
    input) until one of the unblock paths lands. Downstream
    consumers correlating saliency mass against quality see no
    signal until then.
  - Two open backlog rows now (T6-2a-replace-with-u2netp from
    ADR-0257, plus T6-2a-widen-allowlist-resize and
    T6-2a-mirror-u2netp-via-release from this ADR).
- **Neutral / follow-ups**:
  - **T6-2a-widen-allowlist-resize** — separate ADR-scope
    decision: widen `op_allowlist.c` to include `Resize` under a
    bounded-attribute schema (`mode ∈ {nearest, bilinear}`,
    `coordinate_transformation_mode ∈ {asymmetric, half_pixel}`,
    no `antialias`, no dynamic `axes`). Pre-requisite for any
    tiny-AI import that uses bilinear upsampling.
  - **T6-2a-mirror-u2netp-via-release** — long-shot follow-up:
    cut a fork-local release artefact mirroring `u2netp.pth` from
    upstream Google Drive (Apache-2.0 §4 with NOTICE attribution),
    so the export script can pin a stable raw URL by SHA.
    Blocked behind T6-2a-widen-allowlist-resize regardless.
  - **T6-2a-train-saliency-student** — the from-scratch fallback
    if both unblocks above stall. Out-of-scope until a SOD training
    harness lands.

## References

- [ADR-0218](0218-mobilesal-saliency-extractor.md) — original
  MobileSal extractor design with the smoke-only placeholder
  (this ADR continues the deferral chain started by ADR-0257).
- [ADR-0257](0257-mobilesal-real-weights-deferred.md) (PR #328)
  — sibling blocker decision deferring MobileSal real weights and
  recommending the u2netp swap that this ADR is now also
  deferring.
- [ADR-0253](0253-fastdvdnet-pre-real-weights.md) (PR #326) —
  sibling real-weights swap that *did* succeed (FastDVDnet, MIT,
  GitHub-raw downloadable, RGB-only architecture). The pattern
  this ADR was supposed to mirror.
- [Research-0054](../research/0055-u2netp-saliency-replacement-survey.md)
  — full upstream survey, license + distribution analysis, and
  op-allowlist audit.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI
  doc-substance rule this PR satisfies.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — fork-local PR
  deep-dive deliverables checklist.
- Upstream code: <https://github.com/xuebinqin/U-2-Net>
  (HEAD `ac7e1c81`, SPDX = Apache-2.0).
- Upstream paper: Qin, Zhang, Huang, Dehghan, Zaiane, Jagersand,
  *"U^2-Net: Going Deeper with Nested U-Structure for Salient
  Object Detection"*, Pattern Recognition 2020.
- Source: paraphrased — task brief directive "if u2netp weights
  download fails, ship a docs-only blocker PR similar to #328's
  mobilesal pattern, documenting what's needed. Don't push fake
  weights."

### Status update 2026-05-08

The three Consequences-section follow-up paths (T6-2a-widen-allowlist-resize,
T6-2a-mirror-u2netp-via-release, T6-2a-train-saliency-student) have
moved as follows:

- **Path A — `T6-2a-widen-allowlist-resize`** is **closed** by
  [ADR-0258](0258-onnx-allowlist-resize.md) (Accepted 2026-05-03).
  ADR-0258 opted *against* adding per-attribute enforcement for `Resize`
  to the op allowlist, aligning with the
  [ADR-0169](0169-wire-scanner-scope.md) wire-scanner-scope rule —
  enforcement at scanner-level, not at op-allowlist-level. The
  allowlist itself was not widened; the security review simply moved
  to the wire scanner.
- **Path C — `T6-2a-train-saliency-student`** is **closed** by
  PR #359 (`saliency_student_v1`, ~113 K params, ONNX opset 17,
  trained from scratch on DUTS-TR; reported IoU 0.6558 on DUTS-TR).
  See [ADR-0286](0286-saliency-student-fork-trained-on-duts.md) and
  the registry row at `model/tiny/registry.json`. The smoke-only
  `mobilesal_placeholder_v0` is no longer the production
  saliency-mass surface; `feature_mobilesal.c`'s `saliency_mean` is
  no longer content-independent.
- **Path B — `T6-2a-mirror-u2netp-via-release`** is **in flight** as
  PR #469 (`feat(ai): u2netp fork-local release-artefact mirror
  scaffold (ADR-0412)`). When PR #469 lands, the original u2netp
  unblock path closes regardless — but the saliency surface is
  already covered by the path-C delivery, so PR #469 becomes a
  reference replacement / cross-validation rather than the primary
  saliency model.

This appendix records the path-A and path-C closures and the
in-flight status of path B; it does not modify the original
Decision or Alternatives table. Per [ADR-0028](0028-adr-maintenance-rule.md)
appendix-only rule.
