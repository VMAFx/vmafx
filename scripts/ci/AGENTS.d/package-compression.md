---
paths:
  - scripts/ci/tests/test_package_compression.py
invariant: Pushes use IMAGE_COMPRESSION; xz 9; zopfli zips.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# Package compression (ADR-1591, ADR-1594)

Every published archive and image uses strongest compression all of its documented
consumers open ([ADR-1591](../../../docs/adr/1591-package-compression.md),
[ADR-1594](../../../docs/adr/1594-zstd-images-zopfli-zips.md), table in
[Artifact publishing policy](../../../docs/development/publishing.md#compression)).

| Producer | Rule | Guard |
| --- | --- | --- |
| `build-windows-tester-bundle.py::pack()` | every entry is zopfli's Deflate stream (`zopfli_deflate()`); `write_zip()` writes `zipfile`'s Windows records itself and refuses zip64; zopfli pinned once in `requirements/locks/windows-tester-zip.in` and mirrored in rc1-tester `dev` extra | `tools/rc1-tester/tests/test_windows_bundle.py` (stream equals zopfli's; container equals `zipfile`'s), `tests/test_package_compression.py` (pins) |
| `build-macos-tester-bundle.sh` pack step | `tar --options xz:compression-level=9 -cJf <name>.tar.xz`; workflow globs `*.tar.xz` | `tests/test_package_compression.py` |
| `docker-publish-tester.yml`, `docker-publish-production.yml`, `docker-publish-operator-node.yml`, `dev-container-publish.yml`, `published-rc-licence-companions.yml` | `IMAGE_COMPRESSION` = `compression=zstd,compression-level=22,force-compression=true,oci-mediatypes=true`; every push exports through `outputs:` ending in it; no `push:` shorthand; composite action calls pass `compression:` | `tests/test_package_compression.py` |
| any other workflow that pushes image or calls `image-licence-artifacts` | must join publishing list of test | `tests/test_package_compression.py` |

Without `force-compression` BuildKit pushes base-image and cached layers as gzip it
received them in (GitHub Actions cache always stores gzip); with it, every layer is
zstd and base layers lose their upstream digests. Without `oci-mediatypes=true` Docker
cannot pull zstd layers. guides require Docker Engine 23.0 or later
(`docs/usage/docker.md`, "What can pull the images").
