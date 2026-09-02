- **Bump Intel NEO compute stack in `dev/Containerfile` as a matched set.**
  Intel compute-runtime (`NEO_VER`) moves to `26.31.39395.13`, Intel Graphics
  Compiler (`IGC_VER`) to `2.40.13+22418`, Level Zero loader (`LEVEL_ZERO_VER`)
  to `1.32.0`, and Intel gmmlib (`GMMLIB_VER`) stays aligned at `22.10.0`.
  The four components are co-dependent: `dev/Containerfile` downloads
  `libigdgmm12_${GMMLIB_VER}_amd64.deb` from the `intel/compute-runtime`
  release assets for `NEO_VER` and requires the exact IGC deb names referenced
  by that release. Bumping `GMMLIB_VER` in isolation (as PR #1184 attempted)
  caused a 404 error during container build; bumping all components as a
  coherent matched set ensures all release assets resolve (HTTP 200).
