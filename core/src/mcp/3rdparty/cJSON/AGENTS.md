<!-- markdownlint-disable MD013 -->
# AGENTS.md — vendored cJSON

Parent: [../../AGENTS.md](../../AGENTS.md) (core/src/mcp/).

## Vendor policy

This directory holds [cJSON](https://github.com/DaveGamble/cJSON) **v1.7.19**
(MIT) **plus a fork delta**. It is *not* a verbatim copy, and a re-vendor that
drops the upstream files in unchanged is a regression, not a refresh: that is
exactly how the 1.7.18 → 1.7.19 sync (PR #883) silently reverted two security
fixes (`docs/state.md`, `T-VENDORED-CJSON-BANNED-FUNCTIONS-REVERTED-2026-09-19`).

`cJSON.h` is upstream's file unchanged. `cJSON.c` differs from upstream in
these ways, and only these:

1. **No banned libc calls** ([ADR-0683](../../../../../docs/adr/0683-cjson-banned-function-remediation.md),
   [ADR-1061](../../../../../docs/adr/1061-vendored-cjson-pdjson-depth-overflow.md);
   `docs/principles.md` §1.2 rule 30). Upstream's eleven `sprintf` / `strcpy`
   sites are `snprintf` with the real buffer size and `memcpy` with a proven
   length: `cJSON_Version`, `cJSON_SetValuestring`, `print_number` (4×, whose
   `sscanf` round-trip check is `strtod` in `number_round_trips`),
   `print_string_ptr` (the empty string, and the `\uXXXX` escape in
   `print_escape_sequence`, bounded by the space `ensure()` reserved) and the
   `null` / `false` / `true` literals in `print_literal`.
2. **`cJSON_GetArraySize` saturates at `INT_MAX`** instead of wrapping negative
   (ADR-1061, CERT INT31-C).
3. **No `goto`, no function over 60 lines** ([ADR-1142](../../../../../docs/adr/1142-whole-codebase-standards.md):
   vendored code is in scope). Upstream's jump-to-`fail` functions are split
   into helpers with the same control flow: `parse_number`, `ensure`,
   `utf16_literal_to_utf8`, `parse_string`, `print_string_ptr`,
   `cJSON_ParseWithLengthOpts`, `print`, `print_value`, `parse_array`,
   `parse_object`, `print_object`, `cJSON_Duplicate_rec` (now `static`) and
   `cJSON_Compare`. Behaviour is unchanged: on 3,103 inputs (valid, malformed,
   every-prefix truncations, seeded mutations, every allocation-failure point,
   both allocator paths) the output, the parse-error offsets and the allocation
   counts are byte-identical to pristine 1.7.19 under ASan + UBSan.
4. The `can_read` / `can_access_at_index` macro arguments are parenthesised,
   `parse_array` / `parse_object` check their buffer before dereferencing it,
   upstream's dead `object = NULL` in `cJSON_free` is gone, and the file carries
   the [ADR-1138](../../../../../docs/adr/1138-c-translation-units-keep-null.md)
   `modernize-use-nullptr` bracket. These keep clang-tidy and cppcheck at zero.

**Do not** silence any of this with `NOLINT`, a Semgrep path exclude, a
`.semgrepignore` line or a baseline entry. Fix the call site.

## Re-vendoring

1. Replace `cJSON.c`, `cJSON.h` and `LICENSE` with the upstream release.
2. Re-apply the delta above onto the new `cJSON.c`. Keep upstream's behaviour:
   build the old and the new file against the same driver and compare outputs
   before trusting a refactored function.
3. Update the version here, in `core/src/mcp/AGENTS.md`, and in
   `core/test/test_cjson.c` (`test_version` fails on purpose until you do).
4. Run the gates that now see this directory; every one failed to see it when
   the fixes were reverted:

   ```bash
   python3 -m unittest discover -s scripts/ci/tests -p test_semgrep_vendored_scope.py
   meson test -C build test_cjson
   python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build \
       --only core/src/mcp/3rdparty/cJSON/cJSON.c     # must report no warnings
   praetorctl audit                                   # the touched file must be clean
   ```

`core/test/test_cjson.c` compiles `cJSON.c` itself and is not behind
`enable_mcp`, so the CPU build, the Tidy Ratchet and Cppcheck all cover this
file even though the MCP server is an opt-in build option. Do not move that
test behind `enable_mcp`.

## Rebase note

cJSON is an internal dependency of the MCP server (`core/src/mcp/`). It does not
appear in the public C API (`core/include/`) and is not consumed by
`ffmpeg-patches/`. Upstream Netflix/vmaf does not vendor cJSON, so there is no rebase
conflict risk from the Netflix side. Conflicts can only arise if this fork adds a
second copy of cJSON elsewhere.
