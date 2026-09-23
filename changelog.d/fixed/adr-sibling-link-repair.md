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
