<!-- markdownlint-disable MD013 MD060 -->
# ADR-0137: Thread-local locale handling for numeric I/O

- **Status**: Accepted
- **Date**: 2026-04-20
- **Deciders**: Lusoris, Claude
- **Tags**: port, libvmaf, i18n, thread-safety, upstream-port

## Context

libvmaf reads and writes floating-point numbers in several hot paths:
`vmaf_write_output_{xml,json,csv,sub}`, `svm_save_model`, the SVM model
parsers, and `read_json_model.c`. All of these assume the C locale
convention — period as decimal separator — because the output formats
(XML/JSON/CSV) and on-disk SVM model files must round-trip across
systems regardless of the user's regional settings.

If the host thread is in a locale that uses comma as the decimal
separator (`de_DE`, `fr_FR`, `it_IT`, `es_ES`, ...), `printf("%.6f",
1.5)` produces `"1,500000"` instead of `"1.500000"`, and `scanf("%lf",
...)` rejects `"1.5"`. This silently corrupts saved SVM models and
produces non-conforming JSON/CSV output.

The classic mitigation — `setlocale(LC_ALL, "C")` bracketed around the
hot call — is process-global and therefore racy in a multi-threaded
host (another thread's `strftime` or `scanf` sees the switched locale
mid-call). libvmaf is increasingly embedded in multi-threaded hosts
(ffmpeg filter graphs, MCP server worker pools, tiny-AI training
scripts), so the process-global fix is unacceptable.

Upstream Netflix/vmaf PR [#1430][pr-1430] (Diego Nieto, Fluendo) adds
a thin cross-platform abstraction — `thread_locale.h/.c` — that
switches only the calling thread's locale. The fork ports this
PR to pick up the fix before upstream merges it.

## Decision

We will port upstream PR #1430 as-is into the fork, with three fork
corrections:

1. **API-shape adaptation in `test_locale_handling.c`** — fork's
   `vmaf_write_output_{xml,json,csv}` take a trailing `score_format`
   parameter (from ADR-0119); pass `NULL` (library default) in all
   three test calls.
2. **Merge conflict resolution in `core/src/output.c`** — keep fork's
   `return ferror(outfile) ? -EIO : 0;` contract (ADR-0119) on top of
   upstream's `vmaf_thread_locale_pop(locale_state);` cleanup; keep
   fork's `const char *sf = fmt_or_default(score_format);` locals and
   drop upstream's unused `int leading_zeros_count;` stragglers.
3. **Merge conflict resolution in `core/src/svm.cpp`** — replace
   the fork's previous `setlocale()/strdup/setlocale()` bracket (now
   demonstrably racy under the multi-session symptom Lawrence reported
   on Discord) with `vmaf_thread_locale_push_c/pop`; keep fork's K&R
   brace style + 4-space indent when folding in upstream's
   `buffer.imbue(std::locale::classic())` calls on the two SVM parser
   sources; drop the now-dead `<locale.h>` include.

`thread_locale.c/h` and `test_locale_handling.c` keep Diego Nieto's
Netflix-style copyright header verbatim (upstream-authored, ADR-0025
rule 1). No new fork-specific code is introduced in those files
beyond the build-system plumbing.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Port upstream #1430 with minimal corrections (chosen) | Gets the thread-safe fix today; stays close to upstream so the eventual merge is a no-op; covers Windows + macOS + Linux. | Adds a 113-line platform-split file to the fork surface area. | Picked: the multi-session locale race is actively biting users (see Lawrence's multi-VMAF ffmpeg pipeline), upstream merge is uncertain, and the abstraction is small and platform-isolated. |
| Wait for upstream to merge #1430 | Zero fork divergence. | PR #1430 has been open since 2025-07 with no maintainer review; waiting indefinitely leaves users on a racy `setlocale` bracket. | Rejected: deliverability > divergence, consistent with the Batch-A "port OPEN upstream PRs now with correction-during-port" policy. |
| Roll our own `uselocale`-only implementation (drop Windows path) | Smaller surface; POSIX-only is enough for the golden gate hosts. | Breaks CI on the `windows-latest` matrix leg (ADR-0121) and the MCP server's Windows packaging. | Rejected: the fork ships Windows binaries; we need the `_configthreadlocale` branch. |
| Inline the thread-locale calls directly in each writer | No new file. | Duplicated platform `#ifdef` cascade across 5+ call sites; miserable to maintain. | Rejected: standard DRY violation. |

## Consequences

- **Positive**:
  - `vmaf_write_output_{xml,json,csv,sub}` and `svm_save_model` are
    thread-safe under any host locale; multi-threaded hosts no longer
    race on `setlocale`.
  - JSON model files and CSV/JSON output are guaranteed
    period-decimal regardless of the host's `LANG`/`LC_*` env vars —
    cross-machine model file portability is preserved.
  - The fork sheds the `setlocale(LC_ALL, NULL) + strdup +
    setlocale(LC_ALL, "C")` bracket in `svm.cpp`, which was
    process-global and thus broken under multi-threaded use.
- **Negative**:
  - Adds a 113-line platform-split `thread_locale.c` to the fork
    surface area. Windows code path goes through a shared 256-byte
    buffer per call — a `strncpy` hot-path cost per writer
    invocation, but writer invocations are one-per-frame-output so
    the cost is negligible.
  - Fallback path (platforms without `uselocale` and without
    `_configthreadlocale`) returns `NULL` from `push_c()`, which
    writers then no-op on in `pop`. On such platforms callers lose
    the fix — no regression vs. today but also no improvement.
- **Neutral / follow-ups**:
  - Meson now probes `HAVE_USELOCALE` + `HAVE_XLOCALE_H` at configure
    time (ADR-0027 style feature detection); `config.h` gets two new
    symbols.
  - Added `test_locale_handling` to the default test suite. Test
    skips gracefully when `it_IT.UTF-8`/`fr_FR.UTF-8`/`es_ES.UTF-8`
    locales are not present on the runner.
  - When upstream eventually merges #1430, the next `/sync-upstream`
    will see this commit as already-present — the SHA-1 match from
    the cherry-pick `(cherry picked from commit …)` trailer will let
    `git rerere` or a no-op merge handle it.

## References

- Upstream PR: [Netflix/vmaf#1430][pr-1430] — Diego Nieto / Fluendo.
- Upstream cherry-picked SHA: `054a97edc3b4409df84e0ad9630f27673ca18da6`.
- Related: [ADR-0119](0119-cli-precision-default-revert.md) — `score_format` API shape that drives the test-call correction.
- Related: [ADR-0025](0025-copyright-header-policy.md) / [ADR-0105](0105-copyright-header-enforcement.md) — copyright handling for upstream-authored files.
- Related: [ADR-0121](0121-windows-gpu-build-only-legs.md) — Windows CI matrix that requires the `_configthreadlocale` branch.
- Lusoris ↔ Lawrence Discord, 2026-04-20: multi-VMAF-in-one-ffmpeg
  failure mode on CUDA hosts, confirming the multi-session locale
  race as user-visible.
- Source: `req` (direct user direction: "Start T4-3 (Netflix#1430 thread-local locale port)" via AskUserQuestion popup).

[pr-1430]: https://github.com/Netflix/vmaf/pull/1430
