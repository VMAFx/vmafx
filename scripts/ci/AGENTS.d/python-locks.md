---
paths:
  - scripts/ci/check_python_dependency_locks.py
  - scripts/ci/tests/test_python_dependency_locks.py
  - requirements/locks/*
invariant: Every lock registered in `manifest.json`; installs hash-pinned, exact-path bound; scanners fail closed.
---
<!-- markdownlint-disable MD013 MD060 -->
# Hash-locked Python dependency policy (ADR-1305)

`check_python_dependency_locks.py` enforces cryptographic pinning and hermetic
install policy across repository:

- All lock files are generated via `requirements/locks/manifest.json` using reviewed `uv_version` (`0.12.18`).
- Manifest outputs and inputs must be local repository-relative paths on both POSIX and Windows (rejecting directory traversal `..`, drive/UNC or POSIX absolute paths, remote URLs, and surrounding whitespace); inputs must not contain duplicates.
- Manifest `compile_args` must not specify output overrides (`-o`, `--output-file`).
- Every `*-lock.txt` and `requirements/locks/*.txt` in tree must be registered in `manifest.json`.
- requirement target must exactly equal manifest output or explicit `install_aliases` entry bound to specific repo-relative consumer path and context. Consumer bindings themselves must be local repository-relative paths on both POSIX and Windows. Identity uses exact separator-normalized equality, so nested suffix lookalike never inherits authority. Aliases reject directory traversals (`..`), Windows drive/UNC paths, duplicate JSON keys, and unreferenced/dead aliases fail closed. Basename and suffix matches are never authority.
- Executable pip invocations are strictly parsed: global pre-subcommand flags (`--trusted-host`, etc.) are preserved, joined short forms (`-qrfoo`, `-rmalicious.txt`, `-cconstraints.txt`) are split and validated, and unhashed or secondary requirement/constraint flags fail closed.
- Nox AST scanner restricts receiver authority to `@nox.session` parameters, tracks and rejects plain or annotated session/method aliases (`installer = session.install; installer(...)`, `alias: object = session`, `installer: object = session.install`) and literal `getattr(session, "install")` aliases, and inspects literal shell runner invocations (`session.run("sh", "-c", ...)`) fail-closed.
- Nox development locks must be workstation-portable (compiled with `--universal`, no `--python-platform`), and every Nox session must pin explicit Python version that agrees with its lock resolution.
- Git discovery and consumer tracking fail closed with `ContractError` on any process or filesystem error.
- Workflow steps that consume repo-local requirements locks, packages, helper scripts, or local actions must execute strictly after `actions/checkout` in that job; `check_python_dependency_locks.py` fail-closed scanner (`scan_workflow_checkout_ordering`) enforces this ordering across all workflows in `.github/workflows/*.yml`. validator enforces fail-closed semantics across both PyYAML and fallback parsing paths: it accepts only exact `actions/checkout` owner and action pinned to full 40-character hex SHA (rejecting spoofed owners, altered action names, or unpinned refs); rejects foreign `with.repository` checkouts; rejects conditional (`if:`) and `continue-on-error` checkouts as insufficient; rejects checkouts with `with.path` targeting subdirectory; preserves folded YAML run blocks (`>`) where flags such as `-r` and local paths split across physical lines; detects joined pip options (`-rrequirements/...`, `-eai`) and local source targets (`--no-build-isolation ai`); fails closed on malformed YAML when PyYAML is installed; and, without PyYAML, accepts simple quoted or unquoted block keys for `jobs`, job ids, and `steps` while rejecting unsupported inline/flow-style mappings rather than treating unparsed workflow as empty.
- `write` is only network-accessing path; `check` is offline and run in pre-commit and `make lint`.
- Regressions are pinned in `scripts/ci/tests/test_python_dependency_locks.py`.
