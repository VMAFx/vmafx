- ADR tooling: `scripts/adr/next-free.sh --claim` no longer hands two parallel
  callers the same number when one runs from `docs/` and the other from the
  repository root. The lock name now uses the absolute path of the common git
  directory, a caller only releases a lock it holds, and an exclusive claim file
  per number (`.git/adr-claims/NNNN`) decides the number
  ([adr-workflow](docs/development/adr-workflow.md#why-use-claim)).
