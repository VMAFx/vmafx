## nv-codec-headers pinned in the hosted CUDA legs (2026-10-13)

- The four `Install nv-codec-headers` steps of `.github/workflows/build.yml`
  and `.github/workflows/libvmaf-build-matrix.yml` call
  `scripts/ci/fetch-nv-codec-headers.sh`; none clones the repository.
- `scripts/ci/tests/test_nv_codec_headers_single_source.py` fails on a clone of
  `nv-codec-headers` in either workflow and on any count of script calls other
  than two per workflow. A workflow change that adds a CUDA leg updates
  `CI_STEPS_PER_WORKFLOW` with it.
