- **Pinned governance engine advanced to the structural native scanner
  (`f41e74d8f`).** The previous engine's brace tracker counted `extern "C" {`
  linkage blocks and file-scope initialisers as function bodies, so HISS-04
  reported 101 "functions" that are not functions — including
  `core/include/libvmaf/libvmaf.h` as a single 643-LOC function, which is
  unsatisfiable by construction. Because `praetorctl audit` also requires a
  touched file to be fully clean, those phantom findings made six CUDA
  translation units uncommittable, stranding 49 real `goto` removals behind a
  scanner artefact. The new engine classifies declarations structurally while
  still recognising split-signature and Allman-style functions, and measures a
  function's length from its opening brace rather than its signature, so a
  wrapped signature no longer inflates the count. Measured on this tree the
  total moves 1063 → 959, and `.standards-baseline.json` is re-recorded from a
  clean clone to match (a decrease, so no `--allow-increase`). Fingerprints are
  keyed `file:line`, which is why unchanged findings re-fingerprint across the
  bump. The engine also raises the ADR verifier's record ceiling from 512 to
  4096, so `praetorctl adr verify` passes on this repository's 999 decision
  records for the first time, and it adds a managed README governance block
  whose recorded-infraction count tracks the baseline.
