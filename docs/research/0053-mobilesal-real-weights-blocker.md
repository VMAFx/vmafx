<!-- markdownlint-disable MD060 -->
# Research-0053 — MobileSal real-weights swap blocker

| Field      | Value                                                  |
| ---------- | ------------------------------------------------------ |
| **Date**   | 2026-05-03                                             |
| **Status** | Blocker recorded — placeholder remains shipped         |
| **Tags**   | dnn, tiny-ai, mobilesal, saliency, license, blocker    |

Companion to [ADR-0257](../adr/0257-mobilesal-real-weights-deferred.md).
Captures the upstream survey, license analysis, and architectural
mismatch that together block the original T6-2a-followup plan to swap
the smoke-only MobileSal placeholder for real upstream weights, and
records the alternatives reviewed before the deferral decision.

## Background

[ADR-0218](../adr/0218-mobilesal-saliency-extractor.md) (T6-2a) shipped
a smoke-only synthetic ONNX (3→1 1×1 Conv + Sigmoid, 330 bytes) at
`model/tiny/mobilesal.onnx` that locks down the C-side
`feature_mobilesal.c` extractor's input/output contract — input
`input` (float32 NCHW `[1, 3, H, W]` ImageNet-normalised RGB), output
`saliency_map` (float32 NCHW `[1, 1, H, W]` per-pixel saliency in
`[0, 1]`). The follow-up T6-2a was to ship real upstream MobileSal
weights, mirroring the FastDVDnet T6-7b pattern in PR #326. This
digest records why that plan no longer goes through under the
constraints recorded in the original ADR.

## Upstream survey (2026-05-03)

The MobileSal paper (Wu, Liu, Cheng, Lu, Cheng, *"MobileSal:
Extremely Efficient RGB-D Salient Object Detection"*, IEEE TPAMI
2022) has two practical reference implementations:

- **`yuhuan-wu/MobileSal`** (the paper authors' canonical PyTorch
  release; ADR-0218's `yun-liu/MobileSal` URL was a typo — that
  account does not host the codebase).
- **`yuhuan-wu/MobileSal:jittor`** (Jittor port on the same repo).

Both ship the same trained checkpoints. The repository was probed via
the GitHub REST API on 2026-05-03 (HEAD `8f42ded5`).

### License — incompatible with the fork

`yuhuan-wu/MobileSal/README.md` §License declares:

> The code is released under the [Creative Commons
> Attribution-NonCommercial-ShareAlike 4.0 International Public
> License] for NonCommercial use only.

(Quoted verbatim from `README.md` §License at HEAD `8f42ded5`.)

The repository carries no SPDX `LICENSE` file (`gh api
repos/yuhuan-wu/MobileSal --jq '.license'` returns `null`); the only
license declaration is the README sentence above.

CC BY-NC-SA 4.0 is incompatible with the fork's distribution model on
two independent axes:

1. **Non-Commercial clause** — the fork is BSD-3-Clause-Plus-Patent
   and is consumed by commercial encoder pipelines (FFmpeg + libvmaf
   he in-tree `ffmpeg-patches/` series ship with no commercial
   restriction). Bundling a CC-NC weight blob would force every
   downstream commercial consumer to either strip the model or
   relicense their use.
2. **Share-Alike clause** — even if (1) were waived, SA forces any
   derivative containing the weights to release under CC BY-NC-SA
   4.0 or a compatible licence. That's incompatible with both the
   fork's BSD-3-Clause-Plus-Patent licence and Netflix upstream's
   licence, and would taint downstream model-fusion ensembles that
   include `mobilesal` alongside permissive `lpips_sq` /
   `fr_regressor_v1` weights.

ADR-0218's claim that upstream MobileSal is "MIT-licensed" was
inaccurate; this digest is the corrected record.

### Distribution — Google-Drive walled, no GitHub release

The trained checkpoints are not GitHub release artefacts:

- `gh api repos/yuhuan-wu/MobileSal/releases` returns `[]` — the
  repo has zero releases.
- The README links every checkpoint via Google Drive viewer URLs
  (e.g.
  `https://drive.google.com/file/d/1dfyFkdsI1rOfmhmgG-o45ggnOj5Wpr1d/view`).
  A `curl -sIL` HEAD on the viewer URL returns the Drive HTML
  preview page, not the raw `.pth` — the actual download requires
  an authenticated browser session and a "Download anyway" click.

This rules out the FastDVDnet pattern of pinning an upstream commit
and curling the weights file by SHA — there is no stable raw URL to
pin. Even if the licence allowed bundling, the script could not
reproduce the export non-interactively in CI.

### Architectural mismatch — RGB-D, not RGB

MobileSal is **RGB-D salient-object detection**: its forward
signature is

```python
def forward(self, input, depth=None, test=True):
```

(`yuhuan-wu/MobileSal/models/model.py` line 198). The paper's
reported numbers all assume the depth branch is active. The forward
*does* tolerate `depth=None` and skip the `depth_fuse` /
`implicit-depth-restoration` paths, but in that mode the network
runs on RGB only, with no depth input — and the publicly trained
checkpoint was optimised against the joint RGB+depth objective, so
RGB-only inference is an off-distribution use of the weights.

The fork's C-side contract is luma-derived RGB only (a YUV-to-RGB
upsample of the distorted frame; no depth source at all). Adapting
upstream MobileSal to a luma-derived-RGB contract would either:

