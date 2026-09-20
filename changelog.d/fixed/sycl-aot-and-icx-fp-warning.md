# Changelog fragment

- **SYCL AOT images now reach compile-only fat objects.** Intel GPU device
  selection is scoped to the `spir64_gen` backend and icpx emits final AOT
  images instead of silently retaining relocatable device bitcode. Intel C
  scalar-reference libraries also use one strict floating-point model, removing
  conflicting compiler options while preserving disabled FMA contraction.
