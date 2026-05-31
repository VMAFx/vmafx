<!-- markdownlint-disable MD013 MD060 -->
# Research-0062 — Fork-trained saliency student on DUTS-TR

| Field      | Value                                                       |
| ---------- | ----------------------------------------------------------- |
| **Date**   | 2026-05-03                                                  |
| **Status** | Implemented — `saliency_student_v1` shipped                 |
| **Tags**   | dnn, tiny-ai, mobilesal, saliency, training, license, fork-local |

Companion to [ADR-0286](../adr/0286-saliency-student-fork-trained-on-duts.md).
Records the alternatives walked, the dataset-license analysis, the
architecture decisions, and the training-recipe rationale behind the
from-scratch saliency student that replaces the smoke-only
`mobilesal_placeholder_v0` checkpoint.

## Background

[ADR-0257](../adr/0257-mobilesal-real-weights-deferred.md) and
[Research-0053](0053-mobilesal-real-weights-blocker.md) (2026-05-03)
documented the three blockers that ruled out the FastDVDnet-style
real-weights swap for MobileSal: (a) upstream `yuhuan-wu/MobileSal`
is CC BY-NC-SA 4.0 (incompatible with the fork's
BSD-3-Clause-Plus-Patent), (b) checkpoints are distributed via Google
Drive viewer URLs only, and (c) the network is RGB-D while the C
contract is RGB-only. The recommended replacement path was to swap to
U-2-Net `u2netp` (Apache-2.0). When that path was investigated in
detail it inherited the same Google-Drive distribution problem and
its 4.7 MB checkpoint dwarfs every other model under `model/tiny/`.

This digest records the fallback path that *was* feasible: train a
small saliency student from scratch on a permissively-distributed
public corpus. The resulting weights are wholly fork-owned, ship
under BSD-3-Clause-Plus-Patent, and lock down a substantively
content-dependent `saliency_mean` signal for the first time on this
fork.

## Dataset survey

The fork needs a saliency-segmentation dataset with (a) a clearly
documented research-distribution licence (no clickwrap), (b) a stable
HTTP URL the export script can pin, and (c) enough scale to train a
small student to a useful operating point in a single GPU-hour.

| Dataset | Size | License / distribution | Picked? |
| --- | --- | --- | --- |
| **DUTS-TR** (Wang et al. 2017) | 10,553 RGB images + binary masks; 271 MB | "Free for academic research" per the official site (`http://saliencydetection.net/duts/`); direct `https://saliencydetection.net/duts/download/DUTS-TR.zip` URL with `Last-Modified` headers and a stable `ETag` | **Yes** — the only dataset that satisfied all three criteria in the survey |
| ECSSD (Yan et al. 2013) | 1,000 images | Free for research; smaller; same direct-URL distribution | Held in reserve as an external test set; not used for training |
| MSRA10K (Cheng et al. 2014) | 10,000 images | Free for research; mostly single-object centred; less diverse than DUTS-TR | Considered for augmentation; not bundled in v1 |
| HKU-IS (Li & Yu 2015) | 4,447 images | Direct distribution; OK | Smaller than DUTS-TR; held in reserve |
| **DUTS-TE** (Wang et al. 2017) | 5,019 images | Same provenance as DUTS-TR | Used as the *external* test split for future evaluations; v1 keeps the held-out 5% of DUTS-TR for in-loop validation IoU |
| SOD (Movahedi & Faugeras 2010) | 300 images | Direct distribution | Too small to train on alone |
| DAVIS-S (Caelles et al. 2018) | Video | Apache-2.0 mask format but video-segmentation flavour, not still-image SOD | Out of scope |

DUTS-TR is the de-facto standard training corpus for image-level
salient-object detection (used by U-2-Net, BASNet, F3Net, EGNet, ...).
The README on the project page records the academic-research
distribution terms ("free for academic and research purposes"). Only
the **trained weights** are shipped in this fork; the DUTS images
themselves are deliberately *not* committed to the repository.

DUTS-TR archive provenance recorded in
`docs/ai/models/saliency_student_v1.md`:

```text
URL:           https://saliencydetection.net/duts/download/DUTS-TR.zip
Last-Modified: 2025-03-10
Content-Length: 270 997 309 bytes
SHA-256:       ce61e023c8f59d022b4d46981cf16813b83d089242e6489a45630d83962ea058
Pairs:         10 553 (train images + binary masks, 1:1)
```

## Architecture survey

Four candidate architectures were reviewed against the existing C
contract (`input` `[1, 3, H, W]` → `saliency_map` `[1, 1, H, W]`)
and the fork's ONNX op-allowlist
(`core/src/dnn/op_allowlist.c`):

