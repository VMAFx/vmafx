# `u2netp_mirror` — model card (exporter ready; binary upload pending)

> **Status — exporter ready; signed release asset pending.** Per
> [ADR-0412](../../adr/0412-u2netp-fork-mirror-scaffold.md) and
> [ADR-0671](../../adr/0671-u2netp-mirror-exporter.md), the fork has
> an operator exporter for the upstream U-2-Net `u2netp` checkpoint.
> The generated binary itself (`model/u2netp_mirror.onnx`) remains
> gitignored and lands as a signed release asset. Until runtime
> promotion is measured, **the recommended weights for the `mobilesal`
> extractor remain [`saliency_student_v2`](saliency_student_v2.md)**.

This card follows the 5-point bar of
[ADR-0042](../../adr/0042-tinyai-docs-required-per-pr.md):
provenance, training recipe, op-allowlist coverage, deployment
contract, licence-compliance receipt.

## 1. Provenance

| Field                | Value                                                                |
| -------------------- | -------------------------------------------------------------------- |
| Upstream repository  | <https://github.com/xuebinqin/U-2-Net>                               |
| Upstream commit pin  | `ac7e1c81` (re-pinned at binary upload time to the audited HEAD)     |
| Upstream paper       | Qin et al., *U^2-Net*, Pattern Recognition 2020                      |
| Upstream model file  | `u2netp.pth` (~4.7 MB)                                               |
| Upstream license     | Apache-2.0 (no NOTICE file in upstream tree as of HEAD `ac7e1c81`)   |
| Fork release tag     | `u2netp-mirror-v1` (ADR-0412 scaffold scheme)                       |
| Fork artefact path   | `model/u2netp_mirror.onnx` (gitignored; release attachment only)     |
| Fork artefact sha256 | _to be filled at binary upload_ — first recorded by
                        `model/u2netp_mirror.manifest.json`, then written into
                        `model/tiny/registry.json` if/when a registry follow-up lands |
| Sigstore bundle URL  | _to be filled at binary upload_ — emitted by
                        `.github/workflows/release-please.yml`'s `u2netp-mirror-attach` step |

## 2. Training recipe

The fork does **not** re-train this model. The mirrored checkpoint
is the upstream `u2netp` weights byte-identical to the upstream
Google Drive download. `ai/scripts/export_u2netp_mirror.py` imports
an audited local U-2-Net checkout, loads `u2netp.pth`, selects the
upstream `d0` saliency output, and exports ONNX opset 17 as
`input` -> `saliency_map`. The upstream training recipe (per the
U-2-Net paper) is:

- Backbone: `U^2-Net` (nested U-structure) — small variant
  (`U2NETP`) with ~4.7 M parameters.
- Dataset: DUTS-TR (10 553 images).
- Loss: BCE + IoU multi-supervision over 7 saliency heads.
- Optimiser: Adam, lr `1e-3`, batch size `12`, 600 K iterations.
- Image size: 320×320 with random crop / horizontal flip.

For a fork-trained alternative on the same DUTS-TR corpus see
[`saliency_student_v2`](saliency_student_v2.md) — that model is
smaller, fork-owned, and ships under BSD-3-Clause-Plus-Patent
rather than Apache-2.0.

## 3. ONNX op-allowlist coverage

Upstream `u2netp` uses the following ONNX ops (after
`torch.onnx.export` at opset 17):

| Op             | On `op_allowlist.c`?              | Notes |
| -------------- | --------------------------------- | --- |
| `Conv`         | yes                               |
| `BatchNormalization` | yes                         |
| `Relu`         | yes                               |
| `MaxPool`      | yes                               |
| `Concat`       | yes                               |
| `Sigmoid`      | yes                               |
| `Resize`       | yes (added by [ADR-0258](../../adr/0258-onnx-allowlist-resize.md)) | Bilinear `F.upsample` lowers here. Was the axis-2 blocker in [ADR-0265](../../adr/0265-u2netp-saliency-replacement-blocked.md); resolved. |

Coverage at opset 17 is full — the converted ONNX graph loads
unchanged against the fork's wire-format scanner.

## 4. Deployment contract

