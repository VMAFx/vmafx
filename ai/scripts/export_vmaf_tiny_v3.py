#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Export ``vmaf_tiny_v3`` to a self-contained ONNX file.

Mirrors ``export_vmaf_tiny_v2.py`` exactly except for the architecture
factory — the wrapper, scaler-baking strategy, opset, naming, and
sidecar layout are all v2-equivalent. The intent is to keep the v3
deploy story bit-equivalent to v2 from the runtime perspective; only
the MLP weights and shape change.

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
        MLP (Linear(6,32) → ReLU → Linear(32,16) → ReLU → Linear(16,1))
        |
        Squeeze (axis=-1) -> vmaf [N]

opset_version is pinned to 17 to match v2 + sister tiny-AI models.
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


def _build_mlp_medium(in_dim: int):  # type: ignore[no-untyped-def]
    from torch import nn

    return nn.Sequential(
        nn.Linear(in_dim, 32),
        nn.ReLU(),
        nn.Linear(32, 16),
        nn.ReLU(),
        nn.Linear(16, 1),
    )


class _BundledScalerMLP:
    """Pure-PyTorch wrapper that prepends ``(x - mean) / std`` to the MLP.

    Identical to v2's wrapper — the runtime contract is unchanged.
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
    n_params: int,
    run_provenance: dict[str, Any],
) -> dict[str, Any]:
    sidecar = {
        "id": "vmaf_tiny_v3",
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
        "arch": "mlp_medium",
        "n_params": n_params,
        "notes": (
            "vmaf_tiny_v3 — canonical-6 + StandardScaler + mlp_medium "
            f"({n_params} params), 90 epochs Adam @ lr=1e-3, MSE. Scaler "
            "(mean, std) baked into the ONNX graph as Constant nodes "
            "so the runtime feeds raw feature values. Same recipe as "
            "vmaf_tiny_v2 but with ~3x hidden capacity (6 → 32 → 16 → 1)."
        ),
        "run_provenance": run_provenance,
    }
    write_manifest_json(sidecar_path, sidecar)
    return sidecar


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    ap = make_argument_parser(prog="export_vmaf_tiny_v3.py", description=__doc__)
    ap.add_argument(
        "--ckpt",
        type=Path,
        required=True,
        help="Trained checkpoint produced by train_vmaf_tiny_v3.py.",
    )
    ap.add_argument(
        "--out-onnx",
        type=Path,
        required=True,
        help="Destination ONNX file (typically model/tiny/vmaf_tiny_v3.onnx).",
    )
    ap.add_argument(
        "--out-sidecar",
        type=Path,
        required=True,
        help="Sidecar JSON (input/output names + opset, mirrors v2 format).",
    )
    args = ap.parse_args(raw_argv)

    import torch

    # nosec B614: weights_only=False is required because the .pt stores
    # scaler stats + train metrics + features tuple in plain Python types.
    # Trust boundary: developer-supplied --ckpt produced by train_predictor_v3.py.
    state = torch.load(args.ckpt, map_location="cpu", weights_only=False)  # nosec B614
    features = state["features"]
    in_dim = len(features)
    mean = np.asarray(state["input_mean"], dtype=np.float64)
    std = np.asarray(state["input_std"], dtype=np.float64)

    if in_dim != 6:
        print(f"[export-v3] expected 6 features, got {in_dim}", file=sys.stderr)
        return 2

    mlp = _build_mlp_medium(in_dim)
    mlp.load_state_dict(state["state_dict"])
    mlp.eval()

    wrapper = _BundledScalerMLP(mlp, mean, std)

    dummy = torch.zeros(1, in_dim, dtype=torch.float32)
    args.out_onnx.parent.mkdir(parents=True, exist_ok=True)
    print(f"[export-v3] tracing wrapper -> {args.out_onnx} (opset={OPSET})")
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
    n_params = int(state.get("n_params", 0))
    print(f"[export-v3] sha256={digest}")
    print(f"[export-v3] size  ={args.out_onnx.stat().st_size} bytes")

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
        n_params=n_params,
        run_provenance=run_provenance,
    )
    print(f"[export-v3] wrote {args.out_sidecar}")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
