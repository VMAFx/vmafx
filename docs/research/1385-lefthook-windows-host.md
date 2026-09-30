<!-- markdownlint-disable MD013 MD060 -->
# Research-1385: Lefthook and the pre-commit framework on a Windows host

- **Status**: Active
- **Workstream**: [ADR-1385](../adr/1385-lefthook-installer-coexistence.md), [ADR-1249](../adr/1249-praetor-governance-adoption.md), [ADR-1241](../adr/1241-worktree-hook-dispatch.md)
- **Last updated**: 2026-09-30

## Question

Lefthook had never been installed on the Windows 11 workstation that runs most
of this fork's agents, so local commits there ran no `pre-commit` or
`commit-msg` checks. What stops the ADR-1249 hook stack (lefthook delegating to
the pre-commit framework, praetor governance jobs, ADR-1241 dispatchers) from
passing in Git Bash, and what does installing it touch?

## Sources

- Lefthook v2.1.14 source, from the Go module cache:
  `internal/run/controller/exec/exec_windows.go`, `internal/system/sh_windows.go`,
  `internal/templates/hook.tmpl`, `internal/command/uninstall_ai.go`,
  `internal/command/install_ai.go`.
- pre-commit 4.6.2 and reuse 6.2.0 (`reuse/extract.py`) installed from
  `requirements/locks/pre-commit.txt` into a Python 3.14 virtualenv.
- Praetor at the repository pin `f41e74d8` (`PRAETOR_REF` in
  `.github/workflows/standards-gate.yml`) and at `25451d8`, the unpinned build on
  the host's default `PATH`.
- Runs on 2026-09-30 in a scratch clone with lefthook installed in that clone
  only, Git for Windows Bash, GNU Make 4.4.1 (Windows32 build).

## Findings

1. **Double quotes end lefthook's script on Windows.** `exec_windows.go` builds
   the command line as `"<sh>" -c "<run>"` without escaping `<run>`. The first
   `"` inside a `run:` value closes the `-c` argument. The multi-line
   `framework-hooks` job failed in 0.03 s with `syntax error: unexpected end of
   file from 'if' command`; a probe `run: echo "quoted words here" && echo after`
   printed `quoted` and dropped the rest without failing. Multi-line values
   without quotes work. The framework jobs now call
   `scripts/git-hooks/framework-hooks.sh` from a one-line, quote-free `run:`.
2. **Lefthook hands Git Bash a `C:/...` working directory.** Inside a lefthook
   job `$PWD` and `pwd` both print `C:/Users/...`. A `PATH` entry built from it
   splits at the drive colon, so a virtualenv's `Scripts` directory never
   reached `PATH` and pre-commit reported `Executable reuse not found` although
   `reuse.exe` was there. The bridge converts the entry with `cygpath -u` when
   it exists.
3. **reuse needs a pure-Python encoding detector on Windows.** reuse 6.2.0 skips
   `python-magic` on Windows and then fails with `NoEncodingModuleError` unless
   `charset-normalizer` or `chardet` is importable. The pre-commit lock now pins
   `reuse[charset-normalizer]==6.2.0`; Linux keeps using `python-magic` first.
4. **`str(Path)` changes the separator.** `check-container-image-references.py`
   keyed its two local-image exceptions with `/`, but `str(Path(...))` is
   `dev\Containerfile.runner` on Windows, so the runner exception never matched.
5. **Text-mode writes add CR on Windows.** `test_research_digest_ids.py` wrote
   its baseline fixtures in text mode, so the checker's byte-exact
   "deterministic generated form" rule rejected them (10 of 21 tests failed).
   `generate-adr-by-tag.sh --check` reported every `docs/adr/by-tag/` page out
   of date for the same reason, which fails `check-generated-docs` on any commit
   that adds an ADR or a changelog fragment.
6. **`lefthook run` installs hooks.** Without `--no-auto-install`, `lefthook run
   <hook>` syncs the hook shims into Git's hooks directory, which a linked
   worktree shares with the main checkout. A trial run from a scratch worktree
   installed lefthook for every checkout on the host.
7. **`lefthook uninstall` rewrites both agent hook files.** `uninstall_ai.go`
   re-marshals `.claude/settings.json` and `.codex/hooks.json` with Go's
   `json.MarshalIndent` even when it removes nothing: sorted keys, two-space
   indent, one key per line, `<`, `>` and `&` escaped. Reproduced in the scratch
   clone at 12:38:35; this is what dirtied both files in the main checkout at
   12:21:53. `lefthook install` touches neither file while `lefthook.yml` has no
   `ai:` section. Both files are now committed in that form, so an uninstall is
   a no-op.
8. **The two hook managers refused each other.** `make install-hooks` treats a
   lefthook shim as a custom hook and stops, so `commit-msg` and `pre-rebase`
   could not be installed beside lefthook (ADR-1385).
9. **The praetor build on `PATH` matters.** With `25451d8` first on `PATH`,
   `context-check`, `hiss-evidence` and `hiss-audit` failed on an unchanged
   tree; with the pinned `f41e74d8` all three pass.
10. **Some fixture tests assume POSIX tools.** `test_envtest_single_source.py`
    replaces `PATH` with `os.defpath` plus shebang stubs, and
    `test-dedupe-gate.sh` replays `make verify-all` with a POSIX `standardsctl`
    stub that a native Windows make never runs. Both now skip that part on
    Windows with a stated reason; the Linux CI jobs still run them.
11. **The pre-push `security` job loads every Go package.** `govulncheck ./...`
    stopped at `cmd/vmafx-node/bpf/bypass_loader.go: undefined: link.Tracepoint`:
    cilium/ebpf's tracepoint links exist only on Linux, and the package had no
    build constraint although ADR-0996 describes it as Linux-only. Its loader,
    stub and tests are now `//go:build linux`; `gen.go` keeps the package
    non-empty elsewhere. `govulncheck ./...` then passes on Windows.
12. **Pre-push needs more of the toolchain.** The framework's push stage runs
    mypy (`requirements/locks/mypy.txt`) and the MkDocs strict build
    (`docs/requirements.txt`), and `praetorctl flavor audit` expects the
    private `.workingdir` ledgers that the main checkout already has.

## Alternatives explored

- Escaping quotes inside `lefthook.yml`: lefthook passes the value through
  unchanged, and a later edit would reintroduce the bug. A script file plus a
  test that rejects `"` and block scalars in every `run:` value is robust.
- Running reuse from a pre-commit-managed environment (`language: python` with
  `additional_dependencies`): it would add a second, unhashed reuse pin beside
  the lock. Extending the existing lock keeps one install path.
- Making the fixture tests run under a native Windows make: their contracts are
  the Linux Makefile and shell semantics, so a Windows port would test a
  different program.

## Open questions

- `test-configured-lint-driver`, `test-compile-commands-export`,
  `test-tidy-ratchet-lanes` and `test-sync-pelorus-interop` also fail on this
  host. They run only when `Makefile`, `.pre-commit-config.yaml` or their own
  files change. `check-source-adr-citations.py --write` writes its registry with
  CRLF on Windows. Both are tracked as
  `T-HOOKS-WINDOWS-POSIX-FIXTURES-2026-09-30` in [docs/state.md](../state.md).

## Related

- [Local Git hooks](../development/pre-commit-hooks.md) — installation, including
  Windows.
- [ADR-0924](../adr/0924-native-pre-commit-hooks.md) — native formatter mode.
