AI dataset fetch helpers now emit replay manifests: KoNViD-1k fetches write
`fetch_manifest.json` and YouTube-UGC subset fetches write a separate
`*.run-manifest.json` sidecar with shared ADR-0661 run provenance.
