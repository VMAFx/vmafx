- `CHANGELOG.md`'s Unreleased block regenerated from `changelog.d/`, which
  `make docs-fragments-check` had been failing on across every branch — the gate
  is a required check, and it fails on pristine master whenever merged PRs add
  fragments without the block being re-rendered.
