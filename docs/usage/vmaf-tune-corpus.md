<!-- markdownlint-disable MD013 MD024 MD060 -->
# `vmaf-tune corpus` — encoder grid sweep and corpus schema

`vmaf-tune corpus` encodes a reference clip over a grid of presets and
CRF values with one encoder, scores each encode with `vmaf`, and writes
one JSONL row per cell. The corpus feeds
[`recommend`](vmaf-tune-recommend.md),
[`benchmark`](vmaf-tune-benchmark.md) and the
[predictor training](vmaf-tune-predict.md). Overview:
[vmaf-tune.md](vmaf-tune.md).

## Run a sweep

The grid is the Cartesian product of `--preset` and `--crf`. This run
writes six rows:

```shell
vmaf-tune corpus \
    --source ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 10 \
    --preset medium --preset slow \
    --crf 22 --crf 28 --crf 34 \
    --output corpus.jsonl
```

`--source` is repeatable: pass one flag per source clip. Encodes are
written to `--encode-dir` and deleted after scoring unless
`--keep-encodes` is set. On success the command prints
`wrote N rows -> corpus.jsonl` on stderr and exits 0. Exit code 2 means
a missing required flag or an unavailable `--score-backend`.

!!! note
    `--encoder` takes one value per run. To sweep several codecs, run
    the command once per encoder, or use
    [`vmaf-tune compare --encoders a,b,c`](vmaf-tune-compare.md).

### Choose another encoder

`--encoder` accepts any of the 19 registered adapters (see
[codec adapters](vmaf-tune-codec-adapters.md)). The `libsvtav1` adapter
takes the same x264-style preset names and translates them to SVT-AV1
integer presets internally. AV1 CRF values span `0..63`; the informative
window is `(20, 50)`:

```shell
vmaf-tune corpus \
    --source ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 10 \
    --encoder libsvtav1 \
    --preset medium --preset slow \
    --crf 28 --crf 35 --crf 42 \
    --output corpus_av1.jsonl
```

The corpus row records the preset name (`"medium"`); the FFmpeg command
line carries the integer SVT-AV1 expects (`-preset 7`). The full mapping
is on the [AV1 codecs page](vmaf-tune-codec-av1.md)
([ADR-0294](../adr/0294-vmaf-tune-codec-adapter-svtav1.md)).

## Flags

| Flag | Default | Meaning |
|------|---------|---------|
| `--source PATH` | required | Reference video. Repeatable. |
| `--width N` / `--height N` | required | Source resolution. |
| `--pix-fmt PFMT` | `yuv420p` | Forwarded to ffmpeg `-pix_fmt`. |
| `--framerate F` | `24.0` | Source framerate. |
| `--duration S` | `0.0` | Source duration in seconds, used for the bitrate calculation. |
| `--encoder NAME` | `libx264` | Codec adapter; one of the 19 registered adapters. |
| `--preset P` | required | Preset name. Repeatable. Names are encoder specific; see [codec adapters](vmaf-tune-codec-adapters.md). |
| `--crf N` | required | CRF (quality knob) value. Repeatable. Optional with `--coarse-to-fine`, which picks the CRF axis itself. |
| `--output PATH` | `corpus.jsonl` | JSONL destination. |
| `--encode-dir PATH` | `.workingdir/cache/vmafx-tune/encodes` | Scratch directory for encodes; gitignored by convention. |
| `--keep-encodes` | off | Keep encoded files after scoring. |
| `--vmaf-model NAME` | per encode height | Score every row with this model. Without it the model follows the encode height ([resolution-aware](vmaf-tune-resolution-aware.md)). |
| `--neg` | off | Score with the NEG variant of the model that applies. |
| `--ffmpeg-bin PATH` | `ffmpeg` | ffmpeg binary. |
| `--ffprobe-bin PATH` | `ffprobe` | ffprobe binary, used for HDR detection. |
| `--vmaf-bin PATH` | `vmaf` | vmaf binary. |
| `--score-backend NAME` | `auto` | libvmaf scoring backend: `auto`, `cpu`, `cuda`, `sycl`, `hip` or `metal`. See [score backend](vmaf-tune-score-backend.md). |
| `--no-source-hash` | off | Skip `src_sha256`: faster on large YUV files, but loses provenance. |
| `--two-pass` | off | Two-pass encode for adapters with `supports_two_pass` (`libx264`, `libx265`, `libvpx-vp9`, `libaom-av1`, `libvvenc`); other adapters warn on stderr and run single-pass. Doubles encode time. See [multi-pass](vmaf-tune-multipass.md). |
| `--sample-clip-seconds N` | `0.0` | Encode and score only the centre N seconds of each source; `0` uses the full source. See [HDR and sampling](vmaf-tune-hdr-and-sampling.md). |
| `--coarse-to-fine` | off | Coarse-then-fine CRF search instead of the full grid. See [coarse-to-fine](vmaf-tune-coarse-to-fine.md). |
| `--coarse-step N` | `10` | CRF step of the coarse pass. |
| `--fine-radius N` | `5` | Radius around the best coarse CRF searched in the fine pass. |
| `--fine-step N` | `1` | CRF step of the fine pass. |
| `--target-vmaf F` | none | Target for `--coarse-to-fine`. |
| `--auto-hdr` | on | Probe each source with ffprobe; inject HDR encoder flags and the HDR-aware scoring path when PQ or HLG signalling is found. |
| `--force-sdr` | off | Treat every source as SDR and skip detection. |
| `--force-hdr-pq` | off | Treat every source as HDR PQ (SMPTE-2084) without probing. Useful for raw YUV, which carries no colour metadata. |
| `--force-hdr-hlg` | off | Treat every source as HDR HLG (ARIB STD-B67) without probing. |

