- **Option tables, extractor tables and tag declarations no longer print compiler warnings.**
  The clang, gcc, icpx and Apple clang legs reported `-Wmissing-field-initializers`
  (`{NULL}` / `{0}` option terminators, positional test tables), `-Wreorder-init-list` and
  `-Wc99-designator` (the Metal option and extractor tables), `-Wmismatched-tags`
  (`VmafThreadLocaleState`, declared `struct` in the C header and `class` in the C++ file),
  `-Wimplicit-fallthrough`, `-Wkeyword-macro` (vendored cJSON redefining `true` / `false` in
  C23), `-Wtautological-constant-out-of-range-compare` (`vmaf_next_fex_capacity()` on 64-bit
  hosts) and `-Wmacro-redefined` (`DIV_ROUND_UP` in the HIP ADM twin). Every fix is
  value-preserving: initialisers are reordered or completed, `[[fallthrough]]` replaces
  comments, the capacity check compares in `size_t`. No score, option default or exported
  symbol changes.
