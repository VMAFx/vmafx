---
paths:
  - scripts/ci/check-base-image-single-source.sh
  - scripts/ci/check-container-image-references.py
  - scripts/ci/tests/test_base_image_single_source.py
  - docker/dev/ubuntu-26.04-cuda.Dockerfile
invariant: One FROM/COPY parser; shared FROM arguments default from `build-config.env`; CUDA bases equal `DEV_BASE` with digest.
---
<!-- markdownlint-disable MD013 MD060 -->
# Base-image references (ADR-1231)

`check-base-image-single-source.sh` delegates FROM/COPY instruction parsing to
`check-container-image-references.py`. External references without digest
still external: never restore old `*@sha256:*`-only detection. Keep
instruction case, flags and continuations covered by fixture tests.
Shared FROM arguments need global, single-line default named by
`build-config.env`; shell gate owns default drift/repair. Local image
exceptions bind exact consumer and value, never broad unpinned-tag rule.
sole `docker/dev/` file in this ownership set is
`docker/dev/ubuntu-26.04-cuda.Dockerfile`; its CUDA builder mirror is generated
from `build-config.env`, while Alpine, Arch, and Fedora matrix files remain
independent. `CUDA_BUILDER` and `CUDA_RUNTIME` must equal `DEV_BASE` exactly,
including digest, not merely share its Ubuntu tag.
`tests/test_base_image_single_source.py` exercises actual gate in scratch
repositories, wired through `test-base-image-single-source` in
`.pre-commit-config.yaml`.
