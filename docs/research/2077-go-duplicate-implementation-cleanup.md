# Research-2077: Go duplicate implementation cleanup

## Scope and reproducer

The repository's dedicated clone detector failed on nine Go function
families even though the broader Praetor audit passed:

```text
praetorctl dedupe scan .
Duplicate Function Blocks (9)
```

The findings were real shared-behaviour drift risks, not generated noise or
Netflix-origin exceptions. They covered both Go scoring services, model CLI
formatting, registry enumeration, backend errors, and the hand-maintained
Stage-1 Kubernetes deep-copy file. No suppression or threshold change was
considered an acceptable fix.

The investigation also confirmed the CI blind spot: `standardsctl audit`
could pass while `standardsctl dedupe scan .` failed, and no workflow or
blocking local hook invoked the latter. The only previous integration was a
post-commit cadence hook, which runs too late to prevent a commit and is not a
required hosted check.

## Ownership after cleanup

| Reported family | Shared owner | Preserved contract |
| --- | --- | --- |
| `provideMetrics` in controller/server | `internal/app/scoringservice.ProvideMetrics` | Isolated registry with Go, process, and VMAFx collectors |
| `VmafxJob` / `VmafxNode` `DeepCopyInto` | `api/vmafx/v1.deepCopyResource` | Independent metadata, value-copied specs, status deep copies |
| `Known` / `KnownFilters` map sorting | Standard `maps.Keys` + `slices.Sorted` | Deterministic ascending names |
| controller/server `handleReadyz` | `scoringservice.HandleReadyz` | Exact status, body, content type, request metric, and 405 behavior |
| controller/server `handleHealthz` | `scoringservice.HandleHealthz` | Exact status, body, content type, request metric, and 405 behavior |
| controller/server/REST JSON writers | `scoringservice.WriteJSON` | `SetEscapeHTML(false)`, trailing newline, caller-selected status |
| bisect/scorecli `modelArg` | `pkg/model.CLIArgumentOrDefault` | Empty selects `DefaultVersion`; explicit `key=value` passes through |
| corpus/scorebackend backend error | `scorebackend.UnavailableError` with corpus type alias | Existing exported corpus type identity and diagnostic text |
| controller/server `provideScorer` | `scoringservice.ProvideScorer` | Config keys, startup log fields, fx close-hook ordering |

The model formatter also replaces equivalent copies in `pkg/corpus`,
`pkg/fast`, and `pkg/tune/executor`. `CLIArgument` intentionally retains the
corpus boundary where an empty input formats as `version=`; callers that own a
default use `CLIArgumentOrDefault`. This avoids silently changing existing
argv contracts while still leaving one formatter implementation.

Response serialization keeps the previous wire bytes. Failure handling is now
explicit: encoder/socket writes and scratch cleanup failures are logged, and a
zero-exit corpus score with no JSON sidecar is recorded as exit 65 instead of
silently retaining exit 0 with a NaN score.

## Alternatives considered

| Option | Result | Decision |
| --- | --- | --- |
| Suppress generated/upstream-labelled findings | Leaves actual fork-maintained duplicates and drift risk | Rejected |
| Lower the clone detector's sensitivity or make it advisory | Hides this and future regressions | Rejected |
| Rename/reformat copies until hashes differ | Preserves semantic duplication | Rejected |
| Move each behavior to its narrowest shared owner | One implementation with existing public wrappers/types retained | Chosen |
| Rely on the post-commit cadence hook | Reports after the commit and leaves hosted CI blind | Rejected |
| Run an explicit scan in local hooks, `make verify-all`, and the required Standards job | Same fail-closed command at every delivery boundary | Chosen |

No ADR is needed. This is a bug/debt repair with no new architecture policy;
the only acceptable direction under the existing no-suppression rules is to
give each behavior one owner.

## Verification

- `praetorctl dedupe scan .`: 171 files and 954 functions scanned, 100.0%,
  zero duplicate blocks.
- `standardsctl audit`: pass; all 33 touched files are HISS-clean, including
  the composition/scoring functions and cleanup paths exposed by this change.
- `CGO_ENABLED=0 go test ./internal/app/scoringservice ./api/vmafx/v1
  ./pkg/model ./pkg/bisect ./pkg/scorecli ./pkg/corpus ./pkg/fast
  ./pkg/tune/executor ./pkg/codecadapter ./pkg/prefilter ./pkg/scorebackend`:
  all pass.
- `CGO_LDFLAGS=-L.../core/build-cpu/src LD_LIBRARY_PATH=.../core/build-cpu/src
  go test ./internal/app/scoringservice ./cmd/vmafx-server
  ./cmd/vmafx-controller`: all pass, including composition, lifecycle, and
  wire behavior.
- With a short home-filesystem `TMPDIR` (the host `/tmp` tmpfs was 99% full),
  `go test ./...` and `go vet ./...`: all pass.
- `gosec -exclude-generated` over every changed Go package: pass.
- `pre-commit run --files <changed-files>`: all applicable hooks pass,
  including YAML, shellcheck, shfmt, Markdown, generated-document freshness,
  fail-closed CI, and the duplicate-gate contract.
- `scripts/ci/tests/test-dedupe-gate.sh`: proves the required Standards step has
  no failure masking, both blocking local hook stages call the scan, and a
  fake scanner failure makes real `make verify-all` return nonzero.

## Delivery declarations

- Human documentation: the operator-visible response-write diagnostic is
  recorded in `docs/development/observability.md`.
- ADR: none; implementation duplication bug, no policy decision.
- Rebase impact: preserve the shared owners instead of resolving future
  upstream changes back into per-binary or per-package copies. Preserve the
  explicit scan in the Standards workflow, both lefthook stages, and
  `make verify-all`; `standardsctl audit` is not a replacement.
