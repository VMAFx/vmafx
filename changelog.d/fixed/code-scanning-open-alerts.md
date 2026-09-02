- Addressed GitHub code-scanning alerts and re-audited warning-level security dismissals:
  - Fixed the CodeQL Python finding in the MCP server: removed the unused
    `pytest_asyncio` import (`test_smoke_e2e.py`, Alert 919). All six async
    tests in that file carry `@pytest.mark.asyncio` and pytest-asyncio loads
    through its `pytest11` entry point, so the import bound no name and had no
    plugin-loading role; the MCP Smoke job passes without it.
  - Fixed CodeQL C/C++ findings: replaced trivial single-case `switch` with
    `if`/`else` in `feature_name.cpp` (Alert 167); refactored loop-variable
    modification into an idiomatic `while` loop in `mkdirp.cpp` (Alert 164);
    and removed redundant lower bound `0xC2 <= u` in `pdjson.c` `utf8_seq_length`
    (Alert 691).
  - Upgraded `golang.org/x/crypto` to `v0.56.0` in `go.mod` (Alert 4), resolving
    high-severity vulnerabilities `GO-2026-6354` and `GO-2026-6355`.
  - Re-audited and hardened security-relevant dismissals: added
    `usedforsecurity=False` to 3 SHA-1 memoization keys in
    `compat/python-vmaf/tools/decorator.py` (Alerts 731, 732, 733); verified
    UNIX domain socket mode `0o660` with preserved server bind and test coverage
    in `online_trainer.py` (Alert 373) with inline `# nosemgrep`.
  - Reported-not-fixed (no inoperative or fake suppressions added):
    - CodeQL `py/cyclic-import` between `server.py` and `http_transport.py`
      (Alerts 917, 918): the cycle is real and remains. Both directions are
      already function-local, so nothing cycles at import time. Converting one
      side to `importlib.import_module` would hide the cycle from CodeQL
      without removing it, at the cost of static checkability — that is a
      suppression, not a fix, so it is not done here. Breaking it properly
      means extracting the five symbols `http_transport` needs out of the
      ~3.2k-line `server.py` into a shared module, which is a refactor beyond
      the scope of an alert sweep.
    - CodeQL `cpp/equality-on-floats` in `feature_name.cpp` (Alert 168) and
      `predict.c` (Alert 927): load-bearing bit-exact sentinel and default
      comparisons preserved per ADR-0138 / ADR-0139; in-source suppressions
      omitted because no dismiss-alerts workflow exists.
    - CodeQL `cpp/include-non-header` in `test_feature.cpp` (Alert 165) and
      `test_luminance_tools.cpp` (Alert 908): white-box unit-test unity includes
      preserved without suppressions.
    - CodeQL `cpp/unused-local-variable` in `core/build/meson-private/...` (Alert 932):
      transient Meson compiler probe file generated during traced C/C++ builds,
      not filterable via paths-ignore in traced extraction; left visible for maintainer.
    - OpenSSF Scorecard `GO-2026-5932` (Alert 4 sub-finding): unimported deprecated
      `x/crypto/openpgp` reported without unconsumed `osv-scanner.toml` suppression.
    - Scorecard `CodeReviewID` (Alert 1) and `CIIBestPracticesID` (Alert 3):
      documented solo-maintainer and external-badge blockers in `docs/state.md`
      citing ADR-0263.
