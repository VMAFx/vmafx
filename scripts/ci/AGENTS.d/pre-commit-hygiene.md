---
paths:
  - .pre-commit-config.yaml
invariant: Third-party code enters through Meson wraps or `ffmpeg-patches/`, never a submodule; verify hook revision bumps.
---
<!-- markdownlint-disable MD013 MD060 -->
# Pre-commit hook hygiene — no submodules (ADR-0893)

`.pre-commit-config.yaml` ships upstream `forbid-new-submodules`
hook. Fork pulls upstream Netflix/vmaf code via `subprojects/`
(Meson wraps with sha256 pinning) and `ffmpeg-patches/` (out-of-tree
patch series), **never** via `.gitmodules`. Submodule entry would
bypass:

- wrap-pin sha256 enforcement,
- CycloneDX SBOM walk (inspects `subprojects/*.wrap`, not
  `.gitmodules`),
- and license-allow-list audit.

Adding new third-party dependency -> use Meson wrap
(or vendor it under clear "Vendored 3rd-party" banner, with
attendant `.semgrepignore` / `check-copyright` exclusion). Do not
work around `forbid-new-submodules` hook with `--no-verify`.

**Pinned-revision audit cadence**: re-audit `.pre-commit-config.yaml`
revisions roughly every ~6 months or when CI surfaces deprecation
warning. `pre-commit autoupdate` = starting point only; verify
each proposed bump against `git ls-remote --tags --refs <repo>`.
Autoupdate heuristic has known sort-order bug on repos that
land point releases out of branch order (suggested a
`gitleaks v8.30.1 → v8.30.0` downgrade during ADR-0893 audit).
Alpha pre-releases (`X.Y.Za<N>`) never acceptable pin.

**actionlint runs through `scripts/ci/run_actionlint.py` (ADR-2199)**: hook
`entry:` override and `make lint-actions` both. Reason: actionlint v1.7.12
deadlocks writing `run:` script to shellcheck stdin pipe before start;
`fs.pipe-user-pages-soft` exhaustion shrinks pipes to 8 KiB. Wrapper = 90 s
deadline, SIGQUIT first (goroutine dump saved to a named file), exit 124 +
named cause, never pass. Do not call bare `actionlint`
in hook or target; do not lengthen deadline to hide a hang;
`test_run_actionlint.py` plants the hang.
