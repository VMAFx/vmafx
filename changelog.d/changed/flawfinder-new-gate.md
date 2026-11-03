- **A pre-commit hook refuses new flawfinder findings in changed C and C++
  files.** `scripts/ci/check_flawfinder_new.py` runs flawfinder 2.0.20, the
  engine Codacy runs on every pull request, and compares each changed file with
  its merge-base copy, so a finding is caught before the push instead of after
  a CI round. The C API harness of the `pkg/libvmaf` bit-exactness tests now
  reads its YUV pair from descriptors 3 and 4 that the Go test opens, instead of
  opening paths itself.
- The `flawfinder-new` gate scans every C, C++, CUDA and Objective-C++
  suffix pre-commit selects for it (module interfaces such as `.ccm` and
  `.cppm`, headers such as `.hh` and `.cuh`), and the Tooling Tests lock
  installs flawfinder 2.0.20 so the gate's own tests run in that job.
