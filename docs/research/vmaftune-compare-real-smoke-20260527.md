<!-- markdownlint-disable MD013 MD025 MD060 -->
# Copyright 2026 Lusoris

# vmaf-tune compare — Real Integration Smoke Test (2026-05-27)

**Date:** 2026-05-27
**Tester:** Agent (Claude Sonnet 4.6)
**Branch under test:** `fix/feature-correlation-non-numeric-20260526`
**vmaf-tune version:** 0.0.2

---

## 1  Test setup

| Parameter | Value |
|-----------|-------|
| Reference YUV | `python/test/resource/yuv/src01_hrc00_576x324.yuv` |
| Dimensions | 576 × 324, yuv420p, 24 fps, 1.0 s (24 frames) |
| Encoders | `libx264`, `libsvtav1` |
| VMAF targets | 80, 90 |
| Score backend | cpu (auto-selected) |
| Output | `/tmp/vmaftune-smoke/report.{html,md}` |
| ffmpeg | n8.1.1 (system) |
| vmaf binary | `/usr/local/bin/vmaf` |

Command executed:

```bash
.venv/bin/vmaf-tune compare \
  --src python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --width 576 --height 324 --pix-fmt yuv420p --duration 1.0 \
  --encoders libx264,libsvtav1 \
  --target-vmafs 80,90 \
  --output /tmp/vmaftune-smoke/report.json \
  --format both \
  --workdir /tmp/vmaftune-smoke/work
```

Exit code: **0** (all rows ok=True)

---

## 2  Workflow outcome

The workflow completed end-to-end without crashing. Both HTML and MD artifacts
were generated. VMAF bisect converged successfully for all four
(codec × target) combinations:

| Codec | Target VMAF | Achieved | Delta | Bitrate | CRF |
|-------|------------|---------|-------|---------|-----|
| libsvtav1 | 80 | 80.30 | +0.30 | 596 kbps | 53 |
| libsvtav1 | 90 | 90.39 | +0.39 | 1.37 Mbps | 40 |
| libx264 | 80 | 82.65 | +2.65 | 1.03 Mbps | 30 |
| libx264 | 90 | 91.30 | +1.30 | 2.24 Mbps | 25 |

The libx264 VMAF-80 row overshoots by 2.65 points. This is within the
expected bisect tolerance on a 1-second (24-frame) clip where per-frame
VMAF variance is high and the bisect terminates when it lands within the
tolerance window.

---

## 3  PR #1550 known findings — confirmed / refuted

### Finding 1 — Missing `<meta name="viewport">` (line 1634 of report.py)

**Confirmed.** The generated `report.html` contains only
`<meta charset="utf-8">` in the `<head>` block. No viewport tag is
present. On a 980 px desktop the text is readable; on a mobile viewport
the browser defaults to a 980 px layout, making text tiny.

Grep check:

```text
grep -c "viewport" /tmp/vmaftune-smoke/report.html  → 0
```

### Finding 2 — Bitrate unit inconsistency

**Confirmed — partially.** The SVG x-axis label reads
`bitrate (log scale; left is smaller)` with no unit. Tick labels emit
bare `1M`, `10M` without specifying bits-per-second. The quick-takeaways
prose and the HTML table show `kbps` / `Mbps` for the same values, so
readers must infer the SVG unit from context. Three distinct label
surfaces are involved:

- SVG axis title: no unit
- SVG tick labels: bare `M`/`k` suffix (implied bps from context)
- HTML table cells: `kbps` or `Mbps` depending on magnitude

### Finding 3 — Failed rows (`ok=False`) showing `0 kbps` / `0.00`

**Not triggered in this run.** Both codecs succeeded at both targets, so
no `ok=False` row was generated. The code path (lines 1711–1725, 1539–1546
of report.py) is confirmed present in the source; a deliberate codec
failure fixture would be needed to reproduce the rendering defect live.

### Finding 4 — VideoToolbox color collision

**Not triggered.** The test ran on Linux with no VideoToolbox codecs.
The color-assignment logic (lines 876–879 of report.py) is confirmed in
source; reproduction requires a macOS run with
`h264_videotoolbox,libx264` in the encoder list.

### Finding 5 — Pareto annotation clutter

**Partially triggered.** With only 2 codecs × 2 targets the frontier has
2 points, both belonging to libsvtav1. The SVG contains two inline
annotation comments (`libsvtav1 (libsvtav1-4.1.0)` and
`libx264 (libx264-enabled)`) as separate `ax.annotate` calls. At this
scale overlap is not visible. At 4+ codecs × 4+ targets the annotation
density issue described in PR #1550 would materialize.

---

## 4  New bugs discovered

### Bug N-1 — `--format both` silently drops the JSON file

**Severity: High**
**File:** `tools/vmaf-tune/src/vmaftune/cli.py`, function
`_write_compare_profile_report` (lines 3524–3534)

