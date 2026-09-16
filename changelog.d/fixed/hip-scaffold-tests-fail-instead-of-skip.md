- **The HIP parity tests reported 15 failures on a healthy checkout.**
  Most HIP feature extractors are documented scaffolds: their HSACO
  kernel blobs are not built yet, so `init()` returns `-ENOSYS` and the
  extractor falls through to the CPU implementation
  (`core/src/feature/hip/hip_hsaco_stubs.c`, ADR-0533/0536/0539). The
  parity tests already skip when no HIP device is present, but they had
  no branch for the scaffold contract, so a not-yet-ported extractor
  read as a test failure. On a machine with a working AMD device that
  is 15 red tests out of 40 — which trains people to ignore the HIP
  suite, and it hid the four genuine failures underneath.
  The tests now skip on `-ENOSYS` with
  `[skip: HIP extractor is a scaffold (-ENOSYS)]`, exactly as they skip
  on "no device". **Any other error still fails**, so a real regression
  is still caught. HIP goes from 173 passed / 15 failed to 184 passed /
  4 failed / 4 skipped.
  The remaining four are tracked rather than papered over. They return
  `-EINVAL` where the contract says `-ENOSYS`, but the environment they
  were observed in cannot distinguish a code defect from a toolchain
  gap: meson reports `Run-time dependency hip-lang found: NO`, so no
  `.hip` source is compiled into an HSACO blob on that machine at all.
  Diagnosing them needs a host with a complete ROCm toolchain.
