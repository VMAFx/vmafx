- **CI: the 32-bit x86 (`Ubuntu i686 gcc`) lane is removed again.** ADR-0728
  retired 32-bit x86 in May, but a merge the same day brought the lane back,
  and it kept running compile-only with no tests. The fork stays 64-bit only
  (ADR-1258). `scripts/dev/preflight.sh` drops its `m32` stage, which only
  mirrored that lane.
