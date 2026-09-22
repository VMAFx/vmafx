- `praetorctl audit` passes again on this branch. Two gates had gone stale
  against the tree rather than against any defect in it. The HISS baseline
  `.standards-baseline.json` is keyed `file:line`, so the refactors on this
  branch re-fingerprinted findings that had never moved and the audit read
  them as new; it is re-recorded from a clean clone of the branch and drops
  from 1411 to 929 infractions, which the recorder will not allow to rise.
  Separately, `README.md` carried a hand-written "Standards & Governance"
  table instead of the marker-delimited block the engine renders and
  verifies, so the README gate reported the managed block as missing — a
  condition that predates this branch and also holds on `master`, and that
  only became visible once the HISS gate stopped failing first. The block is
  restored with the engine's own content, the prose around it now says it is
  generated, and the stale `standardsctl` command names it advertised are
  corrected to `praetorctl`.
