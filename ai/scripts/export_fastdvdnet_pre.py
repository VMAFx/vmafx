#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Export the *real* upstream FastDVDnet temporal denoiser into the
fork's luma-only 5-frame ONNX contract (T6-7b).

This script supersedes ``export_fastdvdnet_pre_placeholder.py`` (T6-7),
which shipped a randomly-initialised 3-layer CNN purely to pin the
contract.  Upstream FastDVDnet (Tassano, Delon, Veit 2020) is published
under the MIT license at github.com/m-tassano/fastdvdnet pinned to
commit ``c8fdf61`` (2024-02-01); the checkpoint ``model.pth`` (~9.51 MiB,
sha256 ``9d9d8413c33e3d9d961d07c530237befa1197610b9d60602ff42fd77975d2a17``)
is a 2.48M-parameter two-stage UNet that consumes RGB + a per-pixel
noise map and returns RGB.

The fork's C-side extractor (``core/src/feature/fastdvdnet_pre.c``,
ADR-0215) was scoped luma-only with the contract

    input  "frames"   : float32 NCHW [1, 5, H, W]   # [t-2, t-1, t, t+1, t+2]
    output "denoised" : float32 NCHW [1, 1, H, W]

so this exporter wraps upstream's RGB+noise-map graph in a small
``LumaAdapter`` that

    1. tiles the five luma planes into RGB (Y -> [Y, Y, Y]) so the
       upstream input shape ``(1, 15, H, W)`` is satisfied,
    2. supplies a constant noise map of sigma 25/255 (the upstream
       reference inference noise level for the published checkpoint),
    3. runs upstream ``FastDVDnet(num_input_frames=5)``,
    4. converts the RGB output back to luma via BT.601 weights
       ``Y = 0.299 R + 0.587 G + 0.114 B``.

The wrapper keeps every learned parameter from upstream untouched —
only the I/O packing is rewritten.  This trades some quality (the
network was not trained on luma-tiled-into-RGB inputs) for a real,
license-compatible weights drop that fits the existing C contract
unchanged.  The follow-up T6-7c is to retrain a luma-native variant
when the FFmpeg ``vmaf_pre_temporal`` filter (T6-7b proper) lands.

Usage::

    # 1. fetch upstream
    mkdir -p /tmp/fastdvdnet_upstream && cd /tmp/fastdvdnet_upstream
    curl -L -O https://raw.githubusercontent.com/m-tassano/fastdvdnet/c8fdf61/model.pth
    curl -L -O https://raw.githubusercontent.com/m-tassano/fastdvdnet/c8fdf61/models.py

    # 2. export
    python3 ai/scripts/export_fastdvdnet_pre.py \\
        --upstream-dir /tmp/fastdvdnet_upstream \\
        --output model/tiny/fastdvdnet_pre.onnx

Re-running is idempotent.

Provenance (license attribution required by upstream MIT license):

    Copyright 2024 Matias Tassano <mtassano@meta.com>
    https://github.com/m-tassano/fastdvdnet/blob/c8fdf61/LICENSE
