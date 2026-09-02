#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Export a tiny placeholder ONNX checkpoint for the TransNet V2
shot-boundary detector (T6-3a).

This script does NOT reproduce the published TransNet V2 weights from
Soucek & Lokoc (2020). It produces a small randomly-initialised CNN
whose graph respects the I/O contract:

    input  "frames"          : float32 [1, 100, 3, 27, 48]
                                (100-frame window, RGB, 27x48 thumbnails)
    output "boundary_logits" : float32 [1, 100]
                                (per-frame shot-boundary logits)

The output is a smoke-only model shipped to exercise the load / session /
100-frame-buffer plumbing that the eventual real-weights drop in
T6-3a-followup will inherit.

Why a placeholder rather than upstream weights:

* upstream ``github.com/soCzech/TransNetV2`` distributes a TensorFlow
  checkpoint under the MIT license but doesn't ship a pre-converted
  ONNX. Converting TF -> ONNX reliably requires ``tf2onnx`` plus a
  manual graph cleanup step that is out-of-scope for this PR;
* training a fresh shot-boundary model from scratch takes hours and is
  out-of-scope;
* shipping a placeholder unblocks every other piece of the integration
  (registry schema, C extractor, smoke test, docs, ADR) and hard-codes
  the contract that T6-3a-followup only has to swap weights against.

Usage::

    python3 ai/scripts/export_transnet_v2_placeholder.py \
        --output model/tiny/transnet_v2.onnx

Re-running is idempotent.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import torch
from torch import nn

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root
TINY_DIR = REPO_ROOT / "model" / "tiny"
REGISTRY = TINY_DIR / "registry.json"

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.file_utils import sha256  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

WINDOW = 100
CHANNELS = 3
HEIGHT = 27
WIDTH = 48


class TransNetV2Placeholder(nn.Module):
    """Tiny CNN with the TransNet V2 100-frame I/O contract.

    Input:  ``(1, 100, 3, 27, 48)`` — 100 consecutive RGB thumbnails.
    Output: ``(1, 100)`` — per-frame shot-boundary *logits* (sigmoid is
            applied on the C-extractor side).

    Architecture: collapse the 100x3x27x48 input to 100x(3*27*48)=100x3888,
    pass through a tiny MLP, project to 100 logits. Roughly 4K parameters
    after seed-0 init. Smoke-only — T6-3a-followup will replace the
    weights against the real Soucek & Lokoc 2020 checkpoint while
    keeping the same I/O shape.
    """

    def __init__(self) -> None:
        super().__init__()
        self.flat_dim = CHANNELS * HEIGHT * WIDTH
        self.proj = nn.Linear(self.flat_dim, 8)
        self.act = nn.ReLU(inplace=False)
        self.head = nn.Linear(8, 1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # x: (B, T=100, C=3, H=27, W=48)
        b, t, c, h, w = x.shape
        flat = x.reshape(b * t, c * h * w)
        hidden = self.act(self.proj(flat))
        # logits: (B*T, 1) -> (B, T)
        logits = self.head(hidden).reshape(b, t)
        return logits


def _export(onnx_path: Path, opset: int) -> None:
    torch.manual_seed(0x715A57)
    model = TransNetV2Placeholder().eval()
    dummy = torch.zeros(1, WINDOW, CHANNELS, HEIGHT, WIDTH, dtype=torch.float32)
    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    # Use the legacy TorchScript exporter (dynamo=False) so the resulting
    # graph is a single self-contained file under ONNX-Runtime's strict
    # op allowlist (``Reshape``, ``MatMul``, ``Add``, ``Relu``).
    torch.onnx.export(
        model,
        dummy,
        str(onnx_path),
        input_names=["frames"],
        output_names=["boundary_logits"],
        opset_version=opset,
        do_constant_folding=True,
        dynamo=False,
    )


def _write_sidecar(onnx_path: Path, *, run_provenance: dict[str, object] | None = None) -> Path:
    sidecar = onnx_path.with_suffix(".json")
    payload = {
        "id": "transnet_v2",
        "kind": "shot_detector",
        "onnx": onnx_path.name,
        "opset": 17,
        "input_name": "frames",
        "output_name": "boundary_logits",
        "frame_window": WINDOW,
        "thumbnail_h": HEIGHT,
        "thumbnail_w": WIDTH,
        "channels": CHANNELS,
        "boundary_threshold": 0.5,
        "smoke": True,
        "name": "vmaf_tiny_transnet_v2_placeholder_v0",
        "license": "BSD-3-Clause-Plus-Patent",
        "notes": (
            "Placeholder TransNet V2 shot-boundary detector "
            "(T6-3a). 100-frame window of 27x48 RGB thumbnails "
            "-> per-frame shot-boundary logits. SMOKE-ONLY: "
            "random init, not trained. Real upstream weights "
            "(Soucek & Lokoc 2020, MIT) tracked under "
            "T6-3a-followup. See docs/ai/models/transnet_v2.md."
        ),
    }
    if run_provenance is not None:
        payload["run_provenance"] = run_provenance
    write_manifest_json(sidecar, payload)
    return sidecar


def _update_registry(onnx_path: Path) -> None:
    if not REGISTRY.exists():
        sys.exit(f"missing {REGISTRY}")
    doc = json.loads(REGISTRY.read_text())
    models: list[dict] = doc.get("models", [])
    by_id = {m["id"]: m for m in models}
    digest = sha256(onnx_path)
    entry = {
        "id": "transnet_v2",
        "kind": "shot_detector",
        "onnx": onnx_path.name,
        "opset": 17,
        "sha256": digest,
        "smoke": True,
        "license": "BSD-3-Clause-Plus-Patent",
        "notes": (
            "Placeholder TransNet V2 shot-boundary detector (T6-3a). "
            "100-frame window of 27x48 RGB thumbnails -> per-frame "
            "shot-boundary logits. SMOKE-ONLY: random init, not "
            "trained. Real upstream weights (Soucek & Lokoc 2020, MIT) "
            "tracked under T6-3a-followup. Per-shot CRF predictor is "
            "T6-3b. See docs/adr/0220-transnet-v2-shot-detector.md and "
            "docs/ai/models/transnet_v2.md."
        ),
    }
    by_id["transnet_v2"] = entry
    models = sorted(by_id.values(), key=lambda m: m["id"])
    doc["models"] = models
    REGISTRY.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n")


def main(argv: list[str] | None = None) -> None:
    parser = make_argument_parser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=TINY_DIR / "transnet_v2.onnx",
        help="ONNX output path (default: model/tiny/transnet_v2.onnx)",
    )
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument(
        "--no-registry",
        action="store_true",
        help="Skip registry.json + sidecar update (dry-run)",
    )
    raw_argv = collect_cli_argv(argv)
    args = parser.parse_args(raw_argv)

    if args.no_registry:
        return
    _export(args.output, args.opset)
    print(f"[export] wrote {args.output} ({args.output.stat().st_size} bytes)")
    sidecar = _write_sidecar(
        args.output,
        run_provenance=build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            outputs={
                "onnx": args.output,
                "sidecar": args.output.with_suffix(".json"),
                "registry": REGISTRY,
            },
        ),
    )
    print(f"[export] wrote {sidecar}")
    _update_registry(args.output)
    print(f"[export] updated {REGISTRY}")


if __name__ == "__main__":
    main()
