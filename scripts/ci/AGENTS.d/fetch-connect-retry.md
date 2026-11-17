---
paths:
  - scripts/ci/tests/test_fetch_connect_retry.py
  - scripts/ci/install-rocm-from-image.sh
  - scripts/dev/tidy-lane.sh
  - dev/Containerfile
  - docs/development/ci.md
invariant: Curl/wget retries consume options correctly; retried bodies saved before use; tidy-lane APT retries positive.
area: gates
---
# Fetch connection retry contract

- Curl: positive `--retry` + enabled `--retry-all-errors`.
- Wget: `--tries=N`, literal N >= 2 (total attempts) + enabled
  `--retry-connrefused` + `--retry-on-host-error`.
- Retried body -> fetch-owned file; consumer after success; cleanup after use.
- Stdout body forbidden through pipe, capture, shell file redirection;
  `/dev/null` disposal allowed.
- Option allow-list: curl 8.23.0 `src/tool_getparam.c`; GNU Wget 1.25.0 `wget --help`.
- Unknown option -> error; value consumes next argument; final switch wins.
- Exemption shape: exact path + parsed URL + reason; stale/ambiguous entry -> failure.
- `OLD_CURL_STAGES`: exact path + stage name + reason;
  absent file/stage -> stale error.
- Old-curl stage: parsed curl flags checked through next `FROM`;
  exemptions included.
- `dev/Containerfile`, `release-build`: manylinux_2_28 / AlmaLinux 8
  curl 7.61.1;
  `--retry-all-errors` needs 7.71.0 -> forbidden.
- NASM exemption: exact release URL; whole-fetch loop x3,
  `--retry 3 --retry-connrefused`, curl-owned `/tmp/nasm.tar.xz`;
  checksum/extract/build after successful fetch.
- ROCm layer exemption: whole pipeline retried `LAYER_ATTEMPTS` times; inner
  retry appends partial bytes.
- Tidy-lane APT `update`/`install`: `-o Acquire::Retries=N`, literal positive N.
- Known gaps #3392: `git clone`, APT outside tidy lane; `Known gap (#3392):` fixtures.
- Test: `scripts/ci/tests/test_fetch_connect_retry.py`; offline source reads only.
