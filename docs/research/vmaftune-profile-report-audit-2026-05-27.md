<!-- markdownlint-disable MD013 MD036 MD060 -->
# Research — vmaf-tune profile-report renderer audit (T-VMAFTUNE-PROFILE-REPORT-AUDIT)

**Date:** 2026-05-27
**Scope:** `tools/vmaf-tune/src/vmaftune/report.py` (1 976 lines)
**Audit method:** static code review + synthetic fixture generation (`sample_sweep`, `sample_compare`) with 4 codecs × 3 VMAF targets, failed rows (NaN and zero-value), and all four sections (sweep, compare, ladder, shots).

---

## 1. Graph density

**Finding:** the rate-quality sweep chart (8 × 4.5 in, 110 dpi) plots bisect-sample curves at `markersize=3.5` / `linewidth=1.2`. With four codecs and ~10 sampled CRF probes each, curves overlap adequately. However:

- The pareto-frontier annotator (lines 1127–1136) emits a text label (`ax.annotate(p.codec, …)`) for **every** frontier point. When the same codec wins at multiple VMAF targets the labels stack at nearby chart coordinates. In a 3-target sweep where libsvtav1 wins at VMAF 85 and 90, two `libsvtav1` annotations appear 15–30 px apart; at narrow viewport widths they collide.
- The bisect-sample marker (`s=3.5`) is nearly invisible when the chart is rendered at 300 px wide (e.g. mobile or narrow column). The picked-CRF hollow markers (`s=80`) remain visible.

**Recommended fix (`report.py` lines 1127–1136):** Deduplicate pareto annotations by codec — show the label only at the lowest-bitrate frontier point for each codec, or use a legend entry instead of per-point annotation.

---

## 2. Axis scaling

**Finding:** All four chart types use a log x-axis for bitrate (via `_apply_bitrate_xaxis(log_scale=True)`). This is correct. The ladder chart also uses log-scale x. The codec-compare bar chart uses a linear y-axis for bitrate — acceptable for a bar chart but note that very small codecs (< 200 kbps) would be nearly invisible next to a 4 Mbps bar.

No free-range / locked-range concern was found; `_set_padded_ylim` pads the VMAF y-axis sensibly in all paths.

---

## 3. Labels

Three distinct label problems:

**3a. Missing unit on sweep and compare-bar x/y axis labels**

| Chart | Axis | Label text | Unit present |
|---|---|---|---|
| Rate-quality sweep | x | `bitrate (log scale; left is smaller)` | No |
| Codec compare bar | y | `bitrate (lower is smaller)` | No |
| ABR ladder | x | `bitrate (kbps)` | Yes |

The axis titles for the sweep and compare-bar charts omit the unit. The tick formatter (`_bitrate_tick_label`) emits `500k` / `1M` / `2.5M`, where `k` means kbps and `M` means Mbps, but these suffixes are not defined anywhere in the chart and conflict with the table formatter (`_fmt_kbps`) which uses the spelled-out `kbps` / `Mbps`. A reader who sees `1M` on the axis and `1.00 Mbps` in the table may correctly infer the equivalence, but it requires extra reasoning.

**Recommended fix (`report.py` lines 1141–1142, 823–824):** Change sweep axis label to `bitrate (kbps, log scale; left is smaller)` and compare-bar axis to `bitrate (kbps)`; change tick suffixes in `_bitrate_tick_label` from bare `M` / `k` to `Mbps` / `kbps` (or `Mb/s` / `kb/s` for brevity).

**3b. Pareto-frontier annotations use only the codec token (e.g. `libsvtav1`) without a bitrate or VMAF label.** A label like `libsvtav1 @ 1.3 Mbps` would be more informative and resolve the target-ambiguity when labels overlap.

**3c. Legend for bisect-sample chart omits picked-CRF hollow markers.** The scatter call for picked-CRF overlay (lines 1073–1081) is not given a `label=` parameter, so it does not appear in the chart legend. The reader cannot distinguish the curve samples from the picked rungs without reading the title.

**Recommended fix (line 1073):** Add `label="picked CRF"` to the scatter call.

---

## 4. Failed-row affordances

Two separate problems were identified:

**4a. Zero-value failed rows render as valid data in tables.**

`CodecRow('libvpx-vp9', '1.14', 33, 0, 0, 0, False, 'timeout')` (ok=False, bitrate_kbps=0.0, vmaf=0.0) renders as `0 kbps` / `0.00` in the table because `_is_missing` returns False for 0.0 (it only guards against NaN and Inf). The row is correctly styled with a `FAIL: timeout` status badge, but the numeric cells show `0 kbps` rather than `—`. A reader scanning the bitrate column may misread 0 kbps as "successfully encoded at minimal bitrate."

