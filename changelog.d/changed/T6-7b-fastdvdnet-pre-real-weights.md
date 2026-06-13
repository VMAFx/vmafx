- **AI / DNN:** Replaced the `fastdvdnet_pre` smoke-only placeholder
  ONNX with real upstream FastDVDnet weights (Tassano, Delon, Veit
  2020; MIT license) pinned at `m-tassano/fastdvdnet` commit `c8fdf61`.
  The new graph wraps upstream's RGB+noise-map model in a `LumaAdapter`
  that preserves the C-side `[1, 5, H, W]` luma I/O contract from
  ADR-0215: `Y → [Y, Y, Y]` tiling for the upstream 15-channel input,
  a constant `sigma = 25/255` noise map, and BT.601 RGB→Y collapse on
  the output. Upstream `nn.PixelShuffle` is swapped at export time for
  an allowlist-safe `Reshape`/`Transpose`/`Reshape` decomposition
  (`DepthToSpace` is deliberately not on the ONNX op allowlist).
  Registry row `model/tiny/registry.json` flips `smoke: false` with
  the new MIT license, upstream commit pin, and refreshed sha256.
  9.5 MiB ONNX, opset 17. New exporter
  `ai/scripts/export_fastdvdnet_pre.py`. See ADR-0253 and
  `docs/ai/models/fastdvdnet_pre.md`.
