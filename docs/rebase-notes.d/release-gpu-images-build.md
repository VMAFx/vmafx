## nv-codec-headers pin and kernel-object licence rules (2026-10-10)

`fix/release-dry-run-gpu-images`. `build-config.env` owns the nv-codec-headers release
(`NV_CODEC_HEADERS_TAG`, `NV_CODEC_HEADERS_COMMIT`); the root `Dockerfile`,
`dev/Containerfile`, `docker/Dockerfile.tester`,
`docker/Dockerfile.production-gpu`, the configure errors of
`core/src/meson.build` and the `nv-codec-headers` source of
`tools/rc1-tester/image/licensing.json` repeat it, and
`scripts/ci/tests/test_nv_codec_headers_single_source.py` fails on a copy that
differs. `core/src/meson.build` is an upstream-mirror file: on a sync keep the
fork's `nv_codec_headers_need` text and the `cc.has_member('CudaFunctions',
'cuArray3DGetDescriptor', ...)` check after the two header checks (upstream
names commit `876af32a202d`, which lacks that member). A port that calls a
newer loader member raises the pin in the same PR. The kernel-object rules of
`licensing.json` glob `core/src/**`, so a `.cu` or `.hip` stem must stay unique
under `core/src`.
