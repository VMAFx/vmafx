- Migrated the Windows MSYS2 MinGW build matrix leg in
  `.github/workflows/libvmaf-build-matrix.yml` from the deprecated `MINGW64`
  environment linking legacy `msvcrt.dll` to `UCRT64` linking the Universal C
  Runtime (`ucrtbase.dll`), using `mingw-w64-ucrt-x86_64-*` packages. Updated the
  required status check name in `.github/workflows/required-aggregator.yml` to
  `Windows UCRT64` (ADR-1387, #1609).
