- **Three source citations pointed at ADR numbers that do not exist.**
  Under rule 12 an inline `ADR-NNNN` citation *is* the justification for
  a NOLINT or a non-obvious invariant, so one that resolves to nothing
  sends the reader nowhere — and one that resolves to an unrelated ADR
  is worse, because it looks authoritative. An audit of all 533 ADR
  numbers cited from source found 11 with no `docs/adr/NNNN-*.md`.
  Three are corrected, each confirmed by both the citing context and the
  target ADR's own title and body:
  `ADR-0900` → `ADR-0913` (changelog splice contract),
  `ADR-0553` → `ADR-0564` (real integer_ssim GPU kernels),
  `ADR-0204` → `ADR-0206` (ssimulacra2 CUDA + SYCL twins — its cited
  siblings ADR-0192 and ADR-0201 both exist, so only this number was
  wrong).
  The rest are deliberately left, because they are not all the same
  thing: `ADR-0722` is *correctly* cited — ADR-0725's own title records
  that it supersedes it, so the file is retired by design. `ADR-0557`
  and `ADR-0558` are numbers ADR-0559 records as claimed by parallel
  agents for CUDA and HIP `speed_*` twins that never landed — dangling
  plan, not typo. `ADR-0049`, `ADR-0322`, `ADR-0572` and `ADR-1214`
  have no confident target; a plausible-looking renumber would make the
  audit trail worse, not better.
