- **Integer-ADM contrast-masking row rounding is now guarded before score
  conversion can hide placement errors.** A private raw-accumulator seam plus
  an all-backend source contract pins the once-per-complete-row shift across
  the scalar CPU reference, AVX2/AVX-512, and the CUDA, HIP, SYCL and Metal
  twins. Device-free fast tests distinguish correct row rounding from
  per-partition rounding, truncation and a post-shift increment across every
  reduction path. Production arithmetic and public interfaces are unchanged.
