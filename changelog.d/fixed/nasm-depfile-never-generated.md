- **Editing a `.asm` header left a stale object behind.** The meson
  nasm generator declared `depfile: '@BASENAME@.obj.ndep'` and passed
  `-MQ @OUTPUT@ -MF @DEPFILE@`, but `-MF` only *names* the dependency
  file — `-MD` is what turns dependency generation on. Without it nasm
  assembled the source and wrote no depfile at all, so ninja tracked
  `cpuid.asm` alone and neither `core/src/ext/x86/x86inc.asm` nor the
  generated `config.asm` was a dependency of `cpuid.obj`. Editing
  either one silently kept the previously assembled object, which is
  how a CPU-feature-detection change can appear to have no effect.
  The arguments are now `-MD @DEPFILE@ -MQ @OUTPUT@` (`-MD` takes the
  filename directly, so `-MF` is dropped rather than kept alongside
  it). Verified: `ninja -t deps` now lists all three inputs, and
  `touch core/src/ext/x86/x86inc.asm && ninja -C build` reassembles
  `cpuid.obj` where it previously reported no work to do.