- **Input tensor**: `input` `[1, 3, H, W]` `float32` (per
  upstream's `U2NETP.forward` signature).
- **Output tensor**: upstream emits 7 saliency heads
  (`d0..d6`); the rewrap selects `d0` (the highest-resolution
  multi-scale fusion) and renames it to `saliency_map`
  `[1, 1, H, W]`. This matches the C-side `feature_mobilesal.c`
  contract, so the mirror is a drop-in for the existing
  `mobilesal` extractor with no C changes.
- **Pre-processing**: tile luma into RGB (`Y → [Y, Y, Y]`),
  same as the existing `LumaAdapter` pattern from PR #326.
- **Post-processing**: take the per-frame mean of
  `saliency_map` for `saliency_mean` (no change from existing
  C extractor logic).

The deployment contract is intentionally identical to
`saliency_student_v2`'s — both models can be swapped in by
flipping the registry entry, no C-side rebuild needed.

## 5. Licence-compliance receipt

Apache-2.0 §4 (a)–(d) are addressed as follows (full walk in
[Research-0086](../../research/0086-u2netp-fork-mirror-license-compliance.md)):

- **§4 (a)** — full Apache-2.0 text ships at
  [`LICENSES/Apache-2.0-u2netp.txt`](../../../LICENSES/Apache-2.0-u2netp.txt)
  and is uploaded alongside the binary in every release that
  carries the mirror.
- **§4 (b)** — applies only to the ONNX rewrap (derivative
  work). The export script writes a `metadata_props` block on
  the ONNX graph stating "derived from xuebinqin/U-2-Net @
  <sha>; converted via torch.onnx.export opset 17; weights
  byte-identical to upstream `u2netp.pth`". Verbatim `.pth`
  redistribution is **not** a derivative-work modification, so
  §4 (b) is moot in that case.
- **§4 (c)** — attribution block in
  [`LICENSES/Apache-2.0-u2netp.txt`](../../../LICENSES/Apache-2.0-u2netp.txt)
  cites upstream copyright, paper, repository, and commit pin.
- **§4 (d)** — moot. Upstream tree carries no `NOTICE` file
  (verified against HEAD `ac7e1c81`).

The fork's own redistribution metadata (model card, registry
sidecar) does **not** modify the Apache-2.0 license; it is
"additional attribution notices" per §4 (d) ("…provided that
such additional attribution notices cannot be construed as
modifying the License").

## 6. When to use this

Prefer [`saliency_student_v2`](saliency_student_v2.md) by default.
It is fork-owned, much smaller, ships under the same license as the
rest of `model/tiny/`, and was trained on the same DUTS-TR corpus
the upstream u2netp paper used.

Reach for `u2netp_mirror` when one of these applies:

- You are reproducing a published baseline that explicitly
  cites the upstream `u2netp` checkpoint and need
  byte-identical weights for citation.
- You are running a comparative evaluation where the
  upstream-trained signal is the ground truth and the
  fork-trained student is the candidate under test.
- Your downstream pipeline already pins to upstream u2netp
  behaviour (e.g. saliency masks were generated against
  upstream u2netp and you need the matching encoder hook).

In every case, run both models against your own validation set
before committing — the absolute scores differ (the upstream
model has ~40× more parameters; expect mIoU and saliency
distribution differences).

## 7. Operator workflow

The operator-facing fetch + verification recipe lives at
[`docs/ai/u2netp-mirror.md`](../u2netp-mirror.md). Short
version:

```bash
.venv/bin/python ai/scripts/export_u2netp_mirror.py \
  --upstream-dir /path/to/U-2-Net \
  --checkpoint /path/to/u2netp.pth \
  --output model/u2netp_mirror.onnx \
  --manifest-out model/u2netp_mirror.manifest.json

gh release download <tag> --repo VMAFx/vmafx \
  --pattern 'u2netp_mirror_v*.onnx' \
  --pattern 'u2netp_mirror_v*.onnx.bundle' \
  --pattern 'Apache-2.0-u2netp.txt'

cosign verify-blob \
  --bundle u2netp_mirror_v1.onnx.bundle \
  --certificate-identity-regexp '^https://github\.com/VMAFx/vmafx' \
  --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' \
  u2netp_mirror_v1.onnx
```

The verify-blob step is mandatory before the binary is loaded by
any production pipeline — it gates on Sigstore's keyless OIDC
identity (`VMAFx/vmafx` workflow + GitHub OIDC issuer), so a
tampered or wrong-origin binary fails verification.

## 8. Release / promotion follow-ups

The exporter writes the local ONNX and manifest, but release and
runtime promotion are still separate steps:

1. Attach the ONNX, Sigstore bundle, provenance manifest, and
   [`LICENSES/Apache-2.0-u2netp.txt`](../../../LICENSES/Apache-2.0-u2netp.txt)
   to the release asset set.
2. Run saliency/ROI materializer comparisons against refreshed
   tables.
3. Only after measured benefit, add `u2netp_mirror_v1` as an
   alternative registry entry. Do not flip the default in the same
   PR as the asset upload.

## References

- [ADR-0412](../../adr/0412-u2netp-fork-mirror-scaffold.md) —
  the scaffold decision this card documents.
- [ADR-0671](../../adr/0671-u2netp-mirror-exporter.md) — the
  exporter implementation.
- [ADR-0265](../../adr/0265-u2netp-saliency-replacement-blocked.md)
  — the blocker decision this scaffold partially unblocks.
- [ADR-0286](../../adr/0286-saliency-student-fork-trained-on-duts.md)
  — the recommended primary path (`saliency_student_v1`).
- [ADR-0258](../../adr/0258-onnx-allowlist-resize.md) —
  `Resize` allowlist addition that resolves ADR-0265's axis-2
  blocker.
- [Research-0086](../../research/0086-u2netp-fork-mirror-license-compliance.md)
  — full compliance walk + alternatives table.
- Upstream paper: Qin et al., *U^2-Net*, Pattern Recognition
  2020.
- Upstream code: <https://github.com/xuebinqin/U-2-Net>.