"""

from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile
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

# Pinned upstream provenance — bumping these is a deliberate weights swap.
UPSTREAM_REPO = "https://github.com/m-tassano/fastdvdnet"
UPSTREAM_COMMIT = "c8fdf6182a0340e89dd18f5df25b47337cbede6f"
UPSTREAM_WEIGHTS_SHA256 = "9d9d8413c33e3d9d961d07c530237befa1197610b9d60602ff42fd77975d2a17"
# Reference-inference noise level used in upstream test_fastdvdnet.py
# examples; chosen to give the network a non-trivial denoising target on
# inputs whose true noise level isn't known at runtime.
DEFAULT_SIGMA_8BIT = 25.0 / 255.0

WINDOW = 5
CENTRE = 2


def _load_upstream_class(upstream_dir: Path) -> type[nn.Module]:
    """Import upstream ``models.FastDVDnet`` from a local checkout dir.

    We deliberately do NOT vendor the upstream source into this repo —
    the script imports it on the fly so the only artefact that lands
    here is the converted ONNX, the upstream license attribution, and
    this exporter.  Keeps upstream-license-attribution surface to one
    file.
    """
    models_py = upstream_dir / "models.py"
    if not models_py.is_file():
        sys.exit(
            f"missing {models_py}; clone {UPSTREAM_REPO} at commit "
            f"{UPSTREAM_COMMIT} and re-run with --upstream-dir."
        )
    spec = importlib.util.spec_from_file_location("upstream_models", models_py)
    if spec is None or spec.loader is None:
        sys.exit(f"could not load {models_py} as a module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    cls = getattr(module, "FastDVDnet", None)
    if cls is None:
        sys.exit(f"{models_py} has no FastDVDnet class")
    return cls


class _PixelShuffleAllowlistSafe(nn.Module):
    """Drop-in replacement for ``nn.PixelShuffle(r)`` that exports as
    Reshape/Transpose/Reshape rather than ``DepthToSpace``.

    The fork's ONNX op allowlist (``core/src/dnn/op_allowlist.c``)
    does not include ``DepthToSpace`` — the op is bounded and safe but
    has been kept out of the list to avoid widening the trusted
    surface for the rare cases that need it.  PixelShuffle is purely
    a shape op (zero learned parameters), so replacing it with the
    canonical reshape+transpose decomposition produces a numerically
    identical graph using only allowlisted ops (Reshape, Transpose).
    """

    def __init__(self, upscale_factor: int) -> None:
        super().__init__()
        self.r = int(upscale_factor)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # (B, C*r*r, H, W) -> (B, C, r, r, H, W) -> permute -> (B, C, H*r, W*r)
        b, c_rr, h, w = x.shape
        r = self.r
        c = c_rr // (r * r)
        y = x.reshape(b, c, r, r, h, w)
        y = y.permute(0, 1, 4, 2, 5, 3).contiguous()
        y = y.reshape(b, c, h * r, w * r)
        return y


def _replace_pixel_shuffle(module: nn.Module) -> None:
    """Recursively swap every ``nn.PixelShuffle`` for the allowlist-safe
    variant defined above.  Mutates in place; the upstream UpBlock has
    its PixelShuffle nested inside an ``nn.Sequential`` so we walk the
    full tree."""
    for name, child in module.named_children():
        if isinstance(child, nn.PixelShuffle):
            setattr(module, name, _PixelShuffleAllowlistSafe(child.upscale_factor))
        else:
            _replace_pixel_shuffle(child)


def _strip_data_parallel(state_dict: dict) -> dict:
    """Upstream ships the checkpoint inside an ``nn.DataParallel``
    wrapper, so every key is prefixed with ``module.``.  Strip the
    prefix so a plain ``FastDVDnet`` instance can ``load_state_dict``."""
    out = {}
    for k, v in state_dict.items():
        out[k[7:] if k.startswith("module.") else k] = v
    return out


class LumaAdapter(nn.Module):
    """Adapt upstream RGB+noise-map FastDVDnet to the fork's luma contract.

    Forward contract:
        input  ``(1, 5, H, W)`` luma (BT.601 Y, range [0, 1])
        output ``(1, 1, H, W)`` luma (BT.601 Y, range [0, 1])

    Internals:
        * tile each luma plane to RGB by repeating along a new channel
          axis (Y -> [Y, Y, Y]); upstream expects 15 channels = 5 frames
          times 3 colours;
        * synthesise a constant noise map at ``sigma`` (~25/255 by
          default, matching upstream's reference inference level);
        * run upstream FastDVDnet;
        * collapse RGB output back to luma via BT.601 weights.

    The BatchNorm layers in upstream's UNet are fused-into-the-graph at
    eval() time; ``do_constant_folding=True`` in ``torch.onnx.export``
    bakes them down to plain Conv biases.
    """

    # BT.601 Y = 0.299 R + 0.587 G + 0.114 B — chosen because the fork's
    # C-side normalisation reads the luma plane verbatim from the YUV
    # source (no colourspace inference), so BT.601 is the safest
    # round-trip when the upstream RGB weights were trained on
    # natural-colour tristimulus inputs.
    _LUMA_W = (0.299, 0.587, 0.114)

    def __init__(self, upstream: nn.Module, sigma: float = DEFAULT_SIGMA_8BIT) -> None:
        super().__init__()
        self.upstream = upstream
        # Register sigma + luma weights as buffers so torch.onnx exports
        # them as graph constants (no Python literal capture surprises).
        self.register_buffer("sigma", torch.tensor(float(sigma), dtype=torch.float32))
        luma_w = torch.tensor(self._LUMA_W, dtype=torch.float32).view(1, 3, 1, 1)
        self.register_buffer("luma_w", luma_w)

    def forward(self, frames: torch.Tensor) -> torch.Tensor:
        # frames: (1, 5, H, W) luma in [0, 1]
        # Tile to (1, 5*3, H, W) = (1, 15, H, W); upstream expects RGB
        # frames stacked along the channel axis with frame ordering
        # [t-2_R, t-2_G, t-2_B, t-1_R, ...].  Use Concat (allowlisted)
        # rather than expand+reshape (which exports as
        # Equal/Where/ConstantOfShape on dynamic spatial axes — those
        # ops are not on the fork's strict ONNX allowlist).
        per_frame = [frames[:, i : i + 1] for i in range(WINDOW)]
        rgb = torch.cat([p for p in per_frame for _ in range(3)], dim=1)
        # Constant noise map at upstream reference sigma — emit as a
        # plain Mul against a ones-tensor derived from the centre frame
        # so the export stays inside the allowlist (no Expand on
        # dynamic-shape sigma scalars).
        ones_like_centre = torch.ones_like(frames[:, CENTRE : CENTRE + 1])
        noise_map = ones_like_centre * self.sigma
        out_rgb = self.upstream(rgb, noise_map)
        # Clamp to [0, 1] (upstream's test_fastdvdnet.py does the same
        # before colourspace conversion).
        out_rgb = torch.clamp(out_rgb, 0.0, 1.0)
        # RGB -> Y via BT.601 weights.
        out_y = (out_rgb * self.luma_w).sum(dim=1, keepdim=True)
        return out_y


def _verify_upstream_weights(weights_path: Path) -> None:
    digest = sha256(weights_path)
    if digest != UPSTREAM_WEIGHTS_SHA256:
        sys.exit(
            f"upstream weights digest mismatch:\n"
            f"  got      {digest}\n"
            f"  expected {UPSTREAM_WEIGHTS_SHA256}\n"
            f"  re-fetch from {UPSTREAM_REPO} @ {UPSTREAM_COMMIT}"
        )


def _build_adapter(upstream_dir: Path, sigma: float) -> LumaAdapter:
    weights_path = upstream_dir / "model.pth"
    if not weights_path.is_file():
        sys.exit(f"missing {weights_path}; download from {UPSTREAM_REPO}")
    _verify_upstream_weights(weights_path)
    FastDVDnet = _load_upstream_class(upstream_dir)
    upstream = FastDVDnet(num_input_frames=WINDOW)
    state_dict = torch.load(weights_path, map_location="cpu", weights_only=True)
    upstream.load_state_dict(_strip_data_parallel(state_dict))
    # Swap PixelShuffle for the allowlist-safe reshape+transpose
    # decomposition AFTER loading weights — PixelShuffle has no
    # learned parameters so the swap is numerically identical.
    _replace_pixel_shuffle(upstream)
    upstream.eval()
    adapter = LumaAdapter(upstream, sigma=sigma).eval()
    return adapter


def _export(adapter: nn.Module, onnx_path: Path, height: int, width: int, opset: int) -> None:
    dummy = torch.zeros(1, WINDOW, height, width, dtype=torch.float32)
    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    # Legacy TorchScript exporter (dynamo=False) — keeps the graph
    # under ONNX-Runtime's strict op allowlist and compatible with the
    # fork's opset-17 target.  do_constant_folding=True fuses BN into
    # Conv biases at export time.
    torch.onnx.export(
        adapter,
        dummy,
        str(onnx_path),
        input_names=["frames"],
        output_names=["denoised"],
        opset_version=opset,
        dynamic_axes={
            "frames": {2: "H", 3: "W"},
            "denoised": {2: "H", 3: "W"},
        },
        do_constant_folding=True,
        dynamo=False,
    )


def _write_sidecar(onnx_path: Path, *, run_provenance: dict[str, object] | None = None) -> Path:
    sidecar = onnx_path.with_suffix(".json")
    payload = {
        "id": "fastdvdnet_pre",
        "kind": "filter",
        "onnx": onnx_path.name,
        "opset": 17,
        "input_name": "frames",
        "output_name": "denoised",
        "frame_window": WINDOW,
        "centre_index": CENTRE,
        "smoke": False,
        "name": "vmaf_tiny_fastdvdnet_pre_v1",
        "license": "MIT",
        "license_url": (
            "https://github.com/m-tassano/fastdvdnet/blob/" f"{UPSTREAM_COMMIT}/LICENSE"
        ),
        "upstream_repo": UPSTREAM_REPO,
        "upstream_commit": UPSTREAM_COMMIT,
        "upstream_weights_sha256": UPSTREAM_WEIGHTS_SHA256,
        "notes": (
            "FastDVDnet temporal pre-filter — real upstream weights "
            "(Tassano, Delon, Veit 2020; MIT) wrapped by a luma "
            "adapter (Y->[Y,Y,Y] tiling, sigma=25/255 noise map, "
            "RGB->Y BT.601 collapse). 5-frame window "
            "[t-2, t-1, t, t+1, t+2] -> denoised frame t. T6-7b. "
            "See docs/ai/models/fastdvdnet_pre.md."
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
        "id": "fastdvdnet_pre",
        "kind": "filter",
        "onnx": onnx_path.name,
        "opset": 17,
        "sha256": digest,
        "smoke": False,
        "license": "MIT",
        "license_url": (
            "https://github.com/m-tassano/fastdvdnet/blob/" f"{UPSTREAM_COMMIT}/LICENSE"
        ),
        "description": (
            "FastDVDnet temporal pre-filter (5-frame luma window) — "
            "upstream m-tassano/fastdvdnet weights wrapped for the fork's "
            "luma-only NCHW contract."
        ),
        "notes": (
            "Real FastDVDnet upstream weights (Tassano, Delon, Veit 2020; "
            "MIT). Upstream pinned at "
            f"github.com/m-tassano/fastdvdnet@{UPSTREAM_COMMIT[:7]}; "
            "luma adapter (Y->[Y,Y,Y] tile, sigma=25/255 noise map, "
            "RGB->Y BT.601 collapse) preserves the libvmaf 5-frame "
            "luma I/O contract from ADR-0215. Exported via "
            "ai/scripts/export_fastdvdnet_pre.py. T6-7b. "
            "See docs/ai/models/fastdvdnet_pre.md and "
            "docs/adr/0215-fastdvdnet-pre-filter.md."
        ),
    }
    by_id["fastdvdnet_pre"] = entry
    models = sorted(by_id.values(), key=lambda m: m["id"])
    doc["models"] = models
    REGISTRY.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n")


def main(argv: list[str] | None = None) -> None:
    parser = make_argument_parser(description=__doc__)
    scratch_root = Path(os.environ.get("VMAF_TINY_AI_SCRATCH", tempfile.gettempdir()))
    default_upstream_dir = scratch_root / "fastdvdnet_upstream"
    parser.add_argument(
        "--upstream-dir",
        type=Path,
        default=default_upstream_dir,
        help=(
            "Directory holding upstream model.pth + models.py at commit "
            f"{UPSTREAM_COMMIT[:7]}. Default: $VMAF_TINY_AI_SCRATCH/"
            "fastdvdnet_upstream (falls back to the system temp dir)."
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=TINY_DIR / "fastdvdnet_pre.onnx",
        help="ONNX output path (default: model/tiny/fastdvdnet_pre.onnx)",
    )
    parser.add_argument(
        "--sigma",
        type=float,
        default=DEFAULT_SIGMA_8BIT,
        help=(
            "Noise-map sigma (in [0, 1]) baked into the wrapper graph. "
            f"Default: {DEFAULT_SIGMA_8BIT:.5f} (= 25/255, upstream "
            "reference inference level)."
        ),
    )
    parser.add_argument("--height", type=int, default=64)
    parser.add_argument("--width", type=int, default=64)
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument(
        "--no-registry",
        action="store_true",
        help="Skip registry.json + sidecar update (dry-run)",
    )
    raw_argv = collect_cli_argv(argv)
    args = parser.parse_args(raw_argv)

    adapter = _build_adapter(args.upstream_dir, args.sigma)
    _export(adapter, args.output, args.height, args.width, args.opset)
    print(f"[export] wrote {args.output} ({args.output.stat().st_size} bytes)")
    if args.no_registry:
        return
    sidecar = _write_sidecar(
        args.output,
        run_provenance=build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={
                "upstream_dir": args.upstream_dir,
                "upstream_models_py": args.upstream_dir / "models.py",
                "upstream_weights": args.upstream_dir / "model.pth",
            },
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
