- **docs(adr):** Add three optional sections to `docs/adr/0000-template.md` —
  `## Supply-chain impact` (deps added/removed, build-time fetches,
  Sigstore-signability, CVE surface delta), `## SBOM delta` (CycloneDX
  components add/remove snippet), and `## Carbon / footprint` (image-size
  MiB delta, build-time wall-clock delta, runtime energy estimate).
  Sections are explicitly marked `<!-- Optional -->`; authors delete the
  header rather than leaving an `N/A` stub when irrelevant. Existing
  Accepted ADRs are untouched per the immutable-once-Accepted rule; the
  next three ADRs authored from scratch must include all three sections
  (even if "none") so reviewers calibrate the new shape. README updated
  with the per-section roll-out note.
