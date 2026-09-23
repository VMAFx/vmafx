- Repair every ADR-to-ADR citation under `docs/adr/`, and teach
  `scripts/ci/check-adr-links.py` to see them. The checker matched only the
  qualified `adr/NNNN-slug.md` form, so the bare `(NNNN-slug.md)` spelling ADRs
  use to cite each other was invisible to it: 237 sibling links resolved to no
  file while the gate reported the tree clean. 181 were repaired mechanically
  and the remaining 56 by hand.
- Stop repairing a link from its number alone when the ADR carrying that number
  is about something else. A number outlives the decision it named — the
  collision sweeps reallocated many — so `[ADR-0033](0033-hip-applicability.md)`
  would have become a link to `0033-codeql-config-moved-to-github.md`, which
  resolves, reads as authoritative, and is worse than the dead link it replaced.
  A by-number repair now requires the target to mention what the citation's slug
  says it is about, weighing the words that identify a decision (`iir`, `hvs`)
  over the words a whole family of ADRs shares (`simd`, `bitexact`).
- Resolve a citation whose slug words were reordered from the slug half rather
  than the number half: `0335-sycl-adaptivecpp-second-toolchain` is
  `0407-adaptivecpp-second-sycl-toolchain` with two words swapped, and 0335 now
  belongs to an unrelated ADR about hardware capability priors.
- Relink twelve citations that were left as plain text on the assumption their
  ADR was never written. Each one's decision is recorded; the search just had
  to reach past the two halves of the filename into the git history of the
  citing commit. `[ADR-0122](0122-cuda-framesync-segfault-hardening.md)` is the
  clearest: the citing sentence says "fork PR #60 CUDA framesync hardening",
  and `d3b6fad62` is both PR #60 and the commit that created
  `0122-cuda-gencode-coverage-and-init-hardening.md`. The number was right all
  along; the slug had been minted from a `docs/state.md` bug-row label, and the
  ADR never uses the word "framesync", which is why a text-match check
  dismissed it.
