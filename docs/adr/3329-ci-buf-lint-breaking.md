<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3329: Run buf lint and buf breaking in CI

- **Status**: Accepted
- **Date**: 2026-10-15
- **Deciders**: lusoris
- **Tags**: ci, api, governance, standards

## Context

The repository declares buf for the facet `api:public-contract`: `buf.yaml` (lint `STANDARD`, breaking `WIRE_JSON`) over `proto/` ([ADR-2350](2350-cloud-native-platform.md)), and `build-config.env` pins `BUF_VERSION`. `scripts/codegen/proto_generate.py` runs `buf lint` and, with `--breaking-against`, `buf breaking`, but no workflow and no Makefile target calls it with that option: a pull request could rename or retype a protobuf field and every check stayed green. The governance engine at `3bbde07` makes this a failure (`praetorctl audit`: "Declared linters: buf ... not run by any workflow"), which blocks the engine pin.

## Decision

We will run buf in CI as the job `Buf Lint + Breaking` of `lint-and-format.yml`. The job installs the pinned `BUF_VERSION` with `go install` (the Go checksum database verifies the module), runs `buf lint`, then `buf breaking --against '.git#ref=<base>'`, where the base is the pull request's base commit or, on a `master` push, the previous `master` commit. It follows the Lint workflow's tier gate and is not a required check.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Job in `lint-and-format.yml` (chosen) | Reuses the tier gate and draft skip; no new workflow to register in `.github/ci-tier.json` and the routing contract | The Lint workflow grows by one job | — |
| Separate `buf.yml` workflow | Isolated triggers | Needs its own tier wiring, routing-contract and job-name registry entries for one small job | More moving parts for the same check |
| `bufbuild/buf-action` | Upstream action, annotations | A second pin of buf next to `BUF_VERSION`, which the generator and the suite already use | Two sources for one version |
| Call `proto_generate.py --check --breaking-against` | Also compares the generated Go | The engine does not recognise buf behind the script; the Meson suite already runs the generated-code comparison (`test_proto_generated_current`) | Duplicates the suite and stays invisible to the audit |

## Consequences

- **Positive**: a lint or wire/JSON break in `proto/` fails the pull request that brings it; the engine's declared-linter gate counts buf.
- **Negative**: one more job of about two minutes on each ready pull request (Go module download plus buf build).
- **Neutral / follow-ups**: spectral and trivy, the other two declared linters, get their own pull requests; then the engine pin moves to `3bbde07`.

## Verification

A planted lint defect (`string plantedBadName = 99;` in `proto/vmafx/v1/vmafx.proto`) fails `buf lint` with `FIELD_LOWER_SNAKE_CASE`, and a planted type change of field 2 of `ScoreRequest` fails `buf breaking`; both were seen red in CI on this pull request's throwaway commit ([run 38799060402](https://github.com/VMAFx/vmafx/actions/runs/38799060402), job 116643162025: `buf lint` reported `Field name "plantedBadName" should be lower_snake_case`, `buf breaking` reported field 2 of `ScoreRequest` changed type from `string` to `int64`, each step exit 100) and then removed.

## References

- Operator decision `vmafx-engine-pin-declared-linters` (2026-10-15, paraphrased): one small pull request per declared linter, each seen failing on a planted defect, before the engine pin.
- [API generation](../development/api-generation.md), [ADR-2350](2350-cloud-native-platform.md).
