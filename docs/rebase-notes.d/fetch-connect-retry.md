## CI downloads retry connection failures and write to a file first (2026-11-10)

- Netflix-derived `Dockerfile`: keep wget connection and host-resolution retry flags when updating the optional oneAPI key setup from upstream.
- Preserve the temporary-file key download, successful-fetch-before-gpg ordering and cleanup; an inner retry into a pipe can duplicate partial bytes.
- `scripts/ci/tests/test_fetch_connect_retry.py::FetchConnectRetryTests::test_tracked_fetches_retry_connect_failures` guards the live download inventory, retry flags and file-first body contract.
