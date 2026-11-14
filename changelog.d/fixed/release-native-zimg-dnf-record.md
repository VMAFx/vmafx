- **The native release credits the libzimg it ships, and refuses one it cannot name.** The
  `release-native` zimg entry of `tools/rc1-tester/image/licensing.json` described upstream zimg
  3.0.6 built from the `release-3.0.6` tag, but `stage_zimg` actually copies the loader-resolved
  `libzimg.so*` from the `release-build` container's unpinned `dnf` `zimg-devel` (issue #3396).
  The entry now names the installed package the way the Debian production records do: the EPEL 8
  source package `zimg 3.0.6-1.el8` that `dnf zimg-devel` pulls into the manylinux_2_28 base,
  measured against the EPEL 8 repository metadata. A record alone would go stale the next time
  EPEL rebuilds, so `stage_zimg` reads `rpm -qf --qf '%{SOURCERPM}'` for the resolved library,
  compares it with the one expected source package in
  `tools/rc1-tester/image/zimg-source-rpm.txt`, and fails the build when the two differ; the
  record test reads the same file. A rebuild becomes a red build instead of a wrong
  `THIRD_PARTY_NOTICES.txt`.
