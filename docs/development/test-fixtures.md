<!-- markdownlint-disable MD013 -->
# Test fixtures

The Python test suite (`python/test/quality_runner_test.py`,
`python/test/feature_extractor_test.py`) and the cross-backend ULP
gates exercise libvmaf against a fixed set of YUV reference clips.
These clips live **outside the repo** — Netflix removed them upstream
in 2020 (commit `bac8b6073`) and moved them to a sibling resource
repository at <https://github.com/Netflix/vmaf_resource>.

The local target directory `python/test/resource/yuv/` is `.gitignore`-d.

## Provisioning fixtures locally

Run:

```bash
scripts/test/fetch-test-yuvs.sh
```

The script:

- Downloads `src01_hrc00_576x324.yuv` and `src01_hrc01_576x324.yuv` to
  `python/test/resource/yuv/` if not already present.
- **Verifies md5 sums** against hardcoded expected values. A local
  file with the right name but the wrong content is detected, deleted,
  and re-downloaded.
- Is idempotent; re-running on a fully-provisioned tree prints
  `ok      <name> (md5 verified)` for each fixture and exits 0.

These two fixtures are sufficient for the §8 Netflix CPU golden gate
([ADR-0024](../adr/0024-netflix-golden-tests.md)) and for the
cross-backend VIF / motion diff jobs that CI runs.

## Why md5 verification matters

A stale local copy with the canonical name and size but different
content silently produced 84 test failures in the
`feature_extractor_test.py` / `quality_runner_test.py` suites on
2026-05-17. The assertion errors looked like extractor bugs (VIF score
~2× the expected value) — the root cause was a fixture-content
mismatch, not a code bug. See
[ADR-0493](../adr/0493-test-yuv-fixture-md5-verification.md).

The md5 check turns this class of failure into an immediately legible
error at provision time:

```text
stale   src01_hrc00_576x324.yuv (md5 4226fb7e…, want b16f67d3…) — refetching
```

## CI parity

GitHub Actions runs the equivalent inline-`curl` block in
[`.github/workflows/tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml).
If the canonical content in `Netflix/vmaf_resource` changes, **both**
the CI workflow and the script's expected-md5 list must be updated in
the same change.

## Fixtures not covered by the script

Other test fixtures (`checkerboard_1920_1080_10_3_*_0.yuv`,
`KristenAndSara_1280x720_8bit_processed.yuv`, the
multi-frame / bitdepth `src01_*` variants) are referenced by tests
that CI does not currently run on every PR. They are not provisioned
by this script. When a future CI job activates one of those tests,
add the file (with its md5) to the `FIXTURES` array in
`scripts/test/fetch-test-yuvs.sh`.

## Related

- [`CLAUDE.md` §8](../../CLAUDE.md) — Netflix golden-data gate rule
- [ADR-0024](../adr/0024-netflix-golden-tests.md) — Netflix golden tests
- [ADR-0493](../adr/0493-test-yuv-fixture-md5-verification.md) — this provisioner rationale
