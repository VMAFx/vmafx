# Dependency advisories

This page covers how a security advisory against a third-party dependency is
triaged. Vulnerabilities in VMAFx's own code are reported through
[SECURITY.md](https://github.com/VMAFx/vmafx/blob/master/SECURITY.md).

## Where advisories show up

- **Python:** the dependency dashboard (issue #941, Renovate) lists OSV
  advisories against a declared dependency. It lists one entry per package that
  declares the dependency.
- **Go:** the Go vulnerability database, through the govulncheck gate below,
  in the `Go CI` workflow and `make govulncheck`.

## Triage, in order

1. **A fixed release exists.** Update to it. Renovate opens the pull request;
   it lands like any dependency update.
2. **No fixed release, and the dependency is not needed where it is
   declared.** Remove it from that package. torch is the standing example: it
   may appear only in the two training environments (`ai/`,
   `tools/ensemble-training-kit/`), and `scripts/ci/check-torch-scope.py` fails
   when any other package names a torch-family distribution in any dependency
   group, or when one of its locks resolves one
   ([ADR-1886](../adr/1886-torch-training-environments-only.md)).
3. **No fixed release, and the dependency is needed.** Record an
   [OpenVEX](https://github.com/openvex/spec/blob/main/OPENVEX-SPEC.md)
   statement under `security/vex/`. A `not_affected` statement names one of
   the specification's five justifications and an `impact_statement` with the
   evidence: which function the advisory concerns, and where the code does not
   reach it. `scripts/ci/tests/test_openvex_documents.py` rejects a malformed
   document.
4. **The code is affected.** Fix or mitigate it under the timelines of
   SECURITY.md, and record the statement as `affected` with an
   `action_statement`.

Re-read a statement when the dependency publishes a fix, or when our code
starts to call an affected function.

## Current statements

| Document | Dependency | Advisories | Products | Status |
| --- | --- | --- | --- | --- |
| `security/vex/go.openvex.json` | golang.org/x/crypto v0.57.0 | GO-2026-5932 (the deprecated `openpgp` packages; no fixed version) | vmafx Go module | `not_affected`, `vulnerable_code_not_present`: no vmafx binary compiles an `openpgp` package |
| `security/vex/torch.openvex.json` | torch 2.14.1 | PYSEC-2025-189, -190, -192 to -197, -210 (no fixed release in OSV) | vmaf-train (`ai/`), vmaf-ensemble-training-kit | `not_affected`: no profiler, RNN, TorchScript loading or eager-quantized modules in the training code; `torch.jit.script` and the CUDA allocator never receive adversary input |

## The Go gate

`scripts/ci/govulncheck-gate.py`
([ADR-1899](../adr/1899-govulncheck-symbol-gate-openvex.md)) runs govulncheck
(`GOVULNCHECK_VERSION` in `build-config.env`) at symbol level and judges each
advisory by its most specific finding:

| Finding | Gate |
| --- | --- |
| vmafx calls a vulnerable symbol | fails: update the module or stop calling it |
| a package of the advisory is imported, nothing calls it | fails unless `go.openvex.json` has a `not_affected` statement whose justification says why the code cannot be reached (`vulnerable_code_not_in_execute_path`, `vulnerable_code_cannot_be_controlled_by_adversary`, `inline_mitigations_already_exist`) |
| only the module is required | fails unless `go.openvex.json` has any `not_affected` statement |
| govulncheck did not complete | exit 2, never a pass |

Run it with `make govulncheck`. It needs network access (the govulncheck
module and the vulnerability database) and a C compiler for cgo's type
information, but no libvmaf build: nothing is linked.

A module that calls `golang.org/x/text/language.ParseAcceptLanguage` at
`golang.org/x/text` v0.3.7 fails it with
`GO-2022-1059: vmafx calls golang.org/x/text/language.ParseAcceptLanguage`
(govulncheck v1.8.0, 2026-10-05). Without `go.openvex.json` the repository
fails it with `GO-2026-5932: module golang.org/x/crypto has no not_affected
statement`.

### Code the Go build does not need

An advisory against a package nothing uses is cheapest to close by dropping
the package from the build (step 2 above). `go mod why -vendor <package>`
names the import chain from vmafx code; without `-vendor` it also follows the
tests of other modules. On 2026-10-05 it showed that `golang.org/x/crypto/md4`
(NTLM SMTP authentication in a mail client) and `golang.org/x/crypto/argon2`
(a password-hashing helper) reached the binaries only because three files took
the golusoris `HTTP` and `Core` bundles from its root package, which imports
every module golusoris has. `internal/app/bootstrap` now composes the modules
vmafx uses as `bootstrap.Core` and `bootstrap.HTTP`; both packages and 59
modules, the mail client and the helper among them, left the build
([ADR-1899](../adr/1899-govulncheck-symbol-gate-openvex.md)). Do not import
the golusoris root package again; take its sub-packages.

## Checking a change locally

```bash
python3 scripts/ci/check-torch-scope.py
python3 -m unittest scripts/ci/tests/test_check_torch_scope.py scripts/ci/tests/test_openvex_documents.py
```

The check also runs as the `check-torch-scope` pre-commit hook, and the
`Lint` workflow (`lint-and-format.yml`) runs every pre-commit hook.
