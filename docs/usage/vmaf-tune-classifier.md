<!-- markdownlint-disable MD060 -->
# Content-Aware Video Classification

The `vmaf-tune classify` tool analyses source video clips to extract spatial complexity
(Spatial Information, SI) and motion intensity (Temporal Information, TI), probes
container geometry and dynamic range, and optionally performs semantic genre tagging
via local vision models.

The resulting metadata tags enable automatic routing of encoding presets, rate-control
knobs, and VMAF reference models without manual human inspection ([ADR-0618](../adr/0618-content-aware-classifier.md),
[ADR-3374](../adr/3374-vmaf-tune-classifier-vision-fails-by-name.md)).

## Quickstart

### 1. Basic signal analysis (no GPU or model required)

To extract SI, TI, resolution, frame rate, duration, and dynamic range:

```bash
vmaf-tune classify source.mp4
```

Example human-readable output:

```text
Source: source.mp4
Resolution: 1920x1080 @ 24.00 fps (10.00s)
Dynamic range: SDR
Spatial Information (SI): mean=58.20, p95=72.10 (detail: medium)
Temporal Information (TI): mean=8.40, p95=11.50 (motion: low)
Genre: not-run (not-run)
```

### 2. Machine-readable JSON output

Pass `--json` to output a strict JSON payload:

```bash
vmaf-tune classify source.mp4 --json
```

```json
{
  "si_mean": 58.2,
  "si_p95": 72.1,
  "ti_mean": 8.4,
  "ti_p95": 11.5,
  "hdr": null,
  "resolution": [1920, 1080],
  "fps": 24.0,
  "duration": 10.0,
  "motion_class": "low",
  "detail_class": "medium",
  "genre": null,
  "genre_source": "not-run"
}
```

### 3. Genre classification with local VLM

Genre tagging requires an explicit `--genre` opt-in and an accessible local
OpenAI-compatible vision endpoint on the local machine or a private network:

```bash
vmaf-tune classify source.mp4 --genre --vlm-url http://127.0.0.1:8000 --vlm-model <vision-model>
```

## How it works

### Signal complexity & ITU-T P.910 metrics

Spatial Information (SI) and Temporal Information (TI) are extracted via FFmpeg's
built-in `siti` filter ([tools/vmaf-tune/src/vmaftune/classify.py](../../tools/vmaf-tune/src/vmaftune/classify.py)):

- **Spatial Information (SI)**: Measures the spatial gradient across luminance
  planes using Sobel filtering. High SI corresponds to sharp edges, fine texture,
  or complex visual detail.
- **Temporal Information (TI)**: Measures luminance differences between consecutive
  frames. High TI indicates rapid movement, camera pans, or complex motion.

Content is stratified into named classes according to ITU-T P.910 thresholds:

| Metric | Class | Threshold Condition | Typical Content |
|---|---|---|---|
| **TI** | `static` | $\text{TI} < 1.0$ | Title cards, static slides, still scenes |
| **TI** | `low` | $1.0 \le \text{TI} < 12.0$ | Dialogue, talking heads, slow pans |
| **TI** | `high` | $12.0 \le \text{TI} < 30.0$ | Standard live-action, walking, camera tracking |
| **TI** | `very_high` | $\text{TI} \ge 30.0$ | Sports, fast action, complex handheld motion |
| **SI** | `low` | $\text{SI} < 50.0$ | Flat graphics, smooth anime fills, blurred backgrounds |
| **SI** | `medium` | $50.0 \le \text{SI} < 100.0$ | Standard natural scenes, moderate detail |
| **SI** | `high` | $\text{SI} \ge 100.0$ | Intricate textures, foliage, fine grain, noise |

### Dynamic range & container metadata

Container geometry, frame rate, duration, and HDR characteristics are probed using
FFprobe and `detect_hdr` ([tools/vmaf-tune/src/vmaftune/hdr.py](../../tools/vmaf-tune/src/vmaftune/hdr.py)).
HDR detection verifies color transfer (`pq`, `hlg`), primaries (`bt2020`), and mastering
metadata.

## Genre classification and fail-by-name policy

Genre tagging operates under strict operational and security invariants ([ADR-3374](../adr/3374-vmaf-tune-classifier-vision-fails-by-name.md)):

1. **Fail-by-name**: If `--genre` is enabled but no VLM endpoint is provided
   or the endpoint does not support image input, `vmaf-tune` raises
   `VisionBackendUnavailable` naming the missing component and configuration
   key. It **never** silently coerces unclassified content to `unknown`.
2. **Local-only frame transport**: The VLM backend (`OpenAICompatVlm`)
   enforces that the endpoint host is a loopback address (`localhost`,
   `127.0.0.1`, `::1`) or a private RFC 1918 subnet (`10.0.0.0/8`,
   `172.16.0.0/12`, `192.168.0.0/16`). Requests to remote internet hosts are
   refused before any network connection is initiated.
3. **Exact genre vocabulary**: The supported genre labels are:
   - `movie`: Feature films, scripted drama
   - `tv`: Broadcast television, serial programming
   - `anime`: 2D/3D animation, cel graphics
   - `adult`: Lawful adult entertainment (categorized as a standard genre)
   - `sports`: Athletic events, competitive sports broadcasts
   - `unknown`: Explicit label for indeterminate visual evidence

Any VLM output outside this vocabulary raises a validation error rather than
being coerced.

## Sidecar caching

Classification results are cached in a JSON sidecar file (`<source>.classify.json`).
The cache key incorporates the clip fingerprint and classifier version:

- If the optional `fingerprint` extra is installed
  (`vmaf-tune[fingerprint]`) and the clip has $\ge 30$ frames, the perceptual
  video fingerprint
  ([ADR-3372](../adr/3372-perceptual-video-fingerprint.md)) is used.
- Otherwise, an exact SHA-256 digest of the source content is used.
- If a sidecar file is corrupted or unreadable, `vmaf-tune` logs a warning naming
  the file and automatically recomputes the classification.

## Python API

```python
from pathlib import Path
from vmaftune.classify import ContentTags, OpenAICompatVlm, classify

# 1. Deterministic signal classification (no VLM)
tags: ContentTags = classify(Path("clip.mp4"))
print(f"Motion: {tags.motion_class} (TI p95={tags.ti_p95:.2f})")
print(f"Detail: {tags.detail_class} (SI mean={tags.si_mean:.2f})")

# 2. Genre classification with local VLM
vlm = OpenAICompatVlm(base_url="http://127.0.0.1:8000", model="<vision-model>")
tagged = classify(Path("clip.mp4"), genre=True, vlm=vlm)
print(f"Genre: {tagged.genre} (source: {tagged.genre_source})")
```

## Verification

The classification suite tests positive, negative, and boundary conditions:

```bash
cd tools/vmaf-tune
python3 -m pytest -q tests/test_classify.py
```

Test coverage is verified in [tools/vmaf-tune/tests/test_classify.py](../../tools/vmaf-tune/tests/test_classify.py).
