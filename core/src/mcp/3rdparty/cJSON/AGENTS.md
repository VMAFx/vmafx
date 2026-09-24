<!-- markdownlint-disable MD013 -->
# AGENTS.md — vendored cJSON

Parent: [../../AGENTS.md](../../AGENTS.md) (core/src/mcp/).

## Vendor policy

Dir = [cJSON](https://github.com/DaveGamble/cJSON) **v1.7.19** (MIT) **plus fork
delta**. NOT verbatim copy. Re-vendor that drops upstream files in unchanged =
regression, not refresh: exactly how 1.7.18 -> 1.7.19 sync (PR #883) silently
reverted two security fixes (`docs/state.md`,
`T-VENDORED-CJSON-BANNED-FUNCTIONS-REVERTED-2026-09-19`).

`cJSON.h` = upstream file unchanged. `cJSON.c` differs from upstream in these
ways, only these:

1. **No banned libc calls** ([ADR-0683](../../../../../docs/adr/0683-cjson-banned-function-remediation.md),
   [ADR-1061](../../../../../docs/adr/1061-vendored-cjson-pdjson-depth-overflow.md);
   `docs/principles.md` §1.2 rule 30). Upstream's eleven `sprintf` / `strcpy`
   sites = `snprintf` with real buffer size, `memcpy` with proven length:
   `cJSON_Version`, `cJSON_SetValuestring`, `print_number` (4x, whose `sscanf`
   round-trip check = `strtod` in `number_round_trips`), `print_string_ptr`
   (empty string, and `\uXXXX` escape in `print_escape_sequence`, bounded by
   space `ensure()` reserved), `null` / `false` / `true` literals in
   `print_literal`.
2. **`cJSON_GetArraySize` saturates at `INT_MAX`** instead of wrapping negative
   (ADR-1061, CERT INT31-C).
3. **No `goto`, no function over 60 lines** ([ADR-1142](../../../../../docs/adr/1142-whole-codebase-standards.md):
   vendored code in scope). Upstream's jump-to-`fail` functions split into
   helpers, same control flow: `parse_number`, `ensure`,
   `utf16_literal_to_utf8`, `parse_string`, `print_string_ptr`,
   `cJSON_ParseWithLengthOpts`, `print`, `print_value`, `parse_array`,
   `parse_object`, `print_object`, `cJSON_Duplicate_rec` (now `static`),
   `cJSON_Compare`. Behaviour unchanged: on 3,103 inputs (valid, malformed,
   every-prefix truncations, seeded mutations, every allocation-failure point,
   both allocator paths) output, parse-error offsets and allocation counts
   byte-identical to pristine 1.7.19 under ASan + UBSan.
4. `can_read` / `can_access_at_index` macro arguments parenthesised;
   `parse_array` / `parse_object` check buffer before dereferencing it;
   upstream's dead `object = NULL` in `cJSON_free` gone; file carries
   [ADR-1138](../../../../../docs/adr/1138-c-translation-units-keep-null.md)
   `modernize-use-nullptr` bracket. These keep clang-tidy and cppcheck at zero.
5. **`valueint` defined for every double.** Upstream open-codes two range checks
   in `cJSON_CreateNumber` and `cJSON_SetNumberHelper`, then casts. NaN compares
   false against both bounds -> reached `(int)number` = undefined behaviour
   (C11 6.3.1.4p1). Reachable in production: `compute_vmaf.c` hands VMAF score,
   which can be NaN, to `cJSON_AddNumberToObject`. Both sites call
   `saturate_to_int`, which maps NaN to 0. `valuedouble` keeps NaN,
   `print_number` still emits `null` -> printed output unchanged. Only clang's
   UBSan reports upstream form (`float-cast-overflow` not in gcc's `undefined`
   group), so the gcc differential harness above could not see it; `Sanitizers`
   CI lane and `test_cjson`'s `test_number_valueint_*` tests do.
6. **Compiler float model preserved without direct float equality**
   ([ADR-1308](../../../../../docs/adr/1308-codeql-float-equality-contracts.md)).
   `print_number()` evaluates `d - (double)item->valueint == 0.0` rather than
   `d == (double)item->valueint`. This eliminates CodeQL `cpp/equality-on-floats`
   alert 1221 while preserving compiler float-model semantics (including
   Denormals-Are-Zero / DAZ under Intel icx `-fp-model=fast` vs subnormal
   preservation on GCC/Clang).

**Denormals follow build's floating-point model.** `print_number()` takes
integer branch when `d - (double)valueint == 0.0` (ADR-1308; CodeQL clean,
preserving `d == (double)valueint` model). Build treating denormals as zero
answers true for smallest denormal, prints `0` = what that build's arithmetic
says value is. icx defaults to that model (`-fp-model=fast` sets MXCSR
denormals-are-zero bit); `Linux Intel LLVM` lane builds this file with it. gcc
and clang keep denormal. `test_print_number_precision` probes running build
instead of asserting one answer. Do not pin one spelling into test. Do not
"fix" printer: both outputs = what their build's own arithmetic says.

**Do not** silence any of this with `NOLINT`, Semgrep path exclude,
`.semgrepignore` line or baseline entry. Fix call site.

## Re-vendoring

1. Replace `cJSON.c`, `cJSON.h`, `LICENSE` with upstream release.
2. Re-apply delta above onto new `cJSON.c`. Keep upstream behaviour: build old
   and new file against same driver, compare outputs before trusting a
   refactored function.
3. Update version here, in `core/src/mcp/AGENTS.md`, in `core/test/test_cjson.c`
   (`test_version` fails on purpose until you do).
4. Run gates that now see this directory; every one failed to see it when the
   fixes were reverted:

   ```bash
   python3 -m unittest discover -s scripts/ci/tests -p test_semgrep_vendored_scope.py
   meson test -C build test_cjson
   python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build \
       --only core/src/mcp/3rdparty/cJSON/cJSON.c     # must report no warnings
   praetorctl audit                                   # the touched file must be clean
   ```

`core/test/test_cjson.c` compiles `cJSON.c` itself, not behind `enable_mcp` ->
CPU build, Tidy Ratchet and Cppcheck all cover this file even though MCP server
= opt-in build option. Do not move that test behind `enable_mcp`.

## Rebase note

cJSON = internal dependency of MCP server (`core/src/mcp/`). Not in public C
API (`core/include/`); not consumed by `ffmpeg-patches/`. Upstream Netflix/vmaf
does not vendor cJSON -> no rebase conflict risk from Netflix side. Conflict
risk only if fork adds second cJSON copy elsewhere.
