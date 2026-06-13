- **C standard bumped to C23** (`-std=c23`). The minimum compiler versions required
  by the build matrix (GCC 13+, Clang 16+) all support C23; the dev container ships
  GCC 16 and Clang 22. C23 language features (`typeof`, `ckd_*`, `[[fallthrough]]`,
  `#embed`, `nullptr`) are now available in all libvmaf C translation units but are
  not yet used in source — adoption comes in follow-up PRs per the VMAFX rebrand
  roadmap (ADR-0692, umbrella ADR-0686). `-Wimplicit-fallthrough` is now enforced
  at compile time, complementing the JPL Rule-24 fallthrough annotation requirement.
  One latent C23 incompatibility was fixed in `libvmaf/test/test_propagate_metadata.c`:
  the `set_meta` stub callback now carries the correct `(void *, VmafMetadata *)`
  parameter list matching the `VmafMetadataConfiguration.callback` type.
