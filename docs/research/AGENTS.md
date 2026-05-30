# Research Digests — Agent Invariants

## Header ID normalization

Research digest filename IDs and `# Research-NNNN` header IDs must match. Cross-links in the codebase and in other documents use the filename ID (e.g., `docs/research/0033-*.md`), so the header must mirror the filename to maintain consistency.

**Normalization rule:** For any new or renamed research digest file:

- Extract the numeric ID from the filename (e.g., `0033` from `0033-foo.md`)
- Ensure the file starts with `# Research-0033` to match
- If a file lacks a header, add one as the first line (before any subtitle or content)

**Rationale:** Filename IDs are stable cross-link targets (they appear in git diffs, PR descriptions, ADR references, and commit messages). Headers must track filenames to prevent audit mismatches and stale references.