The four HDR flags are mutually exclusive.

!!! note "Model selection on the CLI"
    Without `--vmaf-model`, `corpus` selects the VMAF model per encode
    height ([resolution-aware](vmaf-tune-resolution-aware.md)); with it,
    every row is scored with that model. `--neg` takes the NEG variant of
    either. The choice is printed on stderr and recorded in each row's
    `vmaf_model`.

!!! note "Hardware encoders"
    A hardware encoder (NVENC, QSV, AMF, VideoToolbox) is probed with a
    one-frame encode before the sweep; when the host cannot run it,
    `corpus` exits 2 with the probe's reason. See
    [hardware encoders](vmaf-tune-codec-hardware.md).

!!! note "Cache"
    The encode cache has no `corpus` flags. It is enabled from Python
    with `CorpusOptions(cache_enabled=True, cache_dir=...)`; see
    [cache](vmaf-tune-cache.md).

## Corpus JSONL schema

Each row is one JSON object on its own line. The full key list is
exported as `vmaftune.CORPUS_ROW_KEYS` and versioned by
`vmaftune.SCHEMA_VERSION`, currently `3`. Changing a row's shape means
bumping the version in step with the predictor training code.

| Schema | Added |
|--------|-------|
| v2 | `clip_mode`, for sample-clip mode ([ADR-0301](../adr/0301-vmaf-tune-sample-clip.md)) |
| v3 | HDR provenance triple `hdr_transfer` / `hdr_primaries` / `hdr_forced`, shot statistics, the canonical-6 aggregates and the `enc_internal_*` columns |

### Row identity and source

| Key | Type | Description |
|-----|------|-------------|
| `schema_version` | int | Currently `3`. |
| `run_id` | str | Per-row UUID4 hex. |
| `timestamp` | str | UTC ISO-8601, seconds precision. |
| `src` | str | Path to the reference. |
| `src_sha256` | str | SHA-256 of the reference; empty with `--no-source-hash`. |
| `width` / `height` | int | Source dimensions. |
| `pix_fmt` | str | Source pixel format. |
| `framerate` | float | Source framerate. |
| `duration_s` | float | Source duration in seconds. |

### Encode and score

| Key | Type | Description |
|-----|------|-------------|
| `encoder` | str | Codec adapter name, for example `libx264`. |
| `encoder_version` | str | Detected encoder version, for example `libx264-164`. |
| `preset` | str | Encoder preset. |
| `crf` | int | Quality knob value. |
| `extra_params` | list[str] | Additional encoder arguments; `[]` for a plain grid run. |
| `encode_path` | str | Path to the encoded file; empty when not retained. |
| `encode_size_bytes` | int | Encoded file size. |
| `bitrate_kbps` | float | $\dfrac{8 \cdot \mathrm{encode\_size\_bytes} / 1000}{\mathrm{duration\_s}}$. |
| `encode_time_ms` | float | Wall-clock encode time. |
| `vmaf_score` | float | Pooled-mean VMAF; `NaN` if scoring was skipped or failed. |
| `vmaf_model` | str | Model version string that scored the row. |
| `score_time_ms` | float | Wall-clock scoring time. |
| `ffmpeg_version` | str | Detected ffmpeg version. |
| `vmaf_binary_version` | str | Detected vmaf binary version. |
| `exit_status` | int | First non-zero of the encode and score exit codes. |
| `clip_mode` | str | `"full"` (default) or `"sample_<N>s"` from `--sample-clip-seconds`. v2+. |

### HDR provenance (v3+)

| Key | Type | Description |
|-----|------|-------------|
| `hdr_transfer` | str | `""` for SDR, `"pq"` (SMPTE-2084) or `"hlg"` (ARIB STD-B67). |
| `hdr_primaries` | str | Raw ffprobe `color_primaries`, for example `bt2020`; empty for SDR. |
| `hdr_forced` | bool | `true` when `--force-hdr-*` or `--force-sdr` overrode detection. |

### Content features (v3+)

| Key | Type | Description |
|-----|------|-------------|
| `shot_count` | int | TransNet-V2 shots in the source; `0` when shot detection is unavailable. |
| `shot_avg_duration_sec` | float | Mean shot length in seconds; `0.0` when unavailable. |
| `shot_duration_std_sec` | float | Population standard deviation of shot lengths, a content-class proxy (animation low, live action high). |
| `adm2_mean` | float | Per-frame ADM2 mean (canonical-6); `NaN` when scoring was skipped ([ADR-0366](../adr/0366-corpus-schema-v3.md)). |
| `vif_scale0_mean` … `motion2_std` | float | The remaining canonical-6 mean and standard-deviation aggregates, 12 columns in total. |

