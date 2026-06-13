Fix post-rename CI path drift: replace stale `libvmaf/` source-directory
references in workflow YAML with `core/` following the ADR-0700 repo-layout
rename. Fixes CodeQL, Gitleaks, and Docker/FFmpeg/nightly workflow failures
that blocked all merges. Also enables repository dependency graph and replaces
`gitleaks-action` (requires org license) with the free gitleaks CLI binary.
