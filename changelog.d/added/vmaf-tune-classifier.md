- **vmaf-tune introduces content-aware video classification ([ADR-0618](docs/adr/0618-content-aware-classifier.md), [ADR-3374](docs/adr/3374-vmaf-tune-classifier-vision-fails-by-name.md)).**
  `vmaf-tune classify` extracts ITU-T P.910 spatial information (SI) and temporal
  information (TI) via FFmpeg `siti`, probes container geometry and dynamic range
  (HDR/SDR), and stratifies motion/detail into named classes. Optional genre
  classification hooks into local OpenAI-compatible VLM endpoints (loopback/RFC1918),
  strictly failing by name on missing or unsupported vision endpoints without silent
  fallback to `unknown` ([usage](docs/usage/vmaf-tune-classifier.md)).