`CodecRow` with NaN values (hardware-unavailable rows) correctly renders as `—` because the NaN path is guarded by `_is_missing`. Only the zero-path is broken.

**Recommended fix (`report.py` `_row_html` / `render_markdown` table section):** Apply `ok` guard before formatting numeric cells: if `not row.ok`, render CRF/bitrate/vmaf as `—` regardless of value. Alternatively, change `_is_missing` to treat 0.0 as missing when the row is `ok=False` — but the row-level guard is cleaner.

**4b. Failed rows are silent in charts.** In the sweep chart, failed rows are excluded from the curve (correct — no data to plot), but there is no visual marker, dashed boundary, or annotation at the expected bitrate position indicating a target was attempted and failed. A reader may not know whether libsvtav1 at VMAF 95 simply was not attempted or whether it failed. The table carries the `Status` column, but the chart is standalone.

**Recommended fix:** Consider an `axvspan` or `axhline` dashed annotation at the missing target's last-known bitrate (or a fixed "attempted; failed" label at the y-position of the target VMAF). This is a visual enhancement, not a correctness fix; a test change note in the PR description would suffice.

---

## 5. Bitrate units

Three distinct unit surfaces:

| Surface | Format | Example |
|---|---|---|
| Table cells (`_fmt_kbps`) | `kbps` / `Mbps` (spelled out) | `1.30 Mbps` |
| Chart tick labels (`_bitrate_tick_label`) | `k` / `M` (abbreviated) | `1M`, `500k` |
| Sweep x-axis title | no unit | `bitrate (log scale; …)` |
| Compare-bar y-axis title | no unit | `bitrate (lower is smaller)` |
| Ladder x-axis title | `kbps` | `bitrate (kbps)` |
| Encoder-profile JSON field | `bitrate_kbps` (field name is the unit) | raw float |

The table formatter and tick formatter use different abbreviation styles for the same unit. A unified convention should be picked (recommendation: `kbps` / `Mbps` throughout, matching the table formatter) and applied to `_bitrate_tick_label` and all axis title strings.

File: `report.py` lines 892–897 (`_bitrate_tick_label`), 785 (`ladder`), 1141 (`sweep`), 823 (`compare-bar`).

---

## 6. Mobile layout

**6a. Missing `<meta name="viewport">` tag.** The HTML template (line 1629–1708) has no viewport meta. Mobile browsers default to 980 px layout width, which causes the `max-width: 1100px` body to zoom out and make text tiny on phones. This is the single highest-impact mobile fix.

**Recommended fix (line 1634):** Add `<meta name="viewport" content="width=device-width, initial-scale=1">` to the `<head>`.

**6b. Inline SVG charts carry fixed-pt dimensions** (`width="494.203125pt"`, `height="309.354375pt"`) as HTML attributes on the `<svg>` element. The CSS rule `.chart svg { max-width: 100%; height: auto; }` overrides these via CSS, so charts do scale down on narrow viewports. However, the SVG's intrinsic `viewBox` is correct and the CSS rule covers the responsive case. This is a low-severity issue — the CSS override already works — but the explicit `width`/`height` attributes create redundant-but-overridden dimensions that confuse some parsers and PDF renderers.

**Recommended fix:** Strip `width=` / `height=` attributes from the SVG root before embedding (post-process `_render_chart_svg`). Alternatively, use `fig.set_size_inches` with a fixed inch size that maps to a desired pixel width, and remove the attributes in a post-processing step.

**6c. No media query for narrow (`< 600 px`) screens.** The `max-width: 1100px` body and `.kv { grid-template-columns: 12rem 1fr }` source key-value grid would collapse gracefully on phones with `overflow-x: auto` on tables. The codec-chip inline-flex elements use `white-space: nowrap` which forces the row to be at least as wide as the codec name. No explicit wrapping behaviour is defined for narrow screens.

---

## 7. Deterministic colors

The `_CODEC_COLOURS` map (lines 858–880) contains a collision: `h264_videotoolbox`, `hevc_videotoolbox`, and `av1_videotoolbox` share the same colors as `libx264`, `libx265`, and `libsvtav1` respectively (all map to `_CODEC_PALETTE[0]`, `[1]`, `[2]`). In a report that compares software and VideoToolbox hardware encoders for the same codec family, two lines on the sweep chart share the same hue.

