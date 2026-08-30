<!-- markdownlint-disable MD060 -->
# Research-0055 — U-2-Net `u2netp` saliency replacement survey

| Field      | Value                                                       |
| ---------- | ----------------------------------------------------------- |
| **Date**   | 2026-05-03                                                  |
| **Status** | Blocker recorded — placeholder remains shipped              |
| **Tags**   | dnn, tiny-ai, mobilesal, u2netp, saliency, license, op-allowlist, blocker |

Companion to [ADR-0265](../adr/0265-u2netp-saliency-replacement-blocked.md).
Captures the upstream survey, the license + distribution audit, the
ONNX op-allowlist mismatch, and the alternatives reviewed before the
deferral decision.

## Background

[ADR-0257](../adr/0257-mobilesal-real-weights-deferred.md) /
[Research-0053](0053-mobilesal-real-weights-blocker.md) (PR #328)
deferred the MobileSal real-weights swap because upstream
`yuhuan-wu/MobileSal` is CC BY-NC-SA 4.0, distributes weights through
Google Drive viewer URLs, and is RGB-D where the fork's C contract is
RGB-only. ADR-0257's recommended next step was filed as backlog row
**T6-2a-replace-with-u2netp**: switch the underlying model family from
MobileSal to U-2-Net's 4.7 MB `u2netp` variant under Apache-2.0, which
is permissive, pure RGB, and (per ADR-0257 §Alternatives) "drop-in
compatible with the saliency I/O contract".

This digest records the result of attempting that replacement. The
plan was to mirror the FastDVDnet T6-7b pattern from PR #326 / ADR-0253:

1. Pin upstream commit by SHA.
2. `curl -L -O <raw URL>/u2netp.pth` from the pinned commit.
3. Wrap with a `LumaAdapter` (Y → [Y, Y, Y] tile, RGB → Y collapse)
   so the upstream graph matches the C-side
   `[1, 3, H, W] → [1, 1, H, W]` contract.
4. `torch.onnx.export` at opset 17 with `do_constant_folding=True`.
5. Verify PyTorch ↔ ONNX max-abs-diff `< 1e-5` over 5 random inputs.
6. Bump `model/tiny/registry.json` `mobilesal_placeholder_v0` →
   `u2netp_v1`, `smoke: false`, license `Apache-2.0`.

The plan blocks on two independent findings (sections below). The
saliency extractor's I/O contract is preserved end-to-end — every
piece of the C side, the smoke test, the registry schema, and the
sidecar layout would still work — but step 2 (download) and step 4
(allowlist-safe export) each have their own irreducible blocker.

## Upstream survey (2026-05-03)

The U-2-Net paper (Qin, Zhang, Huang, Dehghan, Zaiane, Jagersand,
*"U^2-Net: Going Deeper with Nested U-Structure for Salient Object
Detection"*, Pattern Recognition 2020) has one canonical reference
implementation:

- **`xuebinqin/U-2-Net`** (the corresponding author's repository,
  HEAD `ac7e1c81` as of 2026-05-03).

The repository was probed via the GitHub REST API on 2026-05-03.

### License — Apache-2.0 (clean)

The repository carries a clean SPDX `LICENSE` file:

```text
$ gh api repos/xuebinqin/U-2-Net/license --jq '.license'
{ "spdx_id": "Apache-2.0", "name": "Apache License 2.0" }
```

Apache-2.0 is fully compatible with the fork's BSD-3-Clause-Plus-Patent
license under the standard combine-and-redistribute pattern (Apache-2.0
§4 NOTICE attribution required; no copyleft). This is the axis that
unblocks U-2-Net relative to MobileSal — there is no licence
incompatibility.

### Distribution — Google-Drive walled, no GitHub release

The trained checkpoints are not in the repository tree and not
GitHub release artefacts:

- `gh api repos/xuebinqin/U-2-Net/releases` returns `[]` — the
  repo has zero releases.
- A recursive listing of HEAD `ac7e1c81`
  (`gh api 'repos/xuebinqin/U-2-Net/git/trees/master?recursive=1'`)
  returns 217 paths; the only `saved_models/` content is
  `saved_models/face_detection_cv2/haarcascade_frontalface_default.xml`
  (an OpenCV cascade, unrelated). No `*.pth` paths.
- The `README.md` links every checkpoint via Google Drive viewer
  URLs — for `u2netp.pth`:

  > or u2netp.pth (4.7 MB) from
  > [**GoogleDrive**](https://drive.google.com/file/d/...)

  The viewer URL returns Google Drive's HTML preview page, not the
  raw `.pth` — the actual download requires an authenticated browser
  session and a "Download anyway" click for files over Drive's
  unauth-quota threshold.

This is exactly the same distribution-channel blocker that ruled out
MobileSal in [Research-0053 §Distribution](0053-mobilesal-real-weights-blocker.md#distribution-google-drive-walled-no-github-release).
The FastDVDnet pattern of pinning an upstream commit and curling the
weights file by SHA does not reproduce non-interactively in CI.

### Architecture — pure RGB ✓

U-2-Net's forward signature is RGB-only:

```python
def forward(self, x):
    # x: (B, 3, H, W) RGB
    ...
```

(`xuebinqin/U-2-Net/u2net.py`, `U2NETP.forward`.) This unblocks the
RGB-D mismatch axis that ADR-0257 recorded for MobileSal — the
fork's C-side luma-derived RGB contract maps onto U-2-Net's input
without an off-distribution depth-map workaround.

### ONNX op allowlist — `Resize` blocker

`xuebinqin/U-2-Net/u2net.py` builds the U^2 architecture with bilinear
up-sampling at every decoder stage:

```python
src = F.upsample(src, size=tar.shape[2:], mode='bilinear')
```

(Verified by reading `model/u2net.py` at upstream HEAD.) PyTorch's
ONNX exporter lowers `F.upsample(..., mode='bilinear')` to the
`Resize` op (the legacy `Upsample` op was removed in opset 13;
opset 17 emits `Resize` exclusively).

The fork's ONNX op allowlist
(`core/src/dnn/op_allowlist.c`) does **not** include `Resize`:

```text
/* structural / shape */         Identity, Reshape, Flatten, Squeeze,
                                 Unsqueeze, Transpose, Concat, Slice,
                                 Gather, Cast, Shape, Expand
/* arithmetic */                 Add, Sub, Mul, Div, Neg, Abs, Sqrt,
                                 Pow, Exp, Log, Clip, Min, Max, Sum, Mean
/* reductions */                 ReduceMean/Sum/Max/Min,
                                 GlobalAveragePool, GlobalMaxPool
/* dense */                      Gemm, MatMul
/* convolutional */              Conv, ConvTranspose, MaxPool, AveragePool
/* normalization */              BatchNormalization, LayerNormalization,
                                 InstanceNormalization
/* activations */                Relu, LeakyRelu, Sigmoid, Tanh, Softmax,
                                 Elu, Selu, Softplus, Softsign, Gelu, Erf,
                                 HardSigmoid, HardSwish, PRelu, Clip
/* dropout */                    Dropout
/* QDQ */                        QuantizeLinear, DequantizeLinear
/* misc */                       Constant, ConstantOfShape
/* control flow */               Loop, If
```

`Resize` is not on the list. Loading a U-2-Net ONNX with `Resize`
nodes through the fork's `vmaf_dnn_session_open` would be rejected
by the recursive scan in `onnx_scan.c`.

The PixelShuffle decomposition trick from PR #326 (replace
`nn.PixelShuffle(r)` with a `Reshape → Transpose → Reshape` block —
allowlist-safe because PixelShuffle is a *pure shape op* with no
learned parameters and an exact integer-stride decomposition) does
not work for bilinear interpolation. Bilinear resampling requires a
2-tap filter tree at every output pixel; the only ways to express
that with the current allowlist are:

- Pre-compute the bilinear weights into a depth-wise `Conv` with
  fixed kernel shape and stride — works for *fixed* spatial
  dimensions but the upstream `F.upsample(..., size=tar.shape[2:])`
  resolves the target size *dynamically* from the encoder skip
  connection. Static-stride `Conv` cannot replicate that.
- Replace bilinear with nearest-neighbour using `Slice` +
  `Concat` — numerically inequivalent (would need to retrain
  from scratch).
- Use `ConvTranspose` with a stride-2 4×4 bilinear kernel — works
  for 2× upsampling but U-2-Net's decoder uses non-power-of-two
  ratios at the inner stages, and the kernel weights are not the
  upstream-trained ones (off-distribution).

None of these preserve the upstream-trained weights faithfully under
the existing allowlist.

The two reasonable unblocks are:

1. **Widen the allowlist to include `Resize`** under a bounded
   attribute schema. Filed as **T6-2a-widen-allowlist-resize**.
2. **Train a from-scratch saliency student** designed against the
   existing allowlist. Filed as **T6-2a-train-saliency-student**.

Both are independent ADR-scope decisions, neither bundleable into a
"swap weights" PR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Defer u2netp swap; keep placeholder; document blocker** | Honest record of what's actually shipped; corrects ADR-0257's recommendation in light of the second blocker; aligns with the task-brief "don't fake it" directive; zero C-side surface change | T6-2a-followup remains open with no near-term unblock | **Chosen** — see ADR-0265 |
| Vendor `u2netp.pth` via an in-tree authenticated-fetch helper (`gdown`) | Real saliency signal; clean Apache-2.0 license | Adds `gdown` (or equivalent Drive-scraper) to the runtime / CI deps; Google can break the unauth path at any time; CI cannot reproduce the export deterministically; license-clean weights from a fragile distribution channel are still a supply-chain risk; explicitly forbidden by the task brief | Rejected — distribution-channel hardening is a separate scope decision |
| Mirror `u2netp.pth` through the fork's own release artefacts | Stable raw URL the export script can pin; Apache-2.0 §4 NOTICE attribution permits redistribution | Apache-2.0 NOTICE bundling is a release-pipeline change; sets precedent for re-hosting every upstream tiny-AI weight blob in our own releases (storage + audit cost); does not solve the op-allowlist blocker | Filed as **T6-2a-mirror-u2netp-via-release**; blocked behind the op-allowlist decision regardless |
| Widen the ONNX op allowlist to include `Resize` (bilinear-only, fixed-attribute) | Unblocks every model that uses bilinear up-sampling (U-2-Net, U-Net, most decoders); a single security-review pass amortises across future imports | `Resize` has 6+ optional attributes any of which change graph semantics; bounding the attribute set requires fork-local op-validator logic; security review is an independent ADR scope | Filed as **T6-2a-widen-allowlist-resize**; not bundled here because the licence + distribution blocker is independent |
| Rewrite `u2net.py` in-tree against allowlist-safe primitives | Fork-owned graph with verifiable lineage to upstream weights | Bilinear upsampling has no exact decomposition into `{Conv, Reshape, Transpose, Slice}` at dynamic strides; either pre-computes the bilinear-kernel `Conv` (graph blow-up, dynamic stride) or accepts nearest-neighbour (re-trains from scratch) | Rejected — engineering effort comparable to from-scratch retrain, with worse provenance |
| Train a from-scratch saliency student on a permissive corpus (DUTS / DUT-OMRON) | Fully fork-owned weights with clean provenance; sidesteps distribution-channel question; can be designed inside the existing op allowlist from the start | Engineering effort comparable to the rest of T6-2a put together; no in-tree SOD training harness; quality-vs-baseline calibration is a research project on its own | Filed as **T6-2a-train-saliency-student**; deferred until at least one of the other unblocks lands |
| Email `xuebinqin/U-2-Net` author to cut a GitHub release with `u2netp.pth` as an artefact | Cleanest fix for the distribution-channel blocker if granted; doesn't require relicensing (already Apache-2.0) | Out-of-band ask with no commitment; even if granted, the op-allowlist blocker remains | Filed as long-shot follow-up; this PR does not depend on it |
| Use `BASNet` / `PoolNet` / other RGB-only saliency model | Different distribution channel might unblock; might use allowlist-safe ops | Each candidate needs its own license + distribution + architecture audit; survey work effectively starts over | Out-of-scope; revisit only if both U-2-Net unblocks stall |
| Ship u2netp anyway with random `.pth` proxy weights and document it as a placeholder | Pattern matches the original placeholder | Conflates "smoke" and "real weights" — already what `mobilesal_placeholder_v0` is; adds nothing; explicitly forbidden by the task brief | Rejected — duplicate of the existing placeholder |

## Recommendation

**Keep the smoke-only placeholder** shipped at
`model/tiny/mobilesal.onnx` (`mobilesal_placeholder_v0`, sha256
`f1226310…`, `smoke: true`) and document the second-tier blocker in
[ADR-0265](../adr/0265-u2netp-saliency-replacement-blocked.md). The
`mobilesal` extractor remains usable end-to-end — every C-side test
in `core/test/test_mobilesal.c` passes against the placeholder —
but `saliency_mean` stays a content-independent constant (~0.5)
until at least one of the unblock paths lands. The C contract does
not change; any future drop-in (U-2-Net via mirror + allowlist
widening, distilled student, or BASNet/PoolNet survey result) just
replaces the `.onnx` and bumps the registry sha256.

Of the three filed follow-ups, `T6-2a-widen-allowlist-resize` is
the load-bearing one — both the U-2-Net mirror path and any future
modern-decoder import depend on it. Recommend prioritising it
before another saliency-replacement attempt.

## References

- [ADR-0218](../adr/0218-mobilesal-saliency-extractor.md) — original
  MobileSal extractor design with the smoke-only placeholder.
- [ADR-0257](../adr/0257-mobilesal-real-weights-deferred.md) /
  [Research-0053](0053-mobilesal-real-weights-blocker.md) (PR #328)
  — sibling MobileSal blocker; this digest extends the chain.
- [ADR-0253](../adr/0253-fastdvdnet-pre-real-weights.md) (PR #326)
  — sibling real-weights swap that *did* succeed (FastDVDnet, MIT,
  GitHub-raw downloadable, RGB-only architecture). The pattern this
  digest was supposed to mirror.
- Upstream code: <https://github.com/xuebinqin/U-2-Net> (HEAD
  `ac7e1c81`, SPDX = Apache-2.0).
- Upstream paper: Qin, Zhang, Huang, Dehghan, Zaiane, Jagersand,
  *"U^2-Net: Going Deeper with Nested U-Structure for Salient
  Object Detection"*, Pattern Recognition 2020.
- ONNX `Resize` op spec:
  <https://onnx.ai/onnx/operators/onnx__Resize.html>.
- `core/src/dnn/op_allowlist.c` — the canonical allowlist
  enumeration this digest audited against.
- Source: paraphrased — task brief directive "if u2netp weights
  download fails, ship a docs-only blocker PR similar to #328's
  mobilesal pattern, documenting what's needed. Don't push fake
  weights."
