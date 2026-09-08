- Allow `tidy-ratchet.py --only <TU> --write` to tighten measured source
  allowances while preserving unmeasured entries and full-report metadata.
  Exact coverage, matching tool version, parse/compile success, debt monotonicity
  and atomic replacement are required; CI still measures the whole tree.
