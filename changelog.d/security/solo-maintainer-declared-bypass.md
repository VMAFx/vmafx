- **The master ruleset declares its one bypass actor instead of pretending to
  have a reviewer.** ADR-1248 created ruleset `VMAFx master security` with no
  bypass actors and one required independent approval. `VMAFx` has exactly one
  collaborator, who authors every pull request, and GitHub forbids approving your
  own pull request — so the requirement had nobody who could satisfy it. Nothing
  merged between 2026-09-08 and 2026-09-15: not #1396 at 70 passing checks
  carrying the container fixes `master`'s own Docker and FFmpeg SYCL lanes
  needed, not the security bumps #1428 and #1429, not any of the pre-rc.1 fixes.
  The ruleset keeps every other control — one required approval,
  dismiss-on-push, last-push approval, thread resolution, strict up-to-date
  `Required Checks Aggregator`, linear history, blocked deletion and force
  pushes — and gains exactly one `User` bypass actor named by numeric id, not a
  role tier. `scripts/dev/check_repository_security.py` changes from "no bypass
  actors" to "exactly the declared actors", so an undeclared actor, a second
  actor beside the declared one, and a widening from `always` to `exempt` all
  still fail the gate. The bypass waives the approval, never the tests.
  ADR-1252, superseding ADR-1248.