**Recommended fix (lines 876–879):** Assign VideoToolbox encoders to palette slots 15–17 (adding three entries to `_CODEC_PALETTE` and `_CODEC_COLOURS`). Color tokens `#e6b89c`, `#c4e8c2`, `#b0c4de` are distinct from the current 15-slot palette and could fill this gap.

---

## 8. Markdown / HTML parity

Both renderers produce the same logical sections:

- Source metadata table — parity: identical
- Quick takeaways bullets — parity: identical (text only)
- Codec guide — parity: identical structure; HTML has inline-block codec chips with colour; MD has hyperlinked `[BADGE codec](url)` chips
- Codec rate-quality sweep / compare table — parity: identical data; HTML has `<span class="tag ok/bad">` pills; MD shows plain `OK` / `FAIL: …` text
- ABR ladder — parity: identical
- Per-shot tuning — parity: identical
- Raw JSON dump — parity: HTML uses `<details><pre>`; MD uses `<details><summary>…</summary>\n```json`

**HTML-only features:**

- Dark/light mode via `prefers-color-scheme` media query
- `overflow-x: auto` table scroll containers
- Codec chip colour via `--codec-colour` CSS variable (MD shows only a text chip)
- `<svg>` charts (MD uses base64 PNG by default; sidecar PNG via `assets_dir`)

**MD-only features:** none; all MD sections have HTML equivalents.

**Parity gap:** MD base64 PNG images are not diff-friendly (every re-render produces a different PNG for the same data because matplotlib embeds a `dc:date` timestamp in the SVG/PNG metadata). This means two renders of the same `ReportData` will produce different MD files. The `--strip` option to remove the `dc:date` from SVGs is not exercised; PNGs embed creation time in their EXIF block.

---

## 9. Artifact packaging

`write_report_outputs` (lines 693–713) writes `.html` and/or `.md` files only. No standalone sidecar `.json` file is written. The `encoder_profile` JSON payload is embedded inside the HTML `<details><pre>` block (HTML-escaped) and inside the MD `<details><summary>` block (in a fenced code block), requiring the user to expand the section and manually extract the JSON if they want to pipe it to `vmaf-tune encode-profile`.

**Recommended fix:** Add an optional `json_sidecar: bool = False` flag to `write_report_outputs` that writes `<output>.json` alongside the HTML/MD. The sidecar would contain the exact `data.to_dict()` payload (the `encoder_profile` sub-key plus the full structured dump), so `vmaf-tune encode-profile --profile report.json` works without user extraction.

**Reproducer command:** The `encoder_profile` section in both HTML and MD contains a guidance string referencing `vmaf-tune encode-profile`. This is accurate and surfaces the right command. The gap is that the command cannot be executed without first extracting the JSON from the report artifact.

---

## 10. Summary of recommended follow-up work (priority order)

| # | Issue | File | Lines | Severity |
|---|---|---|---|---|
| 1 | Missing `<meta name="viewport">` | `report.py` | 1634 | High |
| 2 | Bitrate axis unit inconsistency (sweep, compare-bar) | `report.py` | 892, 1141, 823 | Medium |
| 3 | Failed row with `ok=False` + numeric-0 shows `0 kbps`/`0.00` | `report.py` | 1711–1725, 1539–1546 | Medium |
| 4 | VideoToolbox color collision with software codecs | `report.py` | 858–879 | Medium |
| 5 | Pareto annotation clutter (same codec at multiple targets) | `report.py` | 1127–1136 | Low–Medium |
| 6 | No sidecar JSON file written | `report.py` | 693–713 | Low–Medium |
| 7 | Picked-CRF scatter has no legend entry | `report.py` | 1073–1081 | Low |
| 8 | Pareto annotations lack bitrate context | `report.py` | 1128–1135 | Low |
| 9 | SVG `dc:date` timestamp breaks MD diff-stability | `report.py` | 750–772 | Low |
| 10 | No `axvspan`/marker for failed-target in sweep chart | `report.py` | 1137–1140 | Low |

---

## References

- `tools/vmaf-tune/src/vmaftune/report.py` (audited)
- `tools/vmaf-tune/tests/test_report.py` (smoke tests)
- ADR-0516 vmaf-tune compare rate-quality sweep
- ADR-0530 bisect-samples additive field
- ADR-0531 shot-timeline last-band visibility fix
- ADR-0498 matplotlib fallback
- Synthetic fixtures generated at `.workingdir2/vmaftune-profile-audit-20260527/`