- run the upstream graph in `depth=None` mode and accept the
  off-distribution quality penalty (analogous to FastDVDnet's
  luma-tile-into-RGB compromise in PR #326 / ADR-0253), or
- synthesise a fake depth map (e.g. from a depth-estimation
  network like MiDaS), which adds a second ONNX dependency, a
  second licence audit, and a third layer of off-distribution use.

Neither variant produces a clean numerical-parity story — the
"PyTorch ↔ ONNX < 1e-5 max-abs" gate from FastDVDnet only certifies
that the export is faithful to upstream's forward, not that the
forward is being used as the upstream authors intended.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Defer real-weights swap; keep placeholder; document blocker** | Honest record of what's actually shipped; future researchers don't repeat the upstream walk; aligns with the [no-test-weakening rule](../../docs/ai/) and "don't fake it" directive in the task brief | T6-2a-followup remains open for the foreseeable future | **Chosen** — see ADR-0257 |
| Adapt `yuhuan-wu/MobileSal` weights anyway under fair-use / research-only banner | Real saliency signal | CC BY-NC-SA 4.0 is not waivable by a downstream user; legal exposure for every commercial consumer of the fork; share-alike taints the rest of `model/tiny/` | Rejected — license incompatibility is binary, not negotiable |
| Email the corresponding author for an MIT/BSD relicense | Clean fix if granted | Out-of-band ask with no commitment from the author; the README-declared license is the legal record; even a personal email grant doesn't bind future redistribution | Filed as a long-shot follow-up in T6-2a-blocker; this PR does not depend on it |
| Switch to **U-2-Net** (`xuebinqin/U-2-Net`, Apache-2.0) as the saliency backbone | Permissive licence; well-known SOD model; pure RGB (no depth); same `[1, 3, H, W] → [1, 1, H, W]` contract; available as a 4.7 MB U2NETP variant; pretrained weights distributed through HuggingFace mirrors with raw download URLs | New ADR-scope decision (different model family); requires a fresh export script; the existing model id `mobilesal_placeholder_v0` would become misleading; renaming the registry entry forces a follow-up PR with API-shape implications for downstream `tools/vmaf-roi` (T6-2b) | Filed as the **recommended T6-2a' replacement path** — not bundled into this docs PR because the rename is a substantive scope shift; tracked as new backlog item T6-2a-replace-with-u2netp |
| Switch to **BBS-Net** (`DengPingFan/BBS-Net`, MIT) | MIT license; well-cited SOD model | Also RGB-D (same architectural mismatch as MobileSal); weights also Google-Drive-distributed | Rejected — moves the licence problem out of the way but leaves the architectural mismatch and the distribution-channel problem |
| Train a from-scratch saliency student on a permissive corpus (e.g. DUTS) and ship the student | Fully fork-owned weights with clean provenance; sidesteps every upstream licence question | Engineering effort comparable to the rest of T6-2a put together; no immediate corpus + training harness in tree; quality-vs-baseline calibration story is a research project on its own | Deferred — too large for a "swap weights" follow-up; revisit if T6-2a-replace-with-u2netp also blocks |
| Distil upstream MobileSal under research-only terms into a fork-owned student trained on a permissive corpus | Knowledge transfer without redistributing the upstream weights | Distillation outputs are still considered derivative works under CC BY-NC-SA 4.0 share-alike; the student would inherit the licence taint unless the upstream author signs off | Rejected — same legal problem as the direct shipping option, just one indirection deeper |

## Recommendation

**Keep the smoke-only placeholder** shipped at
`model/tiny/mobilesal.onnx` (`mobilesal_placeholder_v0`, sha256
`f1226310…`) and document the blocker in
[ADR-0257](../adr/0257-mobilesal-real-weights-deferred.md). The
`mobilesal` extractor remains usable end-to-end — every C-side test
in `core/test/test_mobilesal.c` passes against the placeholder —
but `saliency_mean` stays a content-independent constant (~0.5)
until a permissive replacement lands. The C contract does not
change; any future drop-in (U-2-Net export, distilled student, or
an upstream relicense) just replaces the `.onnx` and bumps the
registry sha256.

The recommended next step (filed as **T6-2a-replace-with-u2netp**)
is to swap the underlying model family from MobileSal to U-2-Net's
4.7 MB `u2netp` variant under Apache-2.0. That is a new scope
decision (rename the registry id, refresh ADR-0218 in a successor
ADR, refresh the user-facing doc) — too substantial to bundle into
this docs-only blocker PR.

## References

- [ADR-0218](../adr/0218-mobilesal-saliency-extractor.md) — original
  MobileSal extractor design with the smoke-only placeholder.
- [ADR-0253](../adr/0253-fastdvdnet-pre-real-weights.md) — sibling
  real-weights swap that *did* succeed (FastDVDnet, MIT, GitHub-
  raw downloadable, RGB-only architecture).
- Upstream paper: Wu, Liu, Cheng, Lu, Cheng, *"MobileSal: Extremely
  Efficient RGB-D Salient Object Detection"*, IEEE TPAMI 2022,
  <https://ieeexplore.ieee.org/document/9647954>.
- Upstream code: <https://github.com/yuhuan-wu/MobileSal> (HEAD
  `8f42ded5`, README §License = CC BY-NC-SA 4.0).
- U-2-Net (recommended replacement, Apache-2.0):
  <https://github.com/xuebinqin/U-2-Net>.
- BBS-Net (rejected MIT alternative; same RGB-D mismatch):
  <https://github.com/DengPingFan/BBS-Net>.
- Task brief directive: paraphrased — "if upstream weights are
  unavailable / behind login, don't fake it; open a docs-only PR
  with the blocker digest."
