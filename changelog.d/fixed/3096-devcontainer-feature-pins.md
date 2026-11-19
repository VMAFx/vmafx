- **Dev container:** the common-utils, nix and rust features are pinned to
  exact releases (2.7.1, 1.4.1 and 1.6.1); Renovate now tracks them,
  `check-base-image-single-source.sh` refuses a mutable major tag, and a digest
  remains an accepted exact form (issue 3096).
