## Fixture downloads retry through one policy (2026-10-10)

- `compat/python-vmaf/config.py` (upstream-mirror): `download_reactively()`
  calls `retrieve(remote_path, tmp_path)` from the fork file
  `compat/python-vmaf/tools/retrying_download.py` where upstream calls
  `urllib.request.urlretrieve(remote_path, tmp_path)`. The tempfile and
  `os.replace` structure around it is upstream's (Netflix/vmaf `32780bd9b`)
  and unchanged. **On sync**: keep the `from vmaf.tools.retrying_download
  import retrieve` line and the `retrieve()` call; a sync that restores the
  bare `urlretrieve` fails `scripts/ci/tests/test_fetch_test_yuvs.py` and
  `compat/python-vmaf/tests/test_retrying_download.py`.
- `scripts/test/fetch-test-yuvs.sh` and
  `.github/workflows/upstream-consumers.yml` are fork files; nothing to
  reconcile.
- No score, model or public interface changes.
