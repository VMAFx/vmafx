- **vmaf-tune supports perceptual video fingerprinting for near-duplicate result reuse ([ADR-3372](docs/adr/3372-perceptual-video-fingerprint.md)).**
  `--near-dup-reuse` on `vmaf-tune corpus` and `recommend` enables a secondary
  cache lookup on an exact source hash miss using PDQ perceptual video
  fingerprinting (optional extra `vmaf-tune[fingerprint]`). Sources sharing the
  same encode parameters reuse cached results when duration, aspect ratio,
  frame counts (>= 30 valid frames), and three temporal segments (10%, 50%, 85%)
  meet published thresholds, with optional `--near-dup-verify` spot checking
  against drift ([usage](docs/usage/vmaf-tune-near-dup.md)).
