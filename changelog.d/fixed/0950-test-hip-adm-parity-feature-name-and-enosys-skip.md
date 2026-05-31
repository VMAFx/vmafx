- `core/test/test_hip_adm_parity` now actually exercises the HIP
  `adm_hip` extractor for its HIP comparand (previously called the
  upstream CPU `adm` extractor on both sides, so the integer ADM
  CPU-vs-HIP parity claim from ADR-0539 was not being enforced — the
  gate passed trivially via the CPU fallback). Adds the same
  `-ENOSYS` skip predicate as the motion3 sibling test (ADR-0949)
  for builds with `enable_hip=true, enable_hipcc=false`. Test-only
  change; no runtime behaviour shift. See ADR-0950.
