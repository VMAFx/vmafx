- `scripts/adr/next-free.sh --claim <slug>`: atomically reserves the next free ADR
  number by creating a `docs/adr/NNNN-<slug>.md.stub` marker under a POSIX-mkdir
  lock, preventing same-host parallel agents from claiming the same number. A
  `--release <NNNN>` flag removes abandoned stubs. Remote-branch awareness scans
  in-flight origin branches so cross-branch collisions are also avoided at claim
  time. Smoke-test suite added at `scripts/adr/test-next-free.sh`.
  (ADR-0532, closes the 2026-05-18 renumber-storm discipline issue.)
