- `scripts/release/rollover-changelog-fragments.sh` now cuts release
  candidates: it accepts `--version X.Y.Z-rc.N`, the same shape the tag-time
  verifier accepts, and reads an `-rc.N` version marker as the full version.
  Before this, the verifier required an RC changelog cut that the rollover
  refused to make, so no release candidate could pass publication. The cut
  commit also no longer fails the repository's own gates: the first-release
  index ends cleanly, and the issue-reference provenance and research-digest
  checks follow historical changelog prose into the released changelog.
