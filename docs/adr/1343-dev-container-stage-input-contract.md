<!-- markdownlint-disable MD013 MD060 -->
# ADR-1343: Statically check that every dev-container stage copies the files it reads

- **Status**: Accepted
- **Date**: 2026-09-26
- **Deciders**: lusoris
- **Tags**: ci, build, dev-container, supply-chain

## Context

[ADR-0819](0819-dev-container-ci-gate.md) keeps the pull-request container gate under the runner time budget by building `dev/Containerfile` only up to the `libvmaf-build` stage. The final `dev-mcp` stage is never built in CI. When the pre-RC1 train landed (`dd51d00db`), that stage started reading `/build/vmaf/requirements/locks/package-build.txt` for its hash-locked pip install, but no stage copied `requirements/`. Every check was green, and the first local `dev/scripts/dev-mcp-up.sh` failed with "Could not open requirements file". Since the container is canonical for published artifacts ([ADR-1102](1102-phase4b9-container-only-publishing.md)), an unbuildable final stage is a release risk that the existing gates could not see.

## Decision

We add `scripts/ci/check-dev-container-stage-inputs.py`, a static contract that needs no Docker daemon. It resolves each stage's parent lineage, WORKDIR and non-`--from` `COPY` instructions, then requires that every pip requirement or constraint file a `RUN` reads under `/build/vmaf/` is provided by a `COPY` into that stage or a parent stage and maps to an existing repository file, and that every non-`--from` `COPY` source exists. It reuses the instruction parser of `scripts/ci/check-container-image-references.py`, now exposed as `logical_instructions()`. It runs from a pre-commit and pre-push hook, and therefore in the required Pre-Commit CI job, whenever the Containerfile, a lock file or the checker changes. ADR-0819's build target is unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Static stage-input contract (chosen) | Runs in seconds without Docker; catches the missing-input and missing-source defect classes in every stage, including ones CI never builds | Does not execute the stage, so a failing command with present inputs still passes | Covers the defect that actually escaped at negligible cost |
| Build the `dev-mcp` target in the PR-time CI job | Catches every defect the build can hit | Tens of runner minutes per container-touching PR on top of the libvmaf build, against the budget ADR-0819 set | Cost is disproportionate to the defect class seen so far |
| Keep the gap deferred (`T-DEV-CONTAINER-DEVMCP-STAGE-UNGATED-2026-09-26` open) | No work | The same defect can recur silently | Rejected by the maintainer |

## Consequences

- **Positive**: a stage that reads an uncopied lock file, or copies a missing path, now fails locally and in CI before merge. The shared parser means one implementation of Dockerfile continuation and escape handling.
- **Negative**: the check models only pip requirement and constraint reads; other input kinds need an explicit extension.
- **Neutral / follow-ups**: a full `dev-mcp` build in CI remains possible under a future ADR if a defect escapes this contract.

## References

- Popup, 2026-09-26: "CI never builds the final dev-mcp container stage (ADR-0819 stops at libvmaf-build), which is how the broken stage reached master green. How should that gap be closed?" Answer: "Cheap static check (Recommended)".
- [ADR-0819](0819-dev-container-ci-gate.md) — PR-time container gate scope.
- [ADR-1102](1102-phase4b9-container-only-publishing.md) — the container is canonical for published artifacts.
- [ADR-1231](1231-base-image-single-source.md) — base-image reference gate whose parser this check reuses.
