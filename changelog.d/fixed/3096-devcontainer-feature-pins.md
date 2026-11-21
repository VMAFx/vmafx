- **Dev container:** the generated `.devcontainer/devcontainer.json` keeps its
  major feature tags, while `.devcontainer/devcontainer-lock.json` records the
  exact versions (`2.7.1`, `1.4.1`, `1.6.1`) and `@sha256` digests for
  common-utils, nix and rust. `check-base-image-single-source.sh` rule 5 refuses
  a feature with no lock record or a mutable version/digest (issue 3096).
