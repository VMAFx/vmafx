<!-- markdownlint-disable MD013 -->
# Test fixtures

Run `scripts/test/fetch-test-yuvs.sh` once per checkout to provision the
YUV clips that the Python tests (`python/test/quality_runner_test.py`,
`python/test/feature_extractor_test.py`) and the Netflix golden gate read.

The clips live outside the repository. Netflix removed them upstream in
2020 (commit `bac8b6073`) and moved them to the sibling repository
<https://github.com/Netflix/vmaf_resource>. The local target directory
`python/test/resource/yuv/` is gitignored, so it is absent in a fresh
tree.

## Provisioning fixtures locally

```bash
scripts/test/fetch-test-yuvs.sh
```

The script:

- Downloads seven files into `python/test/resource/yuv/` when they are
  not already present: `src01_hrc00_576x324.yuv` and
  `src01_hrc01_576x324.yuv` (576x324),
  `checkerboard_1920_1080_10_3_0_0.yuv`,
  `checkerboard_1920_1080_10_3_1_0.yuv` and
  `checkerboard_1920_1080_10_3_10_0.yuv` (1920x1080), and the two
  single-frame HDR clips `ref_dock-na-HDR_res2160_..._576x324_1frame.yuv` /
  `dis_dock-na-HDR_res1440_..._576x324_1frame.yuv` (576x324, `yuv420p10le`,
  PQ / BT.2020nc / limited range; the files carry no colorimetry). The HDR
  pair is read by `python/test/vmafexec_color_args_test.py` and is not part of
  the golden gate.
- Verifies the md5 sum of every file against hardcoded expected values.
  A local file with the right name but the wrong content is detected,
  deleted and downloaded again.
- Is idempotent. Re-running on a fully provisioned tree prints
  `ok      <name> (md5 verified)` for each fixture and exits 0.

The first five files are the three Netflix test pairs of the CPU golden gate
(normal, checkerboard 1-px, checkerboard 10-px; see section 8 of
[AGENTS.md](https://github.com/VMAFx/vmafx/blob/master/AGENTS.md) and
[ADR-0024](../adr/0024-netflix-golden-preserved.md)). The cross-backend
VIF and motion diff jobs that CI runs use them too.

## Why md5 verification matters

The script refuses a file whose md5 differs from the expected value and
fetches it again. A mismatch is reported at provision time:

```text
stale   src01_hrc00_576x324.yuv (md5 4226fb7e…, want b16f67d3…) — refetching
```

!!! note
    A stale local copy with the canonical name and size but different
    content once produced 84 test failures in the
    `feature_extractor_test.py` and `quality_runner_test.py` suites
    (2026-05-17). The assertion errors looked like extractor bugs (VIF
    score about twice the expected value), but the cause was a fixture
    content mismatch. See
    [ADR-0493](../adr/0493-test-yuv-fixture-md5-verification.md).

To provision some of the files only, name them:

```bash
scripts/test/fetch-test-yuvs.sh src01_hrc00_576x324.yuv src01_hrc01_576x324.yuv
```

A name that is not in the script's `FIXTURES` list is refused (exit 2).

## Download retries

The fixture host sometimes fails for minutes: on 2026-10-10 it answered HTTP
504 from 14:15 to at least 14:18 UTC and two required CI jobs failed on it.
Both ways a fixture is downloaded now share one retry policy,
`compat/python-vmaf/tools/retrying_download.py`:

- `scripts/test/fetch-test-yuvs.sh` runs that file for every download;
- `download_reactively()` in `compat/python-vmaf/config.py`, which fetches a
  fixture the first time a Python test asks for it, calls its `retrieve()`.

A download gets seven attempts, with waits of 5, 10, 20, 40, 80 and 120
seconds between them (275 seconds in all). The waits stay below the 600 second
per-test timeout of the Coverage Gate's pytest run.

| Failure | What happens |
| --- | --- |
| HTTP 408, 425, 429, 500, 502, 503, 504, 522 or 524 | Retried |
| Refused, reset or timed-out connection, failed name lookup, short body | Retried |
| HTTP 404 and every other status | Fails at once |
| A certificate that does not verify, a full disk | Fails at once |

When the script cannot download a file after the last attempt it stops and
does not try the files after it, which come from the same host. Each retry
prints one line on standard error:

```text
download of https://…/src01_hrc00_576x324.yuv: attempt 1 of 7 failed (HTTP Error 504: Gateway Timeout), next in 5 s
```

## CI parity

CI jobs that need the clips run the same script, and the Python tests of the
Coverage Gate download further fixtures through `download_reactively()`. A
workflow does not download from `Netflix/vmaf_resource` on its own
(`scripts/ci/tests/test_fetch_test_yuvs.py` refuses that). If the canonical
content in `Netflix/vmaf_resource` changes, update the script's expected-md5
list.

## Fixtures not covered by the script

Some tests reference fixtures that CI does not run on every PR and that
the script does not provision:

- `KristenAndSara_1280x720_8bit_processed.yuv`
- the multi-frame and bit-depth `src01_*` variants

When a CI job activates one of those tests, add the file with its md5 to
the `FIXTURES` array in `scripts/test/fetch-test-yuvs.sh`.

## Other fixture roots

- `testdata/` holds the tracked 48-frame pairs
  (`ref_576x324_48f.yuv` / `dis_576x324_48f.yuv` and the 640x480 pair)
  and the snapshot JSONs.
- `testdata/bbb` is a gitignored 4K clip used by the performance
  baselines; see
  [backend performance baselines](backend-perf-baselines.md).
- `python/test/resource/yuv/` is the directory this script fills.

## Related

- [AGENTS.md](https://github.com/VMAFx/vmafx/blob/master/AGENTS.md)
  section 8: Netflix golden-data gate rule
- [ADR-0024](../adr/0024-netflix-golden-preserved.md): Netflix golden tests
- [ADR-0493](../adr/0493-test-yuv-fixture-md5-verification.md): rationale for
  this provisioner
