- **The production images credit the libzimg they ship.** The `vmaf` command now links zimg by
  default ([ADR-3396](../docs/adr/3396-zimg-on-by-default.md)), so every production image copies
  `libzimg.so*` out of Debian's `libzimg2` package — but the `zimg` entry of
  `tools/rc1-tester/image/licensing.json` still described upstream zimg 3.0.6 and claimed no file in
  the image, so the six builders that copy that library had no Debian record for it (issue #3242).
  The `zimg` entry of `production-cli-image` now reads the package's Debian record exactly as the
  tester entry already does: each builder stages `usr/share/doc/libzimg2/copyright` plus a
  `dpkg-query` `SOURCE` line, the runtime stages install both at `/usr/share/doc/libzimg2`, and the
  eight production records that inherit the component follow. The image's source offer names
  `libzimg2`'s Debian source package instead of upstream zimg 3.0.6, and
  `tools/rc1-tester/tests/test_dockerfile_runtime_libs.py` holds all six builders and the nine
  published runtime stages to it.
