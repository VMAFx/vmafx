- The native Linux release bundle (`vmaf` and `libvmaf.so*`) now runs on
  Ubuntu 24.04, Debian 13 and newer distributions: it needs glibc 2.38 and the
  libstdc++ of GCC 12 instead of glibc 2.43. It is compiled on the fork's
  Debian 13 release track, the base of the published container images, and
  each release checks it on Ubuntu 24.04 and in the distroless `cc-debian13`
  runtime image. Ubuntu 22.04 and Debian 12 remain unsupported (ADR-1354).
