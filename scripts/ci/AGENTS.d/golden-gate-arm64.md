---
paths:
  - scripts/ci/setup-golden-build.sh
  - scripts/ci/golden-arm64-preflight.sh
  - scripts/ci/tests/test_golden_gate_makefile_contract.py
  - build-aux/aarch64-linux-gnu-clang.ini
  - Makefile
invariant: Golden gate builds: one profile (`setup-golden-build.sh`), native and aarch64 cross; preflight before any configure.
area: tests
---
<!-- markdownlint-disable MD013 MD060 -->
# Golden gate build profile, native and aarch64 (ADR-1317, ADR-1461)

- One build profile: `GOLDEN_MESON_OPTIONS` in `setup-golden-build.sh`. Native
  gate (`make test-netflix-golden`) and aarch64 gate
  (`make test-netflix-golden-arm64`) both configure through this script; cross
  mode = `GOLDEN_CROSS_FILE`. No second option list in `Makefile`.
- Compiler stays gcc or clang (`validate_compiler_id`); cross build reports
  cross compiler's id, so same check holds. Never icx: ADR-1317.
- Both gates run `$(GOLDEN_PYTEST_ARGS)`. Test added to one gate = test added
  to both; neither recipe spells test file.
- aarch64 gate runs `tools/vmaf` through kernel binfmt handler
  (`/proc/sys/fs/binfmt_misc/qemu-aarch64`) with `QEMU_LD_PREFIX` = sysroot.
  `golden-arm64-preflight.sh` checks cross compiler, cross file, sysroot loader,
  enabled handler; reports every missing piece, exit 1, before configure.
  Missing piece discovered later = `Exec format error` on 283 tests.
- Build dirs: `core/build-golden`, `core/build-golden-arm64-gcc`,
  `core/build-golden-arm64-clang`. Separate, gitignored, removed by `clean`.
- Emulation proves numbers, not time. No timing claim from qemu run.
- `test_golden_gate_makefile_contract.py` pins recipes + preflight (fixture
  host without real cross tools). Change recipe -> change test.
