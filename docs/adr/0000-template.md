<!-- markdownlint-disable MD013 MD041 MD060 -->
> **Allocator**: run `scripts/adr/next-free.sh --claim <your-topic-slug>` to atomically
> reserve a number before creating this file.  The command creates a
> `docs/adr/NNNN-<slug>.md.stub` placeholder that prevents parallel agents from
> claiming the same number.  Rename the stub to `.md` when you commit the real ADR.
> If you abandon the PR, run `scripts/adr/next-free.sh --release <NNNN>` to free the slot.
> The pre-commit hook (`check-adr-numbering`) and the CI gate (`adr-collision-check` in
> `rule-enforcement.yml`) remain the hard backstop for non-compliant callers.
> See [ADR-0386](0386-adr-numbering-collision-prevention.md) and
> [ADR-0535](0535-adr-atomic-allocator.md).

# ADR-0000: <short, declarative title>

- **Status**: Proposed | Accepted | Deprecated | Superseded by [ADR-NNNN](NNNN-title.md)
- **Date**: YYYY-MM-DD
- **Deciders**: <names / handles>
- **Tags**: <comma-separated area tags — e.g. `ci`, `security`, `cuda`, `simd`, `ai`, `build`, `docs`, `license`, `workspace`, `agents`>

## Context

What problem are we solving? What forces are at play (technical, organisational, regulatory)? Cite constraints from [principles.md §2](../principles.md) (NASA/JPL Power of 10, SEI CERT, SLSA, etc.) where relevant.

Keep it short. One or two paragraphs. The reader should understand *why a decision was needed*, not the full history.

## Decision

State the decision in active voice: "We will use X." One paragraph max.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Option A | … | … | … |
| Option B | … | … | … |

At minimum list the runner-up. If the alternatives section is empty, the decision wasn't real — it was a default.

## Consequences

- **Positive**: what becomes easier, faster, safer.
- **Negative**: what becomes harder, slower, more expensive (operationally or cognitively).
- **Neutral / follow-ups**: things that must happen because of this decision (new tests, docs, migration steps, deprecation timeline).

<!-- Optional: the following three sections (Supply-chain impact, SBOM delta,
     Carbon / footprint) are OPTIONAL. Include them when the decision adds /
     removes a dependency, fetches build-time artifacts, changes the image
     layer set, or measurably shifts build-time / runtime cost. Delete the
     section header entirely when not applicable rather than leaving an empty
     "N/A" stub. -->

## Supply-chain impact

<!-- Optional -->

- **New dependencies**: list any added runtime / build-time / test-time deps
  (name, version pin, license, upstream URL). Mark each as `runtime`,
  `build`, `test`, or `dev`.
- **Removed dependencies**: what drops out (and is the removal *complete*, or
  does a transitive caller still pull it in?).
- **Build-time fetches**: new `wget` / `curl` / `git clone` / `pip install`
  / `apt-get` operations in `dev/Containerfile`, `docker/`, or CI workflows.
  Note whether the artifact is pinned by digest / hash.
- **Sigstore-signable**: can the new artifact be verified via Sigstore /
  cosign / SLSA provenance? If not, what's the trust anchor?
- **CVE surface delta**: rough estimate — does this widen or narrow the
  attack surface (new C library, new network listener, new privileged
  syscall)? Cite known CVEs if porting around them.

## SBOM delta

<!-- Optional. Use the cyclonedx-flavoured snippet below; delete the
     components array if nothing changes. The release pipeline reconciles
     declared deltas against the generated CycloneDX SBOM. -->

```yaml
# Components added (cyclonedx 1.5 fragment)
components:
  - type: library
    name: <package>
    version: <pin>
    purl: pkg:<ecosystem>/<name>@<version>
    licenses:
      - license:
          id: <SPDX-ID>
# Components removed
removed:
  - name: <package>
    version: <pin-that-was-in-tree>
```

## Carbon / footprint

<!-- Optional. Numbers can be rough — order-of-magnitude is the point.
     Use vmaf-bench / `meson test --benchmark` / `docker history --human` /
     `time -v` to estimate. Cite the measurement command so a future reader
     can re-run it. -->

- **Image size**: `<image>` MiB before → MiB after (Δ MiB). Source:
  `docker image inspect <image> --format '{{.Size}}'` or
  `docker history <image>`.
- **Build-time**: full `ninja -C build` wall-clock before → after (Δ s).
  Source: `time ninja -C build` on the dev-mcp container.
- **Runtime energy**: rough estimate of per-frame / per-job energy delta on
  the dominant backend (CPU package power × wall-time, or GPU
  `nvidia-smi --query-gpu=power.draw` integrated over the run). Cite the
  measurement command.

## References

- Upstream docs, RFCs, blog posts, prior ADRs.
- Related issues / PRs: `#NNN`.
- Source: `req` (direct user quote) or `Q<round>.<q>` (verbatim popup answer from `.workingdir2/decisions/questions-answered.md`).
