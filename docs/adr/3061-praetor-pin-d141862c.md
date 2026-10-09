<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3061: Move the praetor pin to d141862c430b so the Go API gate builds its checker against golang.org/x/tools v0.51.0

- **Status**: Accepted
- **Date**: 2026-10-09
- **Deciders**: lusoris
- **Tags**: ci, governance, standards

## Context

The required `Go API Compatibility` check (`.github/workflows/praetor-api.yml`) fails on every pull request with "the checker reported the canary's removed exported function as compatible, so it cannot read the packages this Go toolchain builds". Push runs on master pass, because without a root release tag there is no base to compare.

The workflow installs Go with `actions/setup-go` and `go-version: stable`. That spec resolved to 1.27.1 up to the last passing pull request run (37869986532, 01:29 UTC) and to 1.27.2 afterwards. Go 1.27.2 writes export data version 5.

The gate (`tools/apicompat/gate/main.go`) installed its checker as `go install github.com/joelanford/go-apidiff@v0.8.4-0.20260910211158-c3e0953fa2fd`. That module requires `golang.org/x/tools` v0.49.0, which reads export data up to version 4. The checker therefore reads every package as empty and passes every module. The gate's canary, a module whose second commit removes an exported function, turns that silent pass into the failure above.

Measured locally on a two-commit canary repository with the checker built at x/tools v0.49.0:

- `GOTOOLCHAIN=go1.27.1`: exit status 1, "Removed: removed".
- `GOTOOLCHAIN=go1.27.2`: exit status 0. This holds whether the checker itself was built with 1.27.1 or 1.27.2.

The same go-apidiff commit built in a wrapper module that raises `golang.org/x/tools` to v0.50.0 or v0.51.0 (minimal version selection allows both) exits 1 under both toolchains. Upstream go-apidiff has no newer commit.

Both files are praetor-managed. `praetorctl audit` locks `praetor-api.yml` and the gate program byte for byte ("differs from the locked Praetor asset", checked with the engine at the current pin), so neither can be fixed here. Praetor fixed the gate in cordanaLLM/praetor#1051 (`d141862c430b`, closes #1049), green on its own CI. The gate now writes a build module that requires go-apidiff at the same commit and `golang.org/x/tools` v0.51.0 exactly, with its go.sum, and builds the checker there with `-mod=readonly`.

Between the current pin `3a766f2d56ad` and `d141862c430b` lie six praetor commits. On a clean copy of master, the engine at `d141862c430b` reports one stale file, `tools/apicompat/gate/main.go` ("holds an earlier Praetor text"). Its audit warnings equal the old engine's apart from one new advisory: AGENTS.md carries no cache-band markers.

## Decision

Move `PRAETOR_REF` in `.github/workflows/standards-gate.yml` to `d141862c430b3cbc3712976b4db4b9d1fbb12269` under [ADR-1351](1351-praetor-engine-pin-move.md)'s conditions:

- `praetorctl adopt --lock-source-root=<praetor clone at the pin>` runs in a throwaway checkout, without `--force`.
- Only the file audit reported stale, `tools/apicompat/gate/main.go`, is copied back.
- The agent settings, `.codex/hooks.json` and `.config/lefthook/python.sh` that `adopt` also touches are not copied back.

The workflow keeps `go-version: stable`. It is praetor's text, and the Go version that the API gate installs belongs in that template. cordanaLLM/praetor#1037 asks for `go-version-file: go.mod`, so that the gate would follow the `go` directive, which tracks this repository's Go release.

## Alternatives considered

| Option | Why not |
| --- | --- |
| Edit `praetor-api.yml` here: build the checker in a wrapper module and pass `-checker`, or pin the Go version through `build-config.env` | Audit locks the file: "differs from the locked Praetor asset". A local copy would fail the Standards gate and be overwritten by the next adopt. |
| Edit the gate's `checkerModule` here | Same lock: "holds an earlier Praetor text". |
| Move the pin to praetor main (`4053fcc549b5`) | Five more commits, adopter-facing ones among them (managed `.gitignore` blocks, register skills), with no part in this failure. The smallest move that carries the fix is the fix commit itself. |
| Fork go-apidiff or raise x/tools in a vendored copy | Upstream go-apidiff builds with a newer x/tools once its consumer requires one; the build module does exactly that, without a fork. |

## Consequences

- The `Go API Compatibility` check runs its comparison again on pull requests: the canary is found incompatible and the modules are compared.
- The next Go release that changes the export data again fails the canary closed, as it did today. The fix is then a newer x/tools in praetor's build module, which praetor's Renovate rule tracks.
- The hook engine moves with the pin: `praetor-d141862c/bin` (HOOKS-ENV). The old engine fails `audit` on the new gate text.

## References

- Source: CI triage of 2026-10-09, the "Praetor API Compatibility" red on every pull request (job 113983934223).
- cordanaLLM/praetor#1049, #1051 (the gate fix), #1037 (setup-go `stable`).
- [ADR-1351](1351-praetor-engine-pin-move.md), [ADR-2784](2784-praetor-pin-3a766f2d.md).
