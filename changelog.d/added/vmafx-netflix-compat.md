### `--netflix-compat` flag for legacy-default opt-out (ADR-0696)

- Add `--netflix-compat` / `--netflix_compat` CLI flag to both the `vmaf` and
  `vmafx` binaries.
- When passed, forces the full set of Netflix-upstream legacy defaults as the
  final post-parse pass: `--backend=cpu` (all GPU disabled) and
  `--precision=legacy` (`%.6f` output).
- Idempotent on the `vmaf` binary (already the defaults); the primary use case
  is `vmafx --netflix-compat` to override vmafx-mode modernizations.
- Documented in `docs/usage/vmafx-cli.md` under `--netflix-compat`.
