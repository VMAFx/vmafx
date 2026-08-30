## fix(vendored): pdjson depth limit + integer overflow + cJSON banned functions (ADR-1061)

- **pdjson**: define `PDJSON_STACK_MAX 512` unconditionally — the previous
  `#ifdef` guard was never activated, allowing unbounded heap growth on
  deeply-nested JSON inputs.
- **pdjson `push()`**: add explicit addition-overflow and multiply-overflow
  guards before the `realloc` size calculation (CERT-C INT30-C).
- **pdjson `pushchar()`**: guard the `* 2` string-buffer doubling against
  `size_t` wrap when `string_size >= SIZE_MAX / 2` (CERT-C INT30-C).
- **cJSON**: complete ADR-0683 remediation — replace all 12 banned-function
  call sites (`sprintf` → `snprintf`, `strcpy` → `memcpy`) that prior PRs
  #890 and #891 left unmerged.
- **cJSON `cJSON_GetArraySize`**: clamp the `size_t`-to-`int` cast to
  `INT_MAX` instead of wrapping (CERT-C INT31-C); addresses upstream FIXME.
