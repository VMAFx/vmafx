- **A SYCL build with a single AOT target no longer fails the image check.**
  Configuring `-Dsycl_icpx_aot_targets=dg2-g11`, the single-target example in
  the SYCL overview, stopped at `sycl_aot_image_check` with "holds no ocloc fat
  binary" although the build was correct: with exactly one target `ocloc`
  writes each image as a bare native binary instead of a fat binary, and the
  check only knew the fat binary. It now accepts both forms and reads the GPU
  IP version of a bare binary from its product-config note, the value
  `ocloc ids` prints for the target. It still fails the build when an image
  lacks a listed target, is built for a GPU IP version that no listed target
  uses, or is neither form. Builds with two or more targets are checked as
  before.
