# ADR-0850: CodeQL config conflict-marker fix and false-positive triage (2026-05-29)

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: ci, security, codeql, fork-local

## Context

`.github/codeql-config.yml` contained two unresolved Git merge-conflict markers
introduced by commit `24bb5daf89` (post-merge-train path-ref sweep). The markers
rendered the YAML invalid, causing all three CodeQL language analyses (C/C++,
Python, Actions) to fail with a config-parse error on every push to `master`.

Separately, the active CodeQL alert list contained a number of findings that
warranted explicit false-positive dismissals to reduce noise in the security
dashboard:

- **Alerts 222–225** (`subprocess-shell-true`): all four instances are in
  `python/test/command_line_test.py`. The `cmd` argument is a hard-coded
  invocation of the vmaf CLI binary — no user-controlled data flows through it.
  The `# nosemgrep:` annotations already acknowledge the pattern for Semgrep;
  CodeQL surfaces the same locations without recognising the test context.

- **Alerts 212–221, 226** (`insecure-hash-algorithm-sha1`): SHA-1 is used across
  `compat/python-vmaf/` and test fixtures exclusively as a non-cryptographic cache
  key (file path hashing, executor cache, memoisation decorator). All sites carry
  `# nosemgrep:` annotations documenting the non-security intent. There is no
  password storage, authentication token, or integrity-verification use.

- **Alerts 161, 162** (secret-scanner false positives on YUV fixture files):
  `resource/yuv/src01_hrc00_576x324.yuv` and `src01_hrc01_576x324.yuv` triggered
  "generic-api-key" and "sourcegraph-access-token" scanners on binary byte sequences
  that coincidentally match token patterns. These are Netflix golden-data YUV
  fixtures used only for correctness testing; they contain no secrets.

Alerts 180–181 (`py/path-injection` in `mcp-server`) were reviewed: the
`_validate_path` allowlist guard validates paths against an explicit allowlist before
any filesystem operation. These are borderline false positives but are retained open
for a follow-on suppression-annotation PR rather than API-dismissed here.

## Decision

1. Resolve conflict markers in `.github/codeql-config.yml`, retaining the
   `core/` path set (post-ADR-0700 rename) and discarding the stale `libvmaf/` set.
2. Dismiss the 14 clearly false-positive alerts documented above via the
   GitHub code-scanning API with `false_positive` reason and inline justification.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `# codeql-query-suppression` inline annotations | Audit trail in code | Scatters ~14 comments across test/compat files; wrong mechanism for fixture findings | API dismissal preferred |
| Add path exclusions to codeql-config.yml for test files | Clean config | Would suppress all CodeQL findings in test files, not just these patterns | Too broad |
| Leave alerts open | No work | Dashboard noise grows; real findings harder to spot | Not acceptable |

## Consequences

- **Positive**: CodeQL analyses parse the config correctly; all three language gates
  unblock. Security dashboard noise reduced by 14 dismissed false positives.
- **Negative**: Dismissed alerts require periodic re-review if scanner rules change.
- **Neutral / follow-ups**: Alerts 180–181 (MCP path injection) to be addressed with
  inline suppression annotations in a separate PR.

## References

- Conflict markers introduced by commit `24bb5daf89`
- PR #181 (merged): earlier CI YAML conflict-marker fix (same root cause pattern)
- ADR-0700: `libvmaf/` → `core/` rename
- req: "Check codeql-config.yml — if it has lingering markers from the 24bb5daf89 disaster, fix here."
