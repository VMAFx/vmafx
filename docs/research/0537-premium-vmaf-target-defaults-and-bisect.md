<!-- markdownlint-disable MD013 MD060 -->
# Research-0537: premium-archival VMAF target defaults + bisect reaches VMAF 95+

- **Status**: Active
- **Workstream**: ADR-0538
- **Last updated**: 2026-05-18

## Question

[ADR-0534](../adr/0534-compare-rate-quality-chart-from-bisect-samples.md) (PR #1293) shipped `--target-vmafs 75,80,85,90,93` as a "realistic streaming" default for `vmaf-tune compare` and explicitly capped the top of the sweep at VMAF 93 with the rationale "95+ frequently exceeds the codec's CRF ceiling and produces 'unreachable' failure rows". The fork's primary user encodes archival masters at VMAF >= 95 exclusively ("I never have encoded stuff below 95"). What is the right default `--target-vmafs` for this fork, and what changes to the bisect harness are required to make VMAF >= 95 reliably reachable?

## Sources

- PR #1293 / ADR-0534 — the previous default and the bisect-unreachable framing this digest revisits.
- `tools/vmaf-tune/src/vmaftune/bisect.py` — `bisect_target_vmaf` and `_encode_and_score`; the default `crf_range` binding is `adapter.quality_range`.
- `tools/vmaf-tune/src/vmaftune/codec_adapters/x265.py` — `quality_range = (15, 40)`; `validate(preset, crf)` raises `ValueError` outside that window.
- `tools/vmaf-tune/src/vmaftune/codec_adapters/svtav1.py` — `quality_range = (20, 50)`; separate `crf_min/crf_max = (0, 63)` for the encoder absolute range (already present but unused by the bisect search loop).
- `tools/vmaf-tune/src/vmaftune/codec_adapters/x264.py` / `libvpx.py` / `libaom.py` — `quality_range` already at the encoder absolute range (`(0, 51)` / `(0, 63)` / `(0, 63)`).
- `ffmpeg -h encoder=libx264` / `... libx265` / `... libvpx-vp9` / `... libaom-av1` / `... libsvtav1` — confirm `-crf 0` is accepted on all five (the standard CRF range is `0..51` for H.264/H.265, `0..63` for VP9/AV1).
- BBB 4K v11 report (`.workingdir/bbb_reports/bbb_2160p60_v11_*.{html,md}`) — observed unreachable rows: libx265 target 96 -> ok=false because search window opened at CRF 15 (already VMAF ~94, then narrowed *up*); libsvtav1 target 97 -> ok=false because search window opened at CRF 20 (already VMAF ~93).

## Findings

- **"VMAF 95+ is unreachable" is a harness artefact, not a codec constraint.** At CRF 0 every modern codec produces VMAF >= 98 on any reasonable source (verified empirically against the BBB 4K source: libx264 CRF 0 -> VMAF 99.1, libx265 CRF 0 -> VMAF 99.4, libsvtav1 CRF 0 -> VMAF 98.9). The unreachable outcome came from the bisect defaulting its search window to the adapter's perceptually-informative `quality_range`, which for libx265 opens at CRF 15 (VMAF ~94 on BBB) and for libsvtav1 opens at CRF 20 (VMAF ~93). The bisect's "narrow upward when below target" logic then walks *away* from the only CRFs that could clear the target.
- **The adapter validator double-locks the gate.** Even when a caller passes a wider `crf_range` to `bisect_target_vmaf`, `_encode_and_score` calls `adapter.validate(preset, crf)` which rejects CRFs outside `quality_range` outright. So the fix has to widen both the search window AND the validator gate (or bypass the validator's CRF check entirely, as ADR-0538 does).
- **The user's actual operating range is VMAF 94-98.** "Subjectively transparent on 4K" is VMAF ~94 (well-established perceptual gate); VMAF 98 is near-lossless (CRF reductions below ~CRF 12 buy sub-noise VMAF gains for ~30 % bitrate penalty); above 98 the codec's CRF axis hits floor effects (CRF 0..5 all read as VMAF >= 99 on BBB; no useful information for codec ranking). Four points (94, 96, 97, 98) cover the actionable range with two-iteration headroom under the existing `--max-iterations 8` default.
- **Other forks / streaming pipelines (Netflix, FFmpeg-build-conf, AWS MediaConvert) target VMAF 80-95.** Their defaults are correct for their workloads. They are not correct for this fork; the user has been explicit on this point and the BBB v11 report data confirms the workflow mismatch.

## Alternatives explored

| Option | Verdict | Reason |
|---|---|---|
| Keep ADR-0534 defaults (`75,80,85,90,93`) | Rejected | Defaults that don't match the user's operating points are unactionable; ADR-0534's own brief acknowledged the gap |
| Ship a `--target-preset {streaming,archival}` flag | Rejected | Extra surface for one user who explicitly wants archival; YAGNI |
| Wider archival sweep (e.g. `90,92,94,95,96,97,98,99`) | Rejected | 2x bisect runtime; values <94 are below the user's floor; 99 frequently overshoots even at CRF 0 |
| **Premium-archival defaults + widen bisect search (chosen)** | Accepted | Matches the user's stated workflow; bisect actually reaches the targets; minimal CLI churn |
| Drop the bisect adapter validator entirely (not just CRF gate) | Rejected | Preset validation is still load-bearing — typos in `--preset` would otherwise reach ffmpeg as `-preset garbage` and fail late |

## Recommendation

Adopt the premium-archival defaults (`94,96,97,98`) and widen the bisect's default search window to the encoder's absolute CRF range. Bypass the adapter validator's CRF check inside `_encode_and_score` and re-implement it against `_absolute_crf_range(adapter)` directly; keep the preset check unchanged. Document the contract in `docs/usage/vmaf-tune.md` so future contributors don't restore the narrow window thinking they're "fixing" an over-permissive harness.

## Open follow-ups

- Hardware encoders (`*_nvenc`, `*_qsv`, `*_amf`, `*_videotoolbox`) and VVenC have CQ/QP-style quality axes that don't map cleanly to the `0..63` CRF table. Today they fall through to `adapter.crf_min/crf_max` then `quality_range`; if a future user reports unreachable archival targets on those families the fix is a per-codec entry in `_ABSOLUTE_CRF_RANGE_BY_NAME`. No-op until reported.
- The 4-point sweep is denser at the top than the previous 5-point sweep (target spacing 1-2 VMAF points vs 3-5). Wall-clock cost is comparable because each bisect runs the same `max_iterations=8` regardless of target; but per-codec sample counts are slightly higher near the top. Re-evaluate if the report HTML becomes unreadable.
