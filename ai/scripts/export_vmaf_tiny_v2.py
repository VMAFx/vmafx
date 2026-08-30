#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Export ``vmaf_tiny_v2`` to a self-contained ONNX file.

The exported graph bundles the StandardScaler statistics
(``mean``, ``std``) as Constant nodes that run *before* the MLP, so
the runtime simply feeds raw canonical-6 feature values and the model
emits a calibrated VMAF estimate. This satisfies the bundled-scaler
requirement: training-time mean+std are baked into the graph and
do not need to be shipped out-of-band.

Inputs / outputs
~~~~~~~~~~~~~~~~

* Input  ``features`` — float32 tensor of shape ``[N, 6]``
  (``N`` is dynamic, the 6 canonical features in the order
  ``adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2``).
* Output ``vmaf``    — float32 tensor of shape ``[N]``.

Runtime topology after export::

    features [N, 6]
        |
        Sub  <- mean   ([6])
        |
        Div  <- std    ([6])
        |
        MLP (Linear(6,16) → ReLU → Linear(16,8) → ReLU → Linear(8,1))
        |
        Squeeze (axis=-1) -> vmaf [N]

opset_version is pinned to 17 to match the sister tiny-AI models
(``learned_filter_v1``, ``nr_metric_v1``, ``fastdvdnet_pre``).
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

import numpy as np

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.file_utils import sha256  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

OPSET = 17


def _build_mlp_small(in_dim: int):  # type: ignore[no-untyped-def]
    from torch import nn

    return nn.Sequential(
        nn.Linear(in_dim, 16),
        nn.ReLU(),
        nn.Linear(16, 8),
        nn.ReLU(),
        nn.Linear(8, 1),
    )


class _BundledScalerMLP:
    """Pure-PyTorch wrapper that prepends ``(x - mean) / std`` to the MLP.

    ``torch.onnx.export`` traces the wrapper to a single graph in
    which ``mean`` / ``std`` show up as Constant initialisers, so the
    runtime needs no out-of-band scaler stats — the trust-root sha256
    covers the calibration values too.
    """

    def __new__(cls, mlp, mean, std):  # type: ignore[no-untyped-def]
        import torch
        from torch import nn

        class _Wrap(nn.Module):
            def __init__(self) -> None:
                super().__init__()
                self.mlp = mlp
                self.register_buffer("mean", torch.from_numpy(mean.astype(np.float32)))
                self.register_buffer("std", torch.from_numpy(std.astype(np.float32)))

            def forward(self, features):  # type: ignore[no-untyped-def]
                # broadcast (N, 6) - (6,) and / (6,)
                normed = (features - self.mean) / self.std
                out = self.mlp(normed)
                return out.squeeze(-1)

        return _Wrap().eval()


def _write_sidecar(
    *,
    sidecar_path: Path,
    onnx_name: str,
    digest: str,
    in_dim: int,
    features: list[str],
    mean: np.ndarray,
    std: np.ndarray,
    run_provenance: dict[str, Any],
) -> dict[str, Any]:
    sidecar = {
        "id": "vmaf_tiny_v2",
        "kind": "fr",
        "onnx": onnx_name,
        "opset": OPSET,
        "sha256": digest,
        "input_name": "features",
        "input_shape": [-1, in_dim],
        "output_name": "vmaf",
        "output_shape": [-1],
        "features": list(features),
        "input_mean": mean.tolist(),
        "input_std": std.tolist(),
        "notes": (
            "vmaf_tiny_v2 — canonical-6 + StandardScaler + mlp_small "
            "(257 params), 90 epochs Adam @ lr=1e-3, MSE. Scaler "
            "(mean, std) baked into the ONNX graph as Constant nodes "
            "so the runtime feeds raw feature values. Validated PLCC "
            "0.9978 ± 0.0021 on Netflix LOSO; 0.9998 on KoNViD 5-fold."
        ),
        "run_provenance": run_provenance,
    }
    write_manifest_json(sidecar_path, sidecar)
    return sidecar


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    ap = make_argument_parser(prog="export_vmaf_tiny_v2.py", description=__doc__)
    ap.add_argument(
        "--ckpt",
        type=Path,
        required=True,
        help="Trained checkpoint produced by train_vmaf_tiny_v2.py.",
    )
    ap.add_argument(
        "--out-onnx",
        type=Path,
        required=True,
        help="Destination ONNX file (typically model/tiny/vmaf_tiny_v2.onnx).",
    )
    ap.add_argument(
        "--out-sidecar",
        type=Path,
        required=True,
        help="Sidecar JSON (input/output names + opset, mirrors v1 format).",
    )
    args = ap.parse_args(raw_argv)

    import torch

    # nosec B614: weights_only=False is required because the .pt also stores
    # scaler stats + train metrics in plain Python types; weights_only=True
    # rejects those with UnpicklingError. Trust boundary: the checkpoint is
    # produced by our own train_predictor_v2.py run under runs/ — path is a
    # required CLI arg from the developer, not network input.
    state = torch.load(args.ckpt, map_location="cpu", weights_only=False)  # nosec B614
    features = state["features"]
    in_dim = len(features)
    mean = np.asarray(state["input_mean"], dtype=np.float64)
    std = np.asarray(state["input_std"], dtype=np.float64)

    if in_dim != 6:
        print(f"[export-v2] expected 6 features, got {in_dim}", file=sys.stderr)
        return 2

    mlp = _build_mlp_small(in_dim)
    mlp.load_state_dict(state["state_dict"])
    mlp.eval()

    wrapper = _BundledScalerMLP(mlp, mean, std)

    dummy = torch.zeros(1, in_dim, dtype=torch.float32)
    args.out_onnx.parent.mkdir(parents=True, exist_ok=True)
    print(f"[export-v2] tracing wrapper -> {args.out_onnx} (opset={OPSET})")
    torch.onnx.export(
        wrapper,
        (dummy,),
        str(args.out_onnx),
        input_names=["features"],
        output_names=["vmaf"],
        dynamic_axes={"features": {0: "N"}, "vmaf": {0: "N"}},
        opset_version=OPSET,
        do_constant_folding=True,
    )

    # Force inline storage so the sha256 covers the entire model.
    import onnx

    proto = onnx.load(str(args.out_onnx))
    onnx.save(proto, str(args.out_onnx), save_as_external_data=False)
    sidecar_data = args.out_onnx.with_suffix(".onnx.data")
    if sidecar_data.exists():
        sidecar_data.unlink()

    digest = sha256(args.out_onnx)
    print(f"[export-v2] sha256={digest}")
    print(f"[export-v2] size  ={args.out_onnx.stat().st_size} bytes")

    run_provenance = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={"checkpoint": args.ckpt},
        outputs={
            "onnx_target": args.out_onnx,
            "sidecar_target": str(args.out_sidecar),
        },
    )
    _write_sidecar(
        sidecar_path=args.out_sidecar,
        onnx_name=args.out_onnx.name,
        digest=digest,
        in_dim=in_dim,
        features=list(features),
        mean=mean,
        std=std,
        run_provenance=run_provenance,
    )
    print(f"[export-v2] wrote {args.out_sidecar}")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
