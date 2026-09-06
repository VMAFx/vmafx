- **Scorecard and Code Scanning audit and cleanup (2026-09-04)** —
  audited all remaining OpenSSF Scorecard and GitHub Code Scanning alerts.
  (1) Added `osv-scanner.toml` to ignore informational advisory `GO-2026-5932`
  for the unimported and unreachable `golang.org/x/crypto/openpgp` subpackage
  (Scorecard Alert 4). Documented maintainer-only administrative requirements
  for `CodeReviewID` (Alert 1) and `CIIBestPracticesID` (Alert 3).
  (2) Fixed five actionable CodeQL findings: deleted unused `pytest` import
  in `test_parity_argv.py` (Alert 965), added header guards to `core/tools/spinner.h`
  (Alert 960), split `usage()` into non-variadic and variadic overloads in
  `core/tools/cli_parse.cpp` (Alerts 938, 939), rephrased comment in
  `test_model_feature_overload_ownership.c` (Alert 951), and removed redundant
  lower-bound comparisons in `core/src/pdjson.c` (Alert 954).
  (3) Published full audit report in `docs/security/scorecard-alerts-2026-09-04.md`
  covering all resolved and reported-not-fixed items.