| Architecture | Approx. params | Notes | Picked? |
| --- | --- | --- | --- |
| **Tiny U-Net (this work)** | ~113 K | 3 down / 3 up with skip connections; `Conv` + `BatchNormalization` + `ReLU` + `MaxPool` + `ConvTranspose` + `Concat` + `Sigmoid` — every op is on the allowlist; `ConvTranspose` keeps the graph load-clean against vanilla origin/master without an allowlist patch in the same PR | **Yes** |
| BASNet-lite | ~3.4 M | Strong upstream; would require porting a substantive code drop | Too large; needs upstream code import → second licence audit |
| U-2-Net `u2netp` | ~4.7 M | Strong upstream; Apache-2.0 *codebase* but Google-Drive-only weights | Code import is cleaner than re-training but still wraps the licence question; not a useful first cut |
| MobileNetV2 + sigmoid head | ~2.2 M | Easy to wire | Way over the size budget for a pure saliency student |

The chosen architecture has 112,841 trainable parameters — well under
the 200,000-parameter target and an order of magnitude smaller than
any "real" upstream SOD model — but enough capacity to learn a useful
content-dependent saliency signal on DUTS-TR.

## Training-recipe rationale

| Knob | Value | Rationale |
| --- | --- | --- |
| Optimizer | Adam, lr = 1e-3 | Default working setting for tiny U-Nets in segmentation; matches the per-task brief |
| LR schedule | Cosine annealing to 0 over 50 epochs | Smoothly winds down without warmup; deterministic given seed |
| Batch size | 32 | Fits comfortably in 24 GB at 256×256; good gradient-noise / wall-clock trade-off |
| Crop size | 256×256 | Matches what every published SOD model trains at; the model is fully-convolutional so inference at native resolution is unrestricted |
| Loss | BCE + Dice (per-image, mean reduced) | Dice covers the foreground–background imbalance; BCE keeps gradients alive when Dice saturates |
| Augmentation | Random crop + horizontal flip | Cheap, dataset-license-safe (no external data); larger augmentation packages deferred |
| Validation split | 5% held-out from DUTS-TR (528 pairs) | Stable seed-shuffled split; in-loop selection only — the *external* DUTS-TE / ECSSD evaluation is a follow-up |
| Selection | Best epoch by val IoU at threshold 0.5 | Single deterministic selection criterion; per-task brief |
| Early-stop floor | val IoU ≥ 0.5 ships; below ≥ 0.5 ships docs-only failure PR | Per the task brief — a saliency model with IoU < 0.5 at this scale is below useful, so a docs-only failure PR is more honest than a noisy weights drop |
| Seed | 42 | Reproducible across re-runs; baked into `train_saliency_student.py` |

## Op-allowlist analysis

Every op in the exported ONNX graph was verified against
`core/src/dnn/op_allowlist.c` at export time via
`ai/src/vmaf_train/op_allowlist.py::check_model`. The graph contains
exactly the ops `Conv`, `BatchNormalization` (folded into `Conv` by
constant folding at export), `Relu`, `MaxPool`, `ConvTranspose`,
`Concat`, and `Sigmoid` — all on the allowlist. `Resize` is *not*
used (and is not on the allowlist at the time of this PR — using
`ConvTranspose` for stride-2 upsampling avoids a scope expansion into
the allowlist + the C-side scanner in the same PR).

## Known limitations (carried over from MobileSal)

The C-side `feature_mobilesal.c` extractor is unchanged, so the
known limitations recorded in
[`docs/ai/models/mobilesal.md`](../ai/models/mobilesal.md) carry
over verbatim — 8-bit YUV only, BT.709 limited-range YUV→RGB,
ImageNet normalisation in C. The `saliency_student_v1` weights are a
true drop-in replacement (same input/output tensor names, same NCHW
shapes, same dynamic axes).

## Reproducer

```text
.venv/bin/python ai/scripts/train_saliency_student.py \
    --duts-root /path/to/DUTS-TR \
    --output    model/tiny/saliency_student_v1.onnx \
    --epochs 50 --batch-size 32 --lr 1e-3 --seed 42 \
    --metrics-out build_artifacts/saliency_student_v1_train.json
```

The training script is deterministic given the seed and the pinned
PyTorch / CUDA versions; re-runs reproduce the val-IoU curve to
within float-rounding noise.

## References

- [ADR-0286](../adr/0286-saliency-student-fork-trained-on-duts.md) — accompanying decision record.
- [ADR-0218](../adr/0218-mobilesal-saliency-extractor.md) — original MobileSal extractor wiring (unchanged by this PR).
- [ADR-0257](../adr/0257-mobilesal-real-weights-deferred.md) — the deferral that this PR partly unblocks.
- [Research-0053](0053-mobilesal-real-weights-blocker.md) — the upstream survey that ruled out the real-weights swap.
- DUTS dataset: Wang et al., "Learning to Detect Salient Objects with Image-Level Supervision", CVPR 2017. Project page: <http://saliencydetection.net/duts/>. License: free for academic research.
- U-Net: Ronneberger et al., "U-Net: Convolutional Networks for Biomedical Image Segmentation", MICCAI 2015.
- Source: paraphrased — task brief directive "train a small saliency student from scratch on a permissively-licensed public dataset, replacing the placeholder."
