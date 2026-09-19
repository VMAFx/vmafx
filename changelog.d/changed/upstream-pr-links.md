- **The fork now records the eight pull requests it has open against
  Netflix/vmaf, and six upstream defects it found but did not report.**
  `docs/development/known-upstream-bugs.md` lists each contribution against the
  fork's own bug id, and states where the upstream fix differs from the fork's:
  upstream's 16-bit DWT overflow patch starts the sum from the normalization
  offset instead of widening the accumulator, which costs nothing and is worth
  bringing back here. Three `docs/state.md` rows cite the upstream pull request
  that carries their fix.
