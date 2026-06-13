- Drop `and Claude (Anthropic)` from the fork's per-file copyright line:
  every in-tree file with the dual-notice header now carries
  `Copyright 2026 Lusoris` (single notice). Anthropic is not a rights
  holder under BSD-3-Clause-Plus-Patent; agent-authorship credit lives
  in commit-message `Co-Authored-By:` trailers, not in the copyright
  notice. Files preserved at the dual-notice form for audit-trail or
  rebase-stability reasons: `AGENTS.md` / `core/AGENTS.md` (in-flight
  PR #234), `docs/rebase-notes.md` (in-flight PR #321), ADR-0025 /
  ADR-0105 / ADR-0312 bodies (ADR immutability), research-0003
  (research audit trail), `ffmpeg-patches/0002` / `0007` / `0008`
  (regenerated separately to preserve git-index hashes). Rule text in
  `CLAUDE.md` §12 r7 and `CONTRIBUTING.md` updated to the new form.
  Documented in [ADR-0861](docs/adr/0861-vmafx-copyright-policy-drop-anthropic.md);
  partially supersedes the format guidance of
  [ADR-0025](docs/adr/0025-copyright-handling-dual-notice.md) and
  [ADR-0105](docs/adr/0105-copyright-handling-dual-notice.md).
- Fix one remaining `--repo-url=lusoris/vmaf` in
  `docs/research/release-preview-3.1.0-lusoris.0.md`'s reproduce-locally
  block (post-cutover URL sweep leftover). All other in-tree
  `lusoris/vmaf` references were already updated by the
  T-POST-CUTOVER-URL-SWEEP-2026-05-28 sweep; the three remaining
  occurrences in `docs/state.md`, `docs/rebase-notes.md`, and
  `changelog.d/changed/post-cutover-url-sweep.md` are historical
  citations of that sweep and are preserved verbatim.
