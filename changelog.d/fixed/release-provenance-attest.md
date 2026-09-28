- Release provenance for the native Linux files and the `vmaf-mcp` wheel and
  sdist is a GitHub build-provenance attestation (SLSA v1 provenance
  predicate, signed through Sigstore) instead of `slsa-github-generator`
  output. The generator calls its own actions by tag, which the organisation's
  SHA-pinning policy rejects, so the v1.0.0-rc.2 publication failed until the
  policy was relaxed by hand. Releases now attach
  `vmafx-build-provenance.sigstore.json` and `vmaf-mcp-provenance.sigstore.json`
  in place of the `.intoto.jsonl` files; verify with
  `gh attestation verify FILE --repo VMAFx/vmafx`, or offline with `--bundle`
  (ADR-1356). PyPI's PEP 740 attestations are unchanged.
