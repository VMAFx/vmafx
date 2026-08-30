- **Ensemble training kit — Google-Drive contributor-bundle scripts.**
  Adds two operator-facing scripts under
  [`tools/ensemble-training-kit/`](../tools/ensemble-training-kit/):
  `prepare-gdrive-bundle.sh` (lead-user side) compresses the local
  BVI-DVC + Netflix raw YUV corpus (~229 GiB) to lossless HEVC and
  tars it with a manifest into a single ~100 GiB Google-Drive-friendly
  bundle; `extract-corpus.sh` (contributor side) decodes the lossless
  HEVC back to bit-exact YUVs and verifies every file against the
  bundled sha256 manifest before the trainer runs. README quickstart
  for gdrive recipients added at the top of
  [`tools/ensemble-training-kit/README.md`](../tools/ensemble-training-kit/README.md).
  Companion to [ADR-0324](../docs/adr/0324-ensemble-training-kit.md);
  closes the "kit ships orchestrator only, contributors source data
  manually" loop the original kit left open.
