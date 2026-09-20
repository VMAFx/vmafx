# Changelog fragment

- **The NOLINT-citation ratchet now uses exact lexical comment boundaries and
  the tree-wide backlog is closed.** On `origin/master` at `8d0cdd7c4`, the
  shipped scanner reported 46 uncited markers across 25 paths; the exact scan
  finds 63 across 32. Two defects had partially cancelled: 16 markers were
  falsely reported even though their citation was already inside the same block
  or contiguous `//` explanation, while the previous/same/next-line shortcut
  falsely credited 33 markers from outside their own comment. The scanner now
  skips code and string, character, and raw-string literals and accepts an ADR
  only inside the exact `/* ... */` block or contiguous `//` run that owns the
  marker. Of the original 30 genuine violations visible to the old scan, 28
  suppressions were eliminated by anonymous-namespace, kernel, and cleanup-safe
  test refactors or deleted after multi-version clang-tidy probes; only two
  scale-specialised VIF accumulators retain site-specific citations. Of the 33
  violations hidden by adjacency, one dead HIP marker was deleted and 32
  citations moved into the exact marker comment. Four additional already-cited
  no-op markers and two duplicate test suppression pairs were removed in the
  accompanying dead-marker audit. The current exact tree-wide count is zero,
  pinned by 27 focused tests. See
  [ADR-1266](docs/adr/1266-gpu-nolint-citation-closeout-round-2.md).
