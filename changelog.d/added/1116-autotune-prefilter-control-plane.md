- **`vmaf-tune prefilter` — Pelorus deband autotune control plane
  ([ADR-1116](../docs/adr/1116-autotune-prefilter-control-plane.md)).**
  Adds a new `filter_adapters/` family (sibling to `codec_adapters/`)
  with a `FilterAdapter` Protocol and
  `filter_adapters/pelorus_deband.py`, which hard-codes the 10 frozen
  tunable knobs (`range`, `thry`, `thrc`, `grainy`, `grainc`,
  `softness`, `detail`, `dither`, `dynamic`, `protect`) from the Pelorus
  control-plane contract (Pelorus ADR-0110) and emits the
  `-vf "pelorus_deband_vulkan=range=..:thry=..:.."` fragment for a
  parameter dict, with range/type validation against the frozen ranges.
  The new `vmaf-tune prefilter` subcommand drives a joint Optuna TPE
  search over the deband knob space **and** the encoder CRF (reusing the
  `fast.py` `TPESampler` study), scoring each `deband → HW encode →
  VMAF` probe and returning the recommended strengths + CRF + per-probe
  VMAF. vmafx stays Vulkan-free — it only emits the filter string and
  scores the encoded output; the live encode loop is gated behind a
  `pelorus_filter_available()` check so it fails with a clear message
  when the Pelorus Vulkan filter is not compiled into the ffmpeg build.
  `--smoke` exercises the joint search end-to-end with a synthetic
  surface (no ffmpeg / Vulkan / GPU). Implements integration-plan
  workstreams D1 + D2.
