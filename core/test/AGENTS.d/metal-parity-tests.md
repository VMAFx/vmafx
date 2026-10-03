---
paths:
  - core/test/metal_twin.h
  - core/test/test_metal_*_parity.c
  - core/test/test_metal_report_rows_contract.py
  - core/test/*_twin_parity.h
invariant: Metal parity tests compare `==`, run every case via metal_run_case(), skip without a device, pass as self-tests.
---
<!-- markdownlint-disable MD013 -->
# Metal parity tests (ADR-1496)

Device run only on outside tester's Mac (macOS tester bundle). Hosted macOS runner = no Metal device -> every case skips there. So:

- Compare `==` on every output. Only bounds: ciede `CIEDE_TWIN_TOL 1e-9` (= `LIBM_TWINS["ciede"]`), and a shared header's derived bound past 2^53 / for powf options. No `PARITY_TOL`, no places. `test_metal_report_rows_contract.py` scans code (comments stripped).
- `#include "metal_twin.h"` first; twin names via `METAL_TWIN("<metal>", "<cpu>")`; callbacks `metal_twin_open/import/close`; shared `*_twin_parity.h` headers take them unchanged.
- Every case through `metal_run_case(test_x)`; `run_tests()` returns `metal_first_failure`. Never `mu_run_test()` (stops at first failure = later cases unmeasured). `@case <name> pass|fail|skip` line = what `hw_suites.py` reads; renaming a case = edit `tools/rc1-tester/image/metal-rows.json` too.
- No device -> print skip, `mu_skipped = 1`, return NULL. Case needing no state but checking a twin (init(), option tables) starts `if (!metal_twin_have_device()) return NULL;` else hosted CI goes red on today's twins.
- Case that only means something against the twin (CPU accepts what the twin must refuse) starts `if (metal_twin_device_only()) return NULL;`.
- Self-tests `test_metal_selftest_<name>` (suite `fast`, `metal-selftest`, every host) = same TU with `-DVMAF_METAL_TWIN_SELFTEST`: CPU in twin's place. Every `==` case must pass there; failure = wrong fixture/key/option string.
- New Metal twin or test: name into `metal_parity_tests` (`core/test/meson.build`), `tools/rc1-tester/image/unit-tests-macos.txt`, rows into `metal-rows.json`. Contract test fails otherwise.
- `ciede_twin_parity.h` `CIEDE_TWIN_TOL` is `#ifndef`-guarded for the Metal test; keep guard.
