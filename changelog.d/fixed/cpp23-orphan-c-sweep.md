Remove orphan `core/src/metadata_handler.c` left behind after the cpp23
Wave 1-5 conversions renamed the file in meson.build to
`metadata_handler.cpp` (ADR-0708) but did not `git rm` the original `.c`.
Meson compiles only the `.cpp`; the `.c` was dead source.

**Reproducer (inventory check):**
```
find core/src -name '*.c' -not -path '*/vulkan/*' | while read f; do
  cpp="${f%.c}.cpp"
  [ -f "$cpp" ] || continue
  base=$(basename "$f" .c)
  c_ref=$(grep -l "'${base}\.c'" core/src/meson.build core/src/feature/meson.build 2>/dev/null)
  cpp_ref=$(grep -l "'${base}\.cpp'" core/src/meson.build core/src/feature/meson.build \
            core/test/meson.build 2>/dev/null)
  [ -z "$c_ref" ] && [ -n "$cpp_ref" ] && echo "TRUE ORPHAN: $f"
done
```
