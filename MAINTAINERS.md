# Maintainers

Authoritative list of people with write access to the VMAFX fork and
the subtrees they own. Reviewers route to the matching CODEOWNERS
entries in [`.github/CODEOWNERS`](.github/CODEOWNERS).

For the governance model these roles fit into, see
[`GOVERNANCE.md`](GOVERNANCE.md).

## Project lead (BDFL)

| Name      | GitHub                                                 | Scope        |
|-----------|--------------------------------------------------------|--------------|
| Lusoris   | [@Lusoris](https://github.com/Lusoris)                 | All subtrees |

## Maintainers

Currently the BDFL is also the sole maintainer — every subtree maps to
[@Lusoris](https://github.com/Lusoris). As new maintainers come on, they
will be added here with the subtree(s) they own, and the
[`CODEOWNERS`](.github/CODEOWNERS) file will be updated in the same PR.

| Subtree                                  | Maintainer(s) | CODEOWNERS row                  |
|------------------------------------------|---------------|---------------------------------|
| C core (`core/`)                         | @Lusoris      | `/core/`                        |
| Python harness (`compat/python-vmaf/`)   | @Lusoris      | `/compat/python-vmaf/`          |
| GPU backends (CUDA, SYCL, Vulkan, HIP)   | @Lusoris      | `/core/src/{cuda,sycl,vulkan}/` |
| SIMD paths (AVX, NEON)                   | @Lusoris      | `/core/src/feature/{x86,arm64}/`|
| Tiny-AI (`ai/`, `core/src/dnn/`)         | @Lusoris      | `/ai/`, `/core/src/dnn/`        |
| MCP servers (`mcp-server/`)              | @Lusoris      | `/mcp-server/`                  |
| Distributed platform (`cmd/`)            | @Lusoris      | `/cmd/`                         |
| Deployment (`deploy/`, `docker/`, `dev/`)| @Lusoris      | `/deploy/`, `/docker/`, `/dev/` |
| CI / workflows                           | @Lusoris      | `/.github/workflows/`           |
| Build system                             | @Lusoris      | `/Makefile`, `/core/meson.build`|
| ADRs / docs                              | @Lusoris      | `/docs/adr/`, `/docs/`          |

## Becoming a maintainer

New maintainers are invited at the BDFL's discretion based on a track
record of high-quality contributions in a specific subtree (typically
3+ merged non-trivial PRs reviewed without major rework requests).

To propose adding a maintainer, open an issue or PR that:

1. Updates this `MAINTAINERS.md` with the new entry.
2. Updates [`.github/CODEOWNERS`](.github/CODEOWNERS) for the subtree(s)
   they will own.
3. Cites the contribution history justifying the addition.

The BDFL approves the PR and the new maintainer is granted write access.

## Stepping down

A maintainer may step down at any time by opening a PR that removes
their entry from this file and the corresponding `CODEOWNERS` rows.
The PR is merged immediately; no further approval is required.

## Inactive maintainers

A maintainer who has been inactive for 12 months (no reviews, no
commits, no triage activity) may be moved to an `## Inactive` section
here by BDFL action. Inactive maintainers retain commit credit but no
longer block PR review routing. They can be reinstated by opening a
PR that moves their entry back.

(No inactive maintainers at this time.)
