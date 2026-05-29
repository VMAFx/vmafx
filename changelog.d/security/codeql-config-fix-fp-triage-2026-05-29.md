- **CodeQL config conflict-marker fix + 14 false-positive dismissals (ADR-0850)** —
  `.github/codeql-config.yml` contained two unresolved Git merge-conflict markers
  from commit `24bb5daf89`, rendering the YAML invalid and failing all three CodeQL
  language analyses (C/C++, Python, Actions) on every push. Markers resolved: the
  `core/` path set (post-ADR-0700 rename) is retained; the stale `libvmaf/` set is
  discarded. Additionally, 14 false-positive code-scanning alerts are dismissed via
  the GitHub API: 4 × `subprocess-shell-true` in hard-coded CLI test invocations
  (alerts 222–225), 10 × `insecure-hash-algorithm-sha1` used exclusively as
  non-cryptographic cache keys with existing `# nosemgrep:` annotations
  (alerts 212–221, 226), and 2 secret-scanner false positives on binary YUV fixture
  files that coincidentally match token patterns (alerts 161–162).
