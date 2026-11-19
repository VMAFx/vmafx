- **CI:** Move the Windows MSVC+SYCL oneAPI offline installer pin in `libvmaf-build-matrix.yml` to `2026.2.0.661` (`intel-oneapi-toolkit-2026.2.0.661_offline.exe`): MSVC 14.52 refuses Clang front ends older than 22 (`STL1000`), and Intel's release notes describe a Clang 22 front end from compiler release 2026.0.0 ([ADR-3567](docs/adr/3567-windows-sycl-oneapi-2026-2.md)).
  Builds run `44487361326` measured six refused AOT targets (`tgllp`, `adl-s`,
  `adl-p`, `adl-n`, `rpl-s`, `rpl-p`). Xe-LP, Alder Lake and Raptor Lake
  integrated devices get no AOT image from the Windows CI build and, by design,
  fall back to the [`spir64` image compiled at run time](docs/backends/sycl/aot.md#default-target-list).
  The build with 13 accepted targets and this fallback on Windows remain unmeasured.
