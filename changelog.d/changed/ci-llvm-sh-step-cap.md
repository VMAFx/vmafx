<!-- markdownlint-disable MD041 -->
- **CI steps that install LLVM through `llvm.sh` now have a 10-minute limit.**
  The 5-minute limit set with the apt archive cache cut off a `Tidy Ratchet` run on `master` (5.2 minutes); the slowest healthy run of such a step measured 5.7 minutes.
  The jobs holding these steps keep their headroom: `Tidy Changed`, `Tidy Ratchet`, `Tidy SYCL`, `fuzz` and the three sanitizer jobs gain 5 minutes each.
