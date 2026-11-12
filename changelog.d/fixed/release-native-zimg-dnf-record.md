- **The native release credits the libzimg it ships.** The `release-native` zimg entry of
  `tools/rc1-tester/image/licensing.json` described upstream zimg 3.0.6 built from the
  `release-3.0.6` tag, but `stage_zimg` actually copies the loader-resolved `libzimg.so*` from the
  `release-build` container's unpinned `dnf` `zimg-devel` (issue #3396). The entry now names the
  installed package the way the Debian production records do: the EPEL 8 source package `zimg
  3.0.6-1.el8` that `dnf zimg-devel` pulls into the manylinux_2_28 base, measured against the EPEL
  8 repository metadata, so the release's `THIRD_PARTY_NOTICES.txt` names the distribution library
  the bundle carries.