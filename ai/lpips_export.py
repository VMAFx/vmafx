#!/usr/bin/env python3
#
#  Copyright 2026 Lusoris
#  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
"""Export LPIPS-SqueezeNet as a two-input ONNX for libvmaf's tiny-AI surface.

Output contract
---------------
The exported graph accepts **ImageNet-normalised** inputs (mean=[0.485,
0.456, 0.406], std=[0.229, 0.224, 0.225]) because that matches
``vmaf_tensor_from_rgb_imagenet()`` on the C side. Internally the graph
inverts that normalisation and reapplies LPIPS's own ScalingLayer, so
downstream code stays agnostic of LPIPS's unusual input convention.

  inputs:
    ref  float32[1, 3, H, W]   ImageNet-normalised
    dist float32[1, 3, H, W]   ImageNet-normalised

  output:
    score float32[1]           LPIPS distance, lower = more similar

Dynamic H, W — LPIPS-SqueezeNet is fully convolutional + spatially pools.

Determinism
-----------
Torch is seeded, the model is frozen (.eval + requires_grad_(False)),
weights come from the pinned richzhang/PerceptualSimilarity release, and
the final ONNX goes through ``onnx.save_model`` with no
``ModelProto.doc_string`` or trainer metadata, so the bytes — and their
sha256 — are reproducible across runs on the same torch+onnx versions.

Usage
-----
    python ai/lpips_export.py \\
        --output model/tiny/lpips_sq.onnx \\
        --opset 17

Parity check against Torch reference is run automatically; fails with a
non-zero exit if atol>1e-4.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import onnx
import torch
import torch.nn as nn

IMAGENET_MEAN = (0.485, 0.456, 0.406)
IMAGENET_STD = (0.229, 0.224, 0.225)


class _LpipsImagenetWrapper(nn.Module):
    """Wrap ``lpips.LPIPS`` so the graph accepts ImageNet-normalised inputs.

    Pipeline: imagenet-normalised → [0,1] → [-1,1] → LPIPS internals.
    The C side only has to produce ImageNet-normalised tensors (it does,
    via :c:func:`vmaf_tensor_from_rgb_imagenet`).
    """

    def __init__(self, net: str = "squeeze") -> None:
        super().__init__()
        import lpips  # imported lazily: heavy + only needed here

        self.core = lpips.LPIPS(net=net, spatial=False, verbose=False)
        self.core.eval()
        for p in self.core.parameters():
            p.requires_grad_(False)

        mean = torch.tensor(IMAGENET_MEAN, dtype=torch.float32).view(1, 3, 1, 1)
        std = torch.tensor(IMAGENET_STD, dtype=torch.float32).view(1, 3, 1, 1)
        # buffers — baked into the exported graph, not trainable
        self.register_buffer("in_mean", mean)
        self.register_buffer("in_std", std)

    def _denorm(self, x: torch.Tensor) -> torch.Tensor:
        # imagenet-normalised → [0,1] → [-1,1]
        x01 = x * self.in_std + self.in_mean
        return x01 * 2.0 - 1.0

    def forward(self, ref: torch.Tensor, dist: torch.Tensor) -> torch.Tensor:
        ref_m11 = self._denorm(ref)
        dist_m11 = self._denorm(dist)
        # LPIPS returns [N,1,1,1] — squeeze trailing dims to a scalar per item.
        d = self.core(ref_m11, dist_m11, normalize=False, retPerLayer=False)
        return d.reshape(-1)


def _export(output: Path, opset: int) -> int:
    """Export the model. Returns the actual opset version in the written graph.

    Torch's dynamo exporter may emit a newer opset than requested when it
    cannot downconvert. We detect the effective opset after export so the
    sidecar reflects reality.
    """
    torch.manual_seed(0)
    model = _LpipsImagenetWrapper(net="squeeze")
    model.eval()

    # Small dummy tensors — the ONNX graph is dynamic in H, W so the
    # specific spatial size here only affects the trace, not the model.
    dummy_ref = torch.zeros(1, 3, 64, 64, dtype=torch.float32)
    dummy_dist = torch.zeros(1, 3, 64, 64, dtype=torch.float32)

    dynamic_axes = {
        "ref": {2: "H", 3: "W"},
        "dist": {2: "H", 3: "W"},
    }

    tmp = output.with_suffix(".onnx.tmp")
    torch.onnx.export(
        model,
        (dummy_ref, dummy_dist),
        tmp.as_posix(),
        input_names=["ref", "dist"],
        output_names=["score"],
        opset_version=opset,
        dynamic_axes=dynamic_axes,
        do_constant_folding=True,
    )

    # Strip non-deterministic metadata for reproducible sha256.
    proto = onnx.load(tmp.as_posix())
    proto.doc_string = ""
    proto.producer_name = "vmaf-tiny-export"
    proto.producer_version = ""
    proto.domain = ""
    for entry in list(proto.metadata_props):
        proto.metadata_props.remove(entry)
    onnx.checker.check_model(proto)
    onnx.save_model(proto, output.as_posix())
    tmp.unlink(missing_ok=True)
    # Torch's dynamo exporter may write sidecar external-data blobs (.data);
    # after re-saving with everything inlined, they're dead weight.
    for stray in tmp.parent.glob(tmp.name + "*.data"):
        stray.unlink(missing_ok=True)

    # The default (ai.onnx) domain is "".
    effective_opset = next(
        (o.version for o in proto.opset_import if o.domain in ("", "ai.onnx")),
        opset,
    )
    return effective_opset


def _parity_check(onnx_path: Path, atol: float = 1e-4) -> None:
    """Roundtrip a random input pair through Torch vs. ONNX Runtime."""
    try:
        import onnxruntime as ort
    except ImportError:
        print("[warn] onnxruntime not installed — skipping parity check", file=sys.stderr)
        return

    torch.manual_seed(1)
    ref01 = torch.rand(1, 3, 96, 128)
    dist01 = torch.rand(1, 3, 96, 128)
    mean = torch.tensor(IMAGENET_MEAN).view(1, 3, 1, 1)
    std = torch.tensor(IMAGENET_STD).view(1, 3, 1, 1)
    ref_in = (ref01 - mean) / std
    dist_in = (dist01 - mean) / std

    model = _LpipsImagenetWrapper(net="squeeze")
    model.eval()
    with torch.no_grad():
        torch_out = model(ref_in, dist_in).cpu().numpy()

    sess = ort.InferenceSession(onnx_path.as_posix(), providers=["CPUExecutionProvider"])
    ort_out = sess.run(None, {"ref": ref_in.numpy(), "dist": dist_in.numpy()})[0]

    diff = float(np.max(np.abs(torch_out - ort_out)))
    if diff > atol:
        raise SystemExit(f"parity check failed: max|Δ|={diff:.3e} > atol={atol:.1e}")
    print(f"[ok] parity: max|Δ|={diff:.3e} ≤ atol={atol:.1e}")


def _write_sidecar(
    onnx_path: Path,
    opset: int,
    *,
    sidecar_path: Path | None = None,
    run_provenance: dict | None = None,
) -> Path:
    sidecar = sidecar_path if sidecar_path is not None else onnx_path.with_suffix(".json")
    payload: dict = {
        "input_name": "ref",
        "kind": "fr",
        "name": "vmaf_tiny_lpips_sq_v1",
        "notes": (
            "LPIPS-SqueezeNet (richzhang/PerceptualSimilarity v0.1). Inputs are "
            "ImageNet-normalised RGB planes, layout NCHW [1,3,H,W]. Output is a "
            "scalar LPIPS distance (lower = more similar). Two-input model — use "
            "vmaf_dnn_session_run() with named bindings 'ref' and 'dist'."
        ),
        "onnx_opset": opset,
        "output_name": "score",
    }
    if run_provenance is not None:
        payload["run_provenance"] = run_provenance
    sidecar.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    return sidecar


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv: list[str] | None = None) -> int:
    script_path = Path(__file__).resolve()
    repo_root = script_path.parent.parent
    default_out = repo_root / "model" / "tiny" / "lpips_sq.onnx"

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", "--out", dest="output", type=Path, default=default_out)
    parser.add_argument(
        "--sidecar",
        type=Path,
        default=None,
        help="Path for the JSON sidecar (default: <output>.json).",
    )
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument("--skip-parity", action="store_true")
    args = parser.parse_args(argv)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    effective_opset = _export(args.output, args.opset)
    if effective_opset != args.opset:
        print(
            f"[info] requested opset={args.opset}, exporter emitted opset={effective_opset}; "
            "sidecar + registry should use the emitted value",
            file=sys.stderr,
        )
    # Build run_provenance lazily so aiutils import errors surface only when
    # the sidecar is actually written (keeps the module importable without aiutils).
    run_provenance: dict | None = None
    try:
        _ai_src = str(script_path.parent / "src")
        if _ai_src not in sys.path:
            sys.path.insert(0, _ai_src)
        from aiutils.run_manifest import build_run_provenance

        run_provenance = build_run_provenance(
            entrypoint=script_path,
            repo_root=repo_root,
            argv=sys.argv[1:] if argv is None else list(argv),
            args=vars(args),
            outputs={"onnx": args.output},
        )
    except ImportError:
        pass
    _write_sidecar(
        args.output,
        effective_opset,
        sidecar_path=args.sidecar,
        run_provenance=run_provenance,
    )
    if not args.skip_parity:
        _parity_check(args.output)

    sha = _sha256(args.output)
    size = args.output.stat().st_size
    try:
        display_path = args.output.relative_to(repo_root)
    except ValueError:
        display_path = args.output
    print(f"[ok] wrote {display_path} ({size} bytes)")
    print(f"     sha256 {sha}")
    print("     add this digest to model/tiny/registry.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
