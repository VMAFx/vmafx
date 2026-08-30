# Research-0653: CHUG Display Profile Training

## Question

How should CHUG HDR MOS training consume display and panel context without
turning a local viewing setup into corpus source data?

## Findings

- CHUG gives the fork HDR subjective MOS rows, but the current feature rows are
  still clip and ladder facts. They do not describe the target display used for
  local model experiments.
- The signal-mix audit identified HDR display and panel context as a blind
  spot. The missing signals are not more canonical-6 statistics; they are
  viewing-context variables such as peak luminance, black level, color-volume
  coverage, ambient light, panel class, local dimming, and tone-mapping.
- Hard-coding one display into CHUG JSONL would make the corpus look more
  authoritative than it is and would require row rewrites for every target
  panel experiment.
- A trainer-side profile keeps the source rows unchanged while making the
  experiment reproducible through the emitted manifest.
- Future multi-display HDR corpora need row-local display values to win over a
  global fallback profile; otherwise those datasets would lose their main
  distinguishing axis.

## Result

Use a `--display-profile-json` training input and a named
`chug-hdr-display-v1` schema. The profile normalizer accepts common raw field
names (`peak_nits`, `black_nits`, `ambient_lux`, `panel_type`,
`tone_mapping`, BT.2020/P3 coverage) and projects them onto stable numeric
feature columns. The CHUG wrapper auto-selects the display-aware schema only
when the profile flag is supplied and no explicit schema was requested.

## Verification

Regression tests cover profile normalization, row-local display value
precedence, the `chug-hdr-display-v1` feature order, CHUG wrapper schema
selection, and manifest recording of the normalized profile plus source sha256.
