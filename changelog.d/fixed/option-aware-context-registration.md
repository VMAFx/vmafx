- Preserve separately configured feature extractors when their option-derived
  output keys differ, including motion variants requested by multiple models.
  Equivalent defaults, aliases and CPU/GPU twins still share a registration.
  Registration allocation failures preserve existing contexts, and vector
  growth checks both count and byte-size limits in release builds.
