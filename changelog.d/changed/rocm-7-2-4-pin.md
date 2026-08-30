## Internal

- Bump ROCm pin from 7.2.3 to 7.2.4 in `dev/Containerfile`, `build.yml`, and
  `libvmaf-build-matrix.yml`. AMD released 7.2.4 on 2026-05-26; the patch
  carries no KFD ABI break (per ADR-0541). Updated `core/meson_options.txt`
  minimum requirement references from "ROCm 6+" to "ROCm 7.0+" and noted
  7.2.4 as the CI-tested version in `docs/backends/hip/overview.md`.
