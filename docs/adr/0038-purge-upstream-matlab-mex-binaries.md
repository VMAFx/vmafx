<!-- markdownlint-disable MD013 MD060 -->
# ADR-0038: Purge upstream MATLAB MEX compiled binaries from tree

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: security, matlab, supply-chain

## Context

Netflix/vmaf committed 53 MEX / DLL / `.o` / `.exp` / `.lib` artefacts under what is now `python/vmaf/matlab/` (matlabPyrTools MEX, STMAD_2011_MatlabCode MEX). They are platform-specific compiled outputs — none linked into libvmaf, none exercised by the test suite. OpenSSF Scorecard flagged them as Binary-Artifacts (~500 alerts). User on Scorecard noise: "and that is still there as well" + popup: "Delete all committed MEX/DLL/.o binaries (Recommended)".

## Decision

`git rm` all MEX / DLL / `.o` / `.exp` / `.lib` artefacts under `python/vmaf/matlab/` (`*.mexa64`, `*.mexw64`, `*.mexmaci64`, `*.mexglx`, `*.mexlx`, `*.mexmac`, `*.mexsol`, `*.mexw32`, `*.mex`, `*.mex4`, `*.dll`, `*.o`, `*.exp`, `*.lib`) and block via `.gitignore` (`python/vmaf/matlab/**/*.mex*` + equivalents). The `.c` and `.m` sources stay — anyone needing the MATLAB reference path can rebuild locally with `mex file.c`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep binaries | Zero churn | ~500 Scorecard alerts; not reproducible | Rejected |
| Move to git-lfs | Retains binaries | Still binary artefacts; LFS cost; Scorecard still flags | Rejected |
| `git rm` + `.gitignore` (chosen) | Closes alerts; reproducible path stays | Contributors who need MATLAB path must rebuild | Correct |

## Consequences

- **Positive**: ~500 Scorecard Binary-Artifacts alerts closed in one go; repo size drops; build-from-source trail preserved.
- **Negative**: MATLAB-path users must `mex file.c` themselves (documented).
- **Neutral / follow-ups**: `.gitignore` patterns added; sources (`.c`, `.m`) stay.

## References

- Source: `req` (user: re: Scorecard noise "and that is still there as well" + popup: "Delete all committed MEX/DLL/.o binaries (Recommended)")
- Related ADRs: ADR-0030