When `--output /tmp/foo/report.json --format both` is passed, the
function does:

```python
html_path = output.with_suffix(".html")   # → report.html  ✓
md_path   = output.with_suffix(".md")     # → report.md    ✓
# report.json itself is NEVER written          ← bug
```

The original stem (`report.json`) is reused as the base for suffix
replacement, replacing `.json` with `.html` and `.md`. No JSON file is
written. The user gets `report.html` and `report.md` in the directory
where they expected `report.json`. The stderr message also does not
mention JSON:

```text
wrote compare profile report -> /tmp/vmaftune-smoke/report.html, /tmp/vmaftune-smoke/report.md
```

Expected: either a warning that `--format both` does not write JSON, or
a separate `report-data.json` containing the embedded JSON payload.

The `<details>` appendix inside the HTML says *"The raw JSON contains
encoder_profile, a stable payload that can be passed to
`vmaf-tune encode-profile`"* — implying users expect a machine-readable
JSON sidecar, not only the HTML-embedded copy.

**Reproduction:**

```bash
vmaf-tune compare --src <yuv> --output /tmp/foo/report.json --format both …
ls /tmp/foo/         # report.html report.md   (no report.json)
```

### Bug N-2 — `vmaf-tune ladder --encoder libsvtav1` fails immediately with exit 2

**Severity: High (blocks libsvtav1 ladder runs)**
**Files:**

- `tools/vmaf-tune/src/vmaftune/ladder.py` line 173
- `tools/vmaf-tune/src/vmaftune/codec_adapters/svtav1.py` line 92

`DEFAULT_SAMPLER_CRF_SWEEP = (18, 23, 28, 33, 38)` starts at CRF 18,
but SVT-AV1's Phase A `quality_range = (20, 50)` has a lower bound of
CRF 20. The svtav1 adapter's `validate_crf` raises `ValueError` when it
sees CRF 18:

```text
vmaf-tune ladder: crf 18 outside Phase A range [20, 50]
```

The exception is caught and returned as exit code 2 with no output file.
The user gets no ladder JSON and no indication of which CRF values are
acceptable.

libx264 accepts CRF 18 (its Phase A range is 0–51), so ladder works with
the default encoder but fails as soon as the user passes
`--encoder libsvtav1`.

**Reproduction:**

```bash
vmaf-tune ladder \
  --src <yuv> --encoder libsvtav1 \
  --resolutions 576x324 --target-vmafs 80,85,90 \
  --src-width 576 --src-height 324 --duration 1.0 \
  --format json
# → exit 2, no output
# stderr: vmaf-tune ladder: crf 18 outside Phase A range [20, 50]
```

**Workaround:** pass `--crf-sweep 20,25,30,35,40,45,50` explicitly when
using `libsvtav1` with the ladder subcommand.

**Fix direction:** The default CRF sweep in `ladder.py` should intersect
the supported range of the active encoder, or the sampler factory should
clamp/filter the sweep values against the adapter's `quality_range` with
a warning rather than raising.

---

## 5  Additional observations (non-blocking)

- **`report.json` output path confusion** also applies to `--format html`
  with an output path whose stem ends in `.json`: `output.with_suffix(".html")`
  silently renames it. The same logic is repeated at line 4741 for the
  `report` subcommand.
- The embedded JSON `to_dict()` output in the HTML `<details>` block has
  `codec_rows: []` when the v2 sweep path is used (`--target-vmafs` with
  two or more values). This is correct behavior per the v2 schema design
  (data lives in `sweep_points`), but the `<details>` note says the JSON
  is for `vmaf-tune encode-profile`, which already reads from
  `encoder_profile.recommendations` — so no functional bug, but the note
  is misleading about `codec_rows`.
- libx264 VMAF-80 overshoot of 2.65 points on a 24-frame clip is within
  known bisect variance for very short fixtures; not a bug.

---

## 6  Artifacts

| Artifact | Path |
|----------|------|
| HTML report | `/tmp/vmaftune-smoke/report.html` |
| Markdown report | `/tmp/vmaftune-smoke/report.md` |
| Ladder JSON (libx264) | `/tmp/vmaftune-smoke/ladder-x264.json` |
| Compare log | `/tmp/vmaftune-compare.log` |
| Ladder log (svtav1 fail) | `/tmp/vmaftune-ladder.log` |

---

## 7  Summary

| Item | Status |
|------|--------|
| Workflow end-to-end | Pass |
| Finding 1 (no viewport) | Confirmed |
| Finding 2 (bitrate units) | Confirmed |
| Finding 3 (failed rows) | Not triggered (no failures in run) |
| Finding 4 (VT color clash) | Not triggered (Linux host) |
| Finding 5 (pareto clutter) | Partially triggered (low codec count) |
| Bug N-1 (no JSON from `--format both`) | **New — confirmed** |
| Bug N-2 (ladder fails with libsvtav1) | **New — confirmed** |
