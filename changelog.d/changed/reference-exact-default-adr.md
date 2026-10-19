- **The plan for reference-exact extractors is written down.**
  [ADR-2343](docs/adr/2343-reference-exact-default-compat-mode.md) records that RC7 proves every
  extractor against its original implementation, that the default becomes reference-exact with
  Netflix's behaviour as a named compatibility mode the golden gate runs in, and that the RC9
  retrain trains on reference-exact features. The roadmap, the release page and the retrain
  runbook say so. No extractor or score changes yet.
