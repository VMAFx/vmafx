- `vmaf-tune compare`'s `--target-vmafs` default now ships the
  premium-archival sweep `94,96,97,98` (ADR-0538, supersedes
  ADR-0534's `75,80,85,90,93`). The fork's primary user encodes
  archival masters at VMAF >= 95 exclusively, and the previous
  streaming/broadcast default produced an R-Q chart with no points
  in the user's actionable range. Pass `--target-vmafs 75,80,85,90,93`
  explicitly to recover the streaming sweep; the CSV / JSON row
  schema is unchanged.
- `vmaf-tune` target-VMAF bisect now reaches VMAF >= 95 reliably
  (ADR-0538). The bisect's default search window is the encoder's
  absolute CRF range (`libx264` / `libx265` -> `0..51`,
  `libvpx-vp9` / `libaom-av1` / `libsvtav1` -> `0..63`) instead of
  the codec adapter's narrow perceptually-informative
  `quality_range` (e.g. `libx265 = (15, 40)`, `libsvtav1 = (20, 50)`).
  The adapter validator's CRF gate is bypassed inside
  `_encode_and_score` and replaced with an explicit absolute-range
  check; preset validation is unchanged. Pre-fix, premium-archival
  targets on `libx265` and `libsvtav1` returned `ok=false` with an
  "unreachable" error because the search window opened above the
  CRF needed to clear the target. The corpus-generator path in
  `corpus.py` still calls `adapter.validate` unchanged; only the
  bisect search loop was widened.
