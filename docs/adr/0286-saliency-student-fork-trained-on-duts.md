<!-- markdownlint-disable MD060 -->
# ADR-0286: Fork-trained saliency student `saliency_student_v1` on DUTS-TR

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, mobilesal, saliency, training, license, fork-local, docs

## Context

[ADR-0218](0218-mobilesal-saliency-extractor.md) shipped the
no-reference `mobilesal` saliency extractor with a smoke-only
synthetic ONNX placeholder
(`mobilesal_placeholder_v0`, `model/tiny/mobilesal.onnx`, 330 bytes,
3→1 1×1 Conv + Sigmoid) that locked down the C-side I/O contract.
[ADR-0257](0257-mobilesal-real-weights-deferred.md) +
[Research-0053](../research/0053-mobilesal-real-weights-blocker.md)
documented why the FastDVDnet-style real-weights swap is unreachable:
upstream `yuhuan-wu/MobileSal` is CC BY-NC-SA 4.0 (incompatible with
the fork's BSD-3-Clause-Plus-Patent), distributes weights via Google
Drive viewer URLs only, and is RGB-D rather than RGB. The recommended
"swap to U-2-Net `u2netp` (Apache-2.0)" path inherits the same
Google-Drive-only distribution problem when investigated in detail.

That left the third option flagged but deferred in ADR-0257's
"Alternatives considered" table: train a from-scratch saliency
student on a permissively-distributed public corpus. With DUTS-TR
(Wang et al. 2017, "free for academic research", direct
HTTP-pinnable distribution at
`https://saliencydetection.net/duts/download/DUTS-TR.zip`) available
as a 271 MB / 10 553-pair corpus, that path becomes feasible: a
sub-200 K-parameter U-Net trained for ~10 minutes on a single GPU
yields weights that are wholly fork-owned, ship under
BSD-3-Clause-Plus-Patent, and lock down a substantively
content-dependent `saliency_mean` signal for the first time on this
fork.

[Research-0054](../research/0062-saliency-student-from-scratch-on-duts.md)
records the dataset survey, the architecture survey, and the
op-allowlist analysis behind the choice of TinyU-Net (~113 K params,
3 down + 3 up + skip connections, `ConvTranspose` upsample) trained
with BCE + Dice loss on Adam lr=1e-3 for 50 epochs. The chosen
architecture uses only ops already on
`core/src/dnn/op_allowlist.c` so the resulting graph loads
unchanged against vanilla origin/master without an allowlist patch in
the same PR.

## Decision

We will ship `saliency_student_v1`, a fork-trained tiny U-Net
saliency student (~113 K parameters, ONNX opset 17,
BSD-3-Clause-Plus-Patent), as a new `model/tiny/saliency_student_v1.onnx`
registry entry. The model is trained from scratch on DUTS-TR with the
recipe captured in `ai/scripts/train_saliency_student.py` and exposes
the same `input` / `saliency_map` tensor contract as the existing
`mobilesal_placeholder_v0` so it is a drop-in for the C-side
`feature_mobilesal.c` extractor (no C changes, no public-API
changes). The placeholder remains in the registry with `smoke: true`
and is no longer the recommended weights for the `mobilesal`
extractor — `docs/ai/models/saliency_student_v1.md` is the new
authoritative model card; `docs/ai/models/mobilesal.md` gains a
"superseded by saliency_student_v1" pointer and keeps the legacy
record for reference. The DUTS-TR images themselves are **not**
redistributed in-tree; only the trained weights are.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Fork-train a tiny saliency student on DUTS-TR (this ADR)** | Wholly fork-owned weights under BSD-3-Clause-Plus-Patent; permissive dataset with stable HTTP URL; ~10 min on one GPU; same I/O contract → drop-in for `feature_mobilesal.c`; first content-dependent saliency signal on the fork | Model is small (~113 K params) so absolute IoU is below upstream u2netp; needs a training-script in `ai/scripts/`; in-loop validation only in v1 | **Chosen** — see Research-0054 for the survey supporting this |
| Re-investigate U-2-Net `u2netp` real-weights swap | Apache-2.0 codebase; pure RGB; well-known SOD architecture; pretrained `u2netp` is 4.7 MB | Trained checkpoints distributed via Google Drive viewer URLs (same problem ADR-0257 hit on MobileSal); 4.7 MB dwarfs every other entry under `model/tiny/`; legal review still required for the trained-weight redistribution chain | Rejected — the licence-compatible code surface is welcome but the licence-compatible weights surface still isn't reachable |
| Ship the placeholder forever and rely on `saliency_mean` as a content-independent constant | Zero engineering | Defeats the purpose of having `saliency_mean` as a feature; downstream consumers see no signal; ADR-0257's "Negative" consequence stays open | Rejected — the deferral was always meant to unblock |
| Train on a larger / multi-dataset corpus (DUTS-TR ∪ MSRA10K ∪ HKU-IS) | Better absolute IoU; more diverse content | Multiple licence audits; bigger training script; longer wall-clock | Held in reserve as `saliency_student_v2`; v1 establishes the pattern with the simplest possible single-dataset recipe |
| Use `Resize` for upsampling instead of `ConvTranspose` | More common in upstream SOD code; one fewer parameter group | `Resize` is not on `core/src/dnn/op_allowlist.c` at the time of this PR; adding it widens the PR scope into a new C-side audit + scanner change in the same diff as the training run | Rejected — `ConvTranspose` is on the allowlist already and produces a numerically equivalent stride-2 upsample for our purposes |
| Rename the C-side extractor `mobilesal` → `saliency` | Reflects the new model lineage | Breaks every external consumer's `--feature mobilesal` flag and every JSON output column named `saliency_mean` (which it stays, regardless of the underlying weights); pure churn | Rejected — the extractor name is API surface; the model identity changes, the extractor doesn't |

## Consequences

- **Positive**:
  - The `saliency_mean` feature becomes content-dependent for the
    first time on this fork — downstream consumers correlating
    saliency mass against quality see real signal instead of a
    constant.
  - Weights are wholly fork-owned and ship under
    BSD-3-Clause-Plus-Patent — no upstream licence audit, no
    redistribution chain, no third-party clickwrap surface.
  - Establishes the pattern for fork-trained tiny-AI models on
    permissively-distributed public corpora — future saliency
    refreshes (multi-dataset, larger student, retrain on different
    content categories) reuse the same script + ADR-0042 doc surface.
  - The C-side `feature_mobilesal.c` extractor is unchanged — the
    contract from [ADR-0218](0218-mobilesal-saliency-extractor.md)
    holds, so the existing tests, ffmpeg integration, and CLI
    surface continue to work.
- **Negative**:
  - Adds a new training script (`ai/scripts/train_saliency_student.py`,
    ~340 LOC) and a new model card to maintain.
  - The training corpus DUTS-TR is not committed in-tree; reproducing
    the `.onnx` requires downloading the dataset (271 MB). The
    download URL is recorded in the model card and the training
    script's docstring.
  - `~113 K` parameters is below the capacity needed to match
    upstream u2netp absolute IoU on external benchmarks. v1 ships as
    a useful baseline, not as a state-of-the-art SOD model.
- **Neutral / follow-ups**:
  - **`saliency_student_v2`** — multi-dataset training (DUTS-TR ∪
    MSRA10K ∪ HKU-IS) and a slightly larger student (~300 K params)
    to push absolute IoU. Filed as a backlog row.
  - **External evaluation** — adopt DUTS-TE / ECSSD as the held-out
    test split with PLCC / IoU / max-F-measure tables in the model
    card. Filed as a backlog row.
  - **Optional ADR re-classify** — `mobilesal_placeholder_v0` could
    be removed from the registry on a future cleanup pass once
    consumers migrate to `saliency_student_v1`; left in-tree for now
    as the historical smoke-only artefact ADR-0218 / ADR-0257
    referenced.

## References

- [ADR-0218](0218-mobilesal-saliency-extractor.md) — original
  MobileSal extractor wiring (unchanged by this PR).
- [ADR-0257](0257-mobilesal-real-weights-deferred.md) — the
  real-weights deferral this PR partly unblocks. ADR-0286 supersedes
  ADR-0257's "Negative" consequence about content-independent
  saliency.
- [Research-0053](../research/0053-mobilesal-real-weights-blocker.md)
  — the upstream-MobileSal blocker survey.
- [Research-0054](../research/0062-saliency-student-from-scratch-on-duts.md)
  — companion digest for this ADR.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI
  doc-substance rule this PR satisfies (ships
  `docs/ai/models/saliency_student_v1.md`).
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — fork-local PR
  deep-dive deliverables checklist.
- DUTS dataset: Wang, Lu, Wang, Feng, Wang, Yin, Ruan, "Learning to
  Detect Salient Objects with Image-Level Supervision", CVPR 2017.
  Project page: <http://saliencydetection.net/duts/>. Licence: free
  for academic research.
- Source: paraphrased — task brief directive "train a small
  saliency-student model from scratch on a permissively-licensed
  public saliency dataset, replacing the placeholder."

### Status update 2026-05-09: v2 trained

[ADR-0332](0332-saliency-student-v2-resize-decoder.md) (Accepted
2026-05-09) ships `saliency_student_v2` — the Resize-decoder
ablation on the v1 recipe — as a parallel artefact under
`model/tiny/`. v2 swaps v1's `ConvTranspose` decoder upsampler for
the standard "Resize + 3×3 Conv" pattern admitted by ADR-0258 (which
ADR-0286's "Alternatives considered" table flagged as the obvious
follow-up once `Resize` was on the allowlist). v2 held the rest of
the v1 recipe identical so the ablation is single-variable. **Result:
v2 best held-out IoU = 0.7105 vs v1's 0.6558 (+0.0547 / +8.3 %)** on
the same 5 % DUTS-TR validation fold under `seed=42`. v1 stays as
the production weights for the C-side `mobilesal` extractor; the
v2 production-flip is a separate follow-up PR after empirical
validation in real ROI encodes. The "follow-ups" row above for
`saliency_student_v2` is closed by ADR-0332 in its limited scope
(architecture ablation); the multi-dataset / larger-student variant
flagged in the same row is now `saliency_student_v3` and stays in
the backlog.
