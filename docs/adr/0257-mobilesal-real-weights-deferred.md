<!-- markdownlint-disable MD060 -->
# ADR-0257: MobileSal real-weights swap deferred (T6-2a-followup blocker)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, mobilesal, saliency, license, fork-local, docs

## Context

[ADR-0218](0218-mobilesal-saliency-extractor.md) shipped the
no-reference `mobilesal` saliency extractor with a smoke-only
synthetic ONNX placeholder (`model/tiny/mobilesal.onnx`, 330 bytes,
3→1 1×1 Conv + Sigmoid) and recorded the swap to "real upstream
MIT-licensed weights" as the T6-2a-followup task. PR #326 just
demonstrated the working pattern for that kind of swap on
FastDVDnet (ADR-0253): pin an upstream commit, curl the `.pth`
from a stable raw URL, wrap with a luma adapter, export under the
fork's ONNX op allowlist with `< 1e-5` PyTorch ↔ ONNX parity, and
bump `smoke: false` in `model/tiny/registry.json`.

Mirroring that pattern for MobileSal blocks on three independent
findings, captured in detail in
[Research-0053](../research/0053-mobilesal-real-weights-blocker.md):

1. The upstream repository
   ([`yuhuan-wu/MobileSal`](https://github.com/yuhuan-wu/MobileSal))
   declares **CC BY-NC-SA 4.0** in its `README.md` §License — not
   MIT as ADR-0218 stated. The Non-Commercial clause and the
   Share-Alike clause are each independently incompatible with the
   fork's BSD-3-Clause-Plus-Patent licence and with downstream
   commercial-pipeline use of libvmaf. The repository carries no
   SPDX `LICENSE` file (`gh api .../MobileSal --jq '.license'`
   returns `null`), so the README declaration is the only legal
   record.
2. Trained checkpoints are distributed exclusively through Google
   Drive viewer URLs that require an authenticated browser session
   to download — there is no GitHub release artefact and no stable
   raw URL the export script could pin by SHA. Even with a
   waivable licence the FastDVDnet pattern is unreproducible in
   CI.
3. MobileSal is an **RGB-D** salient-object-detection network: the
   forward signature is `forward(input, depth=None, test=True)` and
   the published weights were trained against a joint RGB+depth
   objective. The fork's C-side contract supplies luma-derived RGB
   only with no depth source. Running upstream MobileSal in
   `depth=None` mode is off-distribution use of the trained weights;
   pairing it with a depth-estimation network adds a second ONNX
   dependency, a second licence audit, and a second off-distribution
   step.

These findings together rule out the FastDVDnet-style pinned-commit
real-weights swap. The decision needed is whether to keep the
placeholder shipped while a permissive replacement is identified,
or to make a substantive scope shift in this PR (rename the
extractor, swap the model family, ...).

## Decision

We will **defer the T6-2a-followup real-weights swap** for
MobileSal indefinitely and keep the smoke-only synthetic
placeholder shipped at `model/tiny/mobilesal.onnx`
(`mobilesal_placeholder_v0`, `smoke: true`) until a permissive,
RGB-only saliency model can replace the underlying model family.
This PR ships the blocker digest
([Research-0053](../research/0053-mobilesal-real-weights-blocker.md))
and the corrected user-facing licence record in
[`docs/ai/models/mobilesal.md`](../ai/models/mobilesal.md) (which
ADR-0218 inaccurately documented as MIT). The C-side `mobilesal`
extractor and its registry entry are not touched — the I/O contract
remains stable, and any future drop-in (U-2-Net export, distilled
student, or an upstream relicense) replaces the `.onnx` without C
changes. The recommended replacement path — swap to U-2-Net's
4.7 MB `u2netp` variant under Apache-2.0 — is filed as a new
backlog row (T6-2a-replace-with-u2netp) and is not bundled into
this docs PR because the registry-id rename is a substantive scope
shift in its own right.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Defer real-weights swap; keep placeholder; document blocker (this ADR)** | Honest record of what's actually shipped; corrects ADR-0218's MIT claim; aligns with the task-brief directive "don't fake it"; zero C-side surface change | T6-2a-followup remains open with no near-term unblock | **Chosen** — see Research-0053 for the survey supporting this |
| Ship `yuhuan-wu/MobileSal` weights anyway under fair-use / research-only banner | Real saliency signal; matches ADR-0218's original intent | CC BY-NC-SA 4.0 is binary, not negotiable; legal exposure for every commercial libvmaf consumer; share-alike taints the rest of `model/tiny/` | Rejected — license incompatibility is a blocker, not a trade-off |
| Swap to U-2-Net (`xuebinqin/U-2-Net`, Apache-2.0) inside this PR | Permissive licence; pure RGB; well-known SOD model; same `[1, 3, H, W] → [1, 1, H, W]` contract; pretrained `u2netp` is 4.7 MB | Renaming `mobilesal_placeholder_v0` → `u2netp_v1` is a substantive registry / API / docs shift and a fresh ADR-scope decision; mixing the model-family change into a "real weights for MobileSal" PR muddles the review | Filed as a separate backlog row; not bundled here |
| Swap to BBS-Net (`DengPingFan/BBS-Net`, MIT) | MIT licence solves the licence axis | Also RGB-D (same architectural mismatch); also Google-Drive distributed (same reproducibility problem) | Rejected — only fixes one of the three blocker axes |
| Email upstream author for relicensing to MIT/BSD | Cleanest fix if granted | Out-of-band ask with no commitment; even a personal-email grant doesn't bind future redistribution; not actionable in this PR's timeline | Filed as long-shot follow-up; this PR does not depend on it |
| Train a from-scratch saliency student on a permissive corpus (e.g. DUTS) | Fully fork-owned weights with clean provenance | Engineering effort comparable to the rest of T6-2a put together; no in-tree training harness for SOD; calibration story is a research project on its own | Deferred — too large for a "swap weights" follow-up |
| Keep silently shipping the placeholder without documenting the blocker | Smallest diff | Future researchers re-walk the same survey; ADR-0218's incorrect MIT claim stays in the user-facing docs; violates the project doc-substance rule (CLAUDE.md §12 r10) | Rejected — the corrected licence record is itself user-discoverable |

### Status update 2026-05-08: path B scaffold landed

[ADR-0412](0412-u2netp-fork-mirror-scaffold.md) lands the
fork-local release-artefact mirror scaffold for `u2netp.pth`,
named in [ADR-0265](0265-u2netp-saliency-replacement-blocked.md)'s
T6-2a path (b) follow-up
(`T6-2a-mirror-u2netp-via-release`). The scaffold is the
fallback path; the **primary** replacement for this ADR's
deferred real-weights swap is now
[`saliency_student_v1`](../ai/models/saliency_student_v1.md)
([ADR-0286](0286-saliency-student-fork-trained-on-duts.md), a
fork-trained DUTS student under BSD-3-Clause-Plus-Patent), which
exited this ADR's "Negative consequences" content-independent
saliency note as of PR #341.

This ADR's body remains immutable per
[ADR-0028](0028-adr-maintenance-rule.md); this appendix records
status only.

## Consequences

- **Positive**:
  - The user-facing record (this ADR + Research-0053 +
    `docs/ai/models/mobilesal.md` update) corrects ADR-0218's
    inaccurate MIT claim before it propagates downstream.
  - Future researchers reaching the same task land on a written
    survey instead of repeating the upstream walk.
  - The C-side `feature_mobilesal.c` extractor and its smoke test
    are not touched — `saliency_mean` continues to flow end-to-end
    as a content-independent value, and the I/O contract stays
    stable for any future drop-in.
  - Aligns with the project's "don't fake it" directive (paraphrased
    from the task brief): we record the real reason real weights
    aren't shipping rather than producing a graph that would look
    like real weights but couldn't be.
- **Negative**:
  - `saliency_mean` remains content-independent (~0.5 regardless of
    input) until T6-2a-replace-with-u2netp lands. Downstream
    consumers correlating saliency mass against quality see
    no signal until then.
  - One more open backlog row.
- **Neutral / follow-ups**:
  - **T6-2a-replace-with-u2netp** — swap the underlying model
    family from MobileSal to U-2-Net's `u2netp` (Apache-2.0,
    4.7 MB, pure RGB). Includes a registry-id rename
    (`mobilesal_placeholder_v0` → `u2netp_v1` or similar), a fresh
    ADR superseding ADR-0218 on the model-family choice, and a
    refresh of `docs/ai/models/mobilesal.md` (rename or split).
    Any C-side touchpoints stay limited to the model-resolver
    stub names — the `input` / `saliency_map` tensor names and
    NCHW shapes carry over unchanged.
  - **T6-2b** (encoder-side `tools/vmaf-roi`) is unaffected — it
    consumes the same model id at runtime and will pick up the
    replacement transparently when T6-2a-replace-with-u2netp lands.

## References

- [ADR-0218](0218-mobilesal-saliency-extractor.md) — original
  MobileSal extractor design with the smoke-only placeholder
  (this ADR supersedes the T6-2a-followup commitment from
  ADR-0218 §"Neutral / follow-ups").
- [ADR-0253](0253-fastdvdnet-pre-real-weights.md) — sibling
  real-weights swap that *did* succeed (FastDVDnet, MIT,
  GitHub-raw downloadable, RGB-only architecture). The pattern
  this ADR was supposed to mirror.
- [Research-0053](../research/0053-mobilesal-real-weights-blocker.md)
  — full upstream survey, license analysis, and alternatives walk.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI
  doc-substance rule this PR satisfies.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — fork-local PR
  deep-dive deliverables checklist.
- Upstream code: <https://github.com/yuhuan-wu/MobileSal> (HEAD
  `8f42ded5`, `README.md` §License = CC BY-NC-SA 4.0).
- Upstream paper: Wu, Liu, Cheng, Lu, Cheng, *"MobileSal: Extremely
  Efficient RGB-D Salient Object Detection"*, IEEE TPAMI 2022.
- Source: paraphrased — task brief directive "if upstream weights
  are unavailable / behind login, don't fake it; open a docs-only
  PR with the blocker digest."