The twelve canonical-6 columns (`adm2_mean`, `adm2_std`,
`vif_scale0_mean` through `vif_scale3_std`, `motion2_mean`,
`motion2_std`) come from libvmaf's pooled metrics. When the scoring
model omits VIF, including the default `vmaf_v1.0.16_3d0h` model
(ADR-1168, ADR-1169), both the Python `vmaf-tune` and the Go
`vmafx-tune` pass `--feature vif` to libvmaf and parse feature aliases
with option suffixes, so every canonical-6 column holds a real value
instead of `NaN`.

### Encoder-internal statistics (v3+, ADR-0400)

| Key | Type | Description |
|-----|------|-------------|
| `enc_internal_qp_mean` | float | Per-frame QP mean from the pass-1 statistics. `0.0` for opt-out adapters. |
| `enc_internal_qp_std` | float | Per-frame QP standard deviation. |
| `enc_internal_bits_mean` | float | Per-frame bit-cost mean (`tex+mv+misc`). |
| `enc_internal_bits_std` | float | Per-frame bit-cost standard deviation. |
| `enc_internal_mv_mean` | float | Per-frame motion-vector bit-cost mean. |
| `enc_internal_mv_std` | float | Per-frame motion-vector bit-cost standard deviation. |
| `enc_internal_itex_mean` | float | Mean intra-texture cost across I/i frames. |
| `enc_internal_ptex_mean` | float | Mean predicted-texture cost across P/B/b frames. |
| `enc_internal_intra_ratio` | float | Fraction of macroblocks coded as intra. |
| `enc_internal_skip_ratio` | float | Fraction of macroblocks coded as skip. |

Adapters that declare `supports_encoder_stats = True` populate the ten
`enc_internal_*` columns. Today these are `libx264` and `libx265`; the
parser normalises x264 macroblock counters and x265 `icu` / `pcu` /
`scu` CTU counters into the same intra, predicted and skip ratio
columns.

Hardware encoders (NVENC, AMF, QSV, VideoToolbox) and the AV1 and VVC
software encoders (`libaom-av1`, `libsvtav1`, `libvvenc`) opt out and
emit `0.0` in every column, so the schema stays uniform across a
corpus. The cost for opt-in adapters is encode time: the harness runs
a stats-only `-pass 1` invocation before the production CRF encode, which
roughly doubles per-encode wall time.

### Example row

```json
{
  "schema_version": 3,
  "run_id": "0a3b1c8b...",
  "timestamp": "2026-05-03T16:00:00+00:00",
  "src": "ref.yuv",
  "src_sha256": "",
  "width": 1920, "height": 1080, "pix_fmt": "yuv420p",
  "framerate": 24.0, "duration_s": 10.0,
  "encoder": "libx264", "encoder_version": "libx264-164",
  "preset": "medium", "crf": 28,
  "extra_params": [],
  "encode_path": "",
  "encode_size_bytes": 845210,
  "bitrate_kbps": 676.168,
  "encode_time_ms": 4321.0,
  "vmaf_score": 92.41,
  "vmaf_model": "vmaf_v1.0.16_3d0h",
  "score_time_ms": 1820.5,
  "ffmpeg_version": "6.1.1",
  "vmaf_binary_version": "3.2.1",
  "exit_status": 0,
  "clip_mode": "full",
  "shot_count": 12,
  "shot_avg_duration_sec": 0.83,
  "shot_duration_std_sec": 0.41,
  "adm2_mean": 9.73, "adm2_std": 0.12,
  "enc_internal_qp_mean": 25.23,
  "enc_internal_qp_std": 0.12,
  "enc_internal_bits_mean": 4975.0,
  "enc_internal_bits_std": 1820.5,
  "enc_internal_mv_mean": 60.3,
  "enc_internal_mv_std": 32.1,
  "enc_internal_itex_mean": 8000.0,
  "enc_internal_ptex_mean": 1500.0,
  "enc_internal_intra_ratio": 0.07,
  "enc_internal_skip_ratio": 0.16
}
```

## JSON artifact portability

Human-facing CLI JSON, report artifacts, the executor result JSONL
(`tune_results*.jsonl`) and local sidecar state files are strict
RFC 8259 JSON. Values that are non-finite in memory (`NaN`, `Infinity`,
`-Infinity`) are written as `null`, so notebooks, dashboards, FFmpeg
profile consumers and MCP clients can read them with strict JSON
decoders.

Corpus JSONL rows are the exception: they are the training interchange
format, and their missing-feature semantics are the ones documented in
the schema above.

## See also

- [Overview](vmaf-tune.md)
- [Recommend](vmaf-tune-recommend.md),
  [benchmark](vmaf-tune-benchmark.md)
- [Predictor training and sidecar](vmaf-tune-predict.md)
