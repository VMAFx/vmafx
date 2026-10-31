<!-- markdownlint-disable MD013 -->
# ffmpeg-patches/

Local patch series against FFmpeg **n9.0.2** integrating VMAFx through
`libavfilter/vf_vmafx*` and unified rate-control / profile CLI options.

Every build applies the **shared FFmpeg fix series** first: fixes to FFmpeg
itself, maintained for all consumers in
[VMAFx/ffmpeg-patches](https://github.com/VMAFx/ffmpeg-patches) and pinned in
`build-config.env` (`FFMPEG_FIX_SERIES_*`). `scripts/ci/ffmpeg-shared-series.sh`
downloads the pinned release, verifies it, and applies it; the patches in this
directory add VMAFx integration on top. See
[FFmpeg patch automation](../docs/development/ffmpeg-patch-automation.md),
[ADR-2166](../docs/adr/2166-ffmpeg-series-drops-vulkan-shims.md), and
[ADR-3143](../docs/adr/3143-ffmpeg-shared-fix-series.md). For migration from
retired legacy filters, see the
[Migration Guide](../docs/usage/ffmpeg.md#migrating-from-the-libvmaf-filters).

## Patch Stack (`series.txt`)

The series consists of six consecutive patches:

- **`0001-codec-qpfile-unified.patch`**: Adds unified `-qpfile <path>` AVOption
  to `libx264`, `libsvtav1`, and `libaom-av1`. Uses the shared per-frame parser
  in `libavcodec/qpfile_parser.{c,h}` to supply macroblock and per-picture
  quantization deltas.
- **`0002-pass-autotune-cli-glue.patch`**: Adds `-pass-autotune` CLI option in
  `fftools/ffmpeg_opt.c` for multi-pass rate-control orchestration.
- **`0003-add-vmafx-profile-cli-glue.patch`**: Adds `-vmafx-profile <path>`
  CLI option in `fftools/ffmpeg_opt.c` for profile-guided encoder tuning.
- **`0004-add-vmafx-filter.patch`**: Implements the primary `vmafx` scoring
  filter and `vmafx_tune` filter. Built from canonical sources
  `ffmpeg-patches/src/vf_vmafx.c` and `vf_vmafx_options.h`. Provides unified
  runtime backend selection (`backend=auto|cpu|cuda|sycl|hip|metal`), zero-copy
  hardware frame import (CUDA, VA-API/QSV, DRM PRIME, Metal IOSurface,
  Vulkan), AVFrame colorimetry mapping to `libvmafx`, windowed statistics,
  score formatting (`score_fmt`), and fail-closed error handling. Enabled via
  `--enable-libvmafx`.
- **`0005-add-vmafx-pre-filter.patch`**: Implements the `vmafx_pre` learned
  filter for ONNX neural pre-processing. Built from canonical source
  `ffmpeg-patches/src/vf_vmafx_pre.c`. Enabled via `--enable-libvmafx`.
- **`0006-fftools-loopback-decoder-hwaccel.patch`**: Adds GPU loopback decoder
  hardware acceleration in `fftools/ffmpeg_dec_hwaccel.c` before `-dec` for
  efficient decode-score pipelines (ADR-2125 Q-048).

## Test Scripts

Test scripts under `ffmpeg-patches/test/` verify the patch series and filter
behavior:

- **`build-and-run.sh`**: Fetches clean upstream FFmpeg at `FFMPEG_TAG`, applies
  the shared fix series and `series.txt`, builds against `libvmafx`, verifies
  option registration, and tests score equivalence against the `vmaf` CLI.
- **`vmafx_filter_check.py`**: Integration test suite verifying `vmafx`:
  - `parity`: Per-frame and pooled metric parity against the `vmaf` CLI.
  - `windows`: Sliding-window statistics against CLI reference windows.
  - `contract`: Frame ordering, un-scored frame counts, and timeline contracts.
  - `provenance`: Provenance record logging and metadata embed contracts.
  - `pool`: VA-API fixed frame pool ingestion and error bounds.
  - `refusal`: Validation of invalid parameters and incompatible options.
  - `layouts`: 4:2:2 and 4:4:4 semi-planar, packed, and MSB layouts.
- **`check-vmafx-input-colorimetry.sh`**: Verifies HDR colorimetry mapping
  from AVFrame properties into `libvmafx`, checking tagged BT.2020/PQ parity
  against the CLI and fail-closed refusal on untagged inputs.
- **`check-input-contract.sh`**: Inspects patch text to enforce input ordering,
  pad declaration, and PTS contract invariants.
- **`check-sycl-feature-routing.sh`**: Verifies SYCL backend option and feature
  routing in `vf_vmafx.c`.
- **`check-sycl-pixfmt.sh`**: Verifies software and hardware pixel format lists
  in `vf_vmafx.c`.
- **`check-vmafx-import-failure.sh`**: Injects frame import faults to verify
  fail-closed termination naming the failed frame and printing no pooled score.

## How to Replay

To verify that the patch series applies cleanly on top of the shared fix
series:

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --check
```

To refresh patch context or rebase against updated sources:

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --refresh
```

## License

EUPL-1.2 for patches authored in this repository. Patches are applied against
FFmpeg (LGPL-2.1-or-later / GPL-2.0-or-later); distribution terms of compiled
binaries are governed by FFmpeg's license.
