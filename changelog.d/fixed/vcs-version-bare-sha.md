- Fixed `vmaf --version`, the JSON/XML `version` field and `vmaf_version()`
  reporting a bare commit abbreviation (e.g. `abafdfc`) instead of a version
  whenever the build tree could not reach a `v*.*.*` tag — a shallow CI
  checkout, a tarball export, or a worktree whose `.git` is a file. The
  version is now always a real version string: `v3.1.0-2417-gabcdef0` when a
  tag is reachable, and the project version otherwise.
