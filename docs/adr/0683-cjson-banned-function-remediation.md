<!-- markdownlint-disable MD013 MD060 -->
# ADR-0683: Replace banned functions in vendored MCP cJSON

- **Status**: Accepted
- **Date**: 2026-05-22
- **Deciders**: lusoris
- **Tags**: mcp, vendored, security, c, libvmaf, fork-local

## Context

The fork vendors a copy of cJSON (`core/src/mcp/3rdparty/cJSON/`) as an internal
dependency of the libvmaf MCP server (`core/src/mcp/`). The
`docs/principles.md` §1.2 rule 30 and the fork-wide touched-file lint rule
([ADR-0141](0141-touched-file-cleanup-rule.md)) ban `sprintf`, `strcpy`, `strcat`,
`strtok`, `atoi`, `atof`, `gets`, `rand`, and `system` in all code the fork touches,
including vendored libraries. The `feedback_vendored_in_scope` rule (see MEMORY.md)
explicitly extends this to cJSON and libsvm.

At the time of writing `cJSON.c` contains eleven call sites using banned functions:

- `sprintf` — six sites in `cJSON_Version`, `print_number` (4x), and `print_string_ptr`
- `strcpy` — five sites in `cJSON_SetValuestring`, `print_string_ptr`, and `print_value` (3x)

Two prior PRs (#890 `fix/vendored-banned-functions-2026-05-16` and #891
`fix/cjson-banned-functions-2026-05-16`) were opened and then closed without merging,
leaving the violations in tree. This ADR records the final remediation.

## Decision

Replace every `sprintf` call with `snprintf` (with the explicit buffer size), and every
`strcpy` call with `memcpy` (where the destination is statically sized or provably large
enough) or `snprintf` (where a format string is involved). Discard return values
explicitly as `(void)` only where the surrounding code already has a length-check guard
that makes the return value redundant; where the return value is used downstream
(e.g., `print_number`), keep the assignment.

Add an `AGENTS.md` in `core/src/mcp/3rdparty/cJSON/` that documents the vendor
policy, the banned-function rule, and the update procedure so future sync passes do not
re-introduce the violations.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Apply `// NOLINT` suppressions | Zero churn | Violates ADR-0278 (NOLINT requires inline ADR citation and is reserved for load-bearing invariants); `feedback_vendored_in_scope` explicitly forbids NOLINT cover for vendored bugs | Not chosen |
| Sync to a newer cJSON release | Picks up upstream fixes | cJSON upstream still uses `sprintf` in some paths; also introduces unreviewed churn beyond the banned-function scope | Not chosen as primary strategy; sync remains the recommended future path, but the banned functions must be fixed regardless |
| Remove cJSON and use `pdjson` already in tree | Reduces vendored footprint | MCP JSON-RPC response rendering requires generation (not only parsing); `pdjson` is parse-only | Not feasible without a separate MCP serializer port |

## Consequences

- **Positive**: `cJSON.c` is lint-clean under the fork's strictest profile; the
  touched-file lint rule (ADR-0141) is satisfied; banned-function CI gate passes.
- **Negative**: Minor deviation from upstream cJSON source; future `cJSON` version
  syncs must re-verify and re-apply these fixes if the upstream has not addressed them.
- **Neutral / follow-ups**: `AGENTS.md` in the vendor directory captures the update
  procedure. A future PR can optionally sync to a newer cJSON version and validate
  that the fixes remain.

## References

- `docs/principles.md` §1.2 rule 30 (banned functions list).
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file lint-clean rule.
- [ADR-0278](0278-nolint-citation-closeout.md) — NOLINT citation requirements.
- Prior closed PRs: #890 (`fix/vendored-banned-functions-2026-05-16`), #891 (`fix/cjson-banned-functions-2026-05-16`).
- Source: `feedback_vendored_in_scope` (MEMORY.md): vendored code (cJSON, libsvm, pdjson) receives the same audit and fix treatment as fork-original code; NOLINT cover is not a substitute.
