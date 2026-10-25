## Perceptual video fingerprint for near-duplicate result reuse in vmaf-tune (2026-10-24)

- `tools/vmaf-tune/src/vmaftune/fingerprint.py`: implements PDQ frame sampling
  at 1 fps via FFmpeg, Hamming distance matching (<= 31), quality filtering
  (>= 50), metadata extraction, and ADR-3372 refusal ladder evaluation.
- `tools/vmaf-tune/src/vmaftune/cache.py`: secondary lookup `get_near_duplicate()`
  on exact miss using `params_key` and fingerprint sidecars under `fingerprints/`.
- `tools/vmaf-tune/src/vmaftune/corpus.py`: wire `--near-dup-reuse` and
  `--near-dup-verify` options into corpus sweep, record match provenance on
  cache hit rows, and run 1-second VMAF spot checks.
- `tools/vmaf-tune/src/vmaftune/cli.py`: expose `--near-dup-reuse` and
  `--near-dup-verify` flags on `corpus` and `recommend` subcommands.
- `tools/vmaf-tune/pyproject.toml`: add optional extra `fingerprint = ["pdqhash>=0.2.8", "numpy"]`
  and include `pdqhash>=0.2.8` under `dev` extra.
- `tools/vmaf-tune/tests/test_fingerprint.py`: full test suite covering positive,
  negative, boundary conditions, planted negative 1 MB collision, and cache
  integration.
- **On sync**: keep fork's near-duplicate caching and perceptual fingerprinting
  implementation.
