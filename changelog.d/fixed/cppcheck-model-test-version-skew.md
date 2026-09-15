- **The cppcheck model test passes on the cppcheck CI actually installs.** Its
  public-roots case asserted a non-zero exit code when an unlisted unused
  function is reported. cppcheck 2.21 propagates a whole-program
  `unusedFunction` finding to the exit code; Ubuntu 24.04's 2.13 — what the
  workflow gets from `apt` — prints the identical diagnostic and still exits 0
  under `--cppcheck-build-dir`, so the job failed on CI while passing on a
  developer machine with a newer tool. The contract the test defends is which
  functions get reported, and that is now asserted unconditionally; the exit
  code is checked for consistency with what the running tool does, learned from
  the case's own first probe rather than matched against a version number. A
  tool that stopped reporting still fails. Verified against both 2.13 and 2.21.
