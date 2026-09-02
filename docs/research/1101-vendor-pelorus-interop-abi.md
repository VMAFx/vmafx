<!-- markdownlint-disable MD013 MD060 -->
# Research-1101: Vendoring the Pelorus interop ABI into vmafx

- **Status**: Closed (ADR-1113 Accepted)
- **Workstream**: Pelorus <-> vmafx bidirectional integration, workstream A
  (vmafx: vendor ABI)
- **Last updated**: 2026-06-14
- **Related ADR**: [ADR-1113](../adr/1113-vendor-pelorus-interop-abi.md)
- **Source plan**: `.workingdir2/rc/pelorus/PLAN.md` (gitignored planning
  artifact; this digest is the tracked summary of its verified findings).

## Question

vmafx needs the Pelorus data-plane interop ABI (a versioned per-frame side-data
blob + its pack/parse pair) in-tree to compile a reader and run a conformance
test. The ABI is owned by Pelorus (single source of truth, Pelorus ADR-0103).
How should vmafx obtain it without (a) coupling its build to Pelorus's
Vulkan/shader toolchain, (b) allowing the two repos' parsers to diverge
silently, or (c) tripping the fork's touched-file lint rule on code it must not
edit?

## Options weighed

| Option | Verdict |
| --- | --- |
| **Pinned read-only vendor + sync guard** | **Chosen.** Zero build coupling; drift is mechanically detected; the shared conformance fixture proves byte-compat. |
| git submodule of pelorus | Rejected — drags in Pelorus's full build (Vulkan/shader deps vmafx rejects); poor UX in CI / release tarballs / worktrees. |
| meson subproject (wrap) | Rejected — couples to Pelorus's meson graph + option matrix; configure-time fetch is a release-pipeline liability for a flat, dependency-free ABI. |
| Hand-reimplement the parser | Rejected — two diverging implementations of a frozen wire format guarantee an eventual interop bug; defeats single-source-of-truth. |

The vendoring posture matches the fork's existing treatment of vendored code
(`core/src/svm.cpp`, `pdjson.c`): copy verbatim, exclude from the strict lint
profile, fix defects upstream. The only local edits are a `DO NOT EDIT` banner
and a `pelorus/<x>.h` → `libvmaf/pelorus/<x>.h` include rewrite so the headers
resolve under `core/include/`.

## Verified findings (guess + check)

1. **The conformance fixture needs more than `interop.c`.** Hypothesis: vendor
   only `interop.c` + 3 headers per the plan's A1. Check: the pelorus
   `interop_test.c` links `pel_deband_params_default/validate` (deband_params.c)
   and `pelorus_version_string` (version.c) and exercises the `deband_params`
   vector (A3 requires keeping all 7 vectors). Conclusion: vendor
   `deband_params.c` + `version.c` alongside `interop.c` so the fixture stays
   byte-identical and links. All three are CPU-only, dependency-free,
   BSD+Patent — the same vendoring posture applies.

2. **The local pelorus checkout had advanced past the pin.** Hypothesis: diff
   the vendored mirror against the working tree at
   `/home/kilian/dev/pelorus`. Check: `git -C … rev-parse HEAD` returned
   `ff63ebe`, not the pinned `835e097`; the working tree had been reformatted
   (wider column limit) AND refactored (`pel_blob_pack` split into
   `validate_pack_sections` + `write_pack_header` helpers). Conclusion: the
   drift guard must read the pinned commit's **git tree object**
   (`git show 835e097:libpelorus/…`), not the working tree, so it stays
   pin-accurate regardless of where the checkout's `HEAD` sits. With this fix
   the guard reports no drift against `835e097`.

3. **A dir-local `Checks: '-*'` is the wrong lint-exclusion mechanism.**
   Hypothesis: drop a `.clang-tidy` with `Checks: '-*'` in each vendored dir
   (clang-tidy uses the nearest config). Check: `make lint-c` and the CI
   clang-tidy job pass *all changed files in a single invocation*; a
   `Checks: '-*'` file makes clang-tidy abort the whole run with
   `Error: no checks enabled`. Conclusion: use the repo's established idiom —
   `grep -v` path exclusion in the Makefile `lint-c` glob and the CI workflow's
   exclusion filter (same place `core/src/svm.cpp` and the GPU TUs are
   excluded), plus a cppcheck suppression and the `auto-format-on-edit.sh` hook
   skip. clang-format 22 was independently confirmed to reflow the vendored
   files (different version than pelorus's), so the format exclusion is load-
   bearing for byte-identity.

## Outcome

Vendored 6 files (3 headers + 3 sources) + the conformance fixture; registered
the sources in `core/src/meson.build` and the fixture in `core/test/meson.build`
(fast suite). Conformance fixture passes 7/7; the sync guard reports no drift
against `835e097`; the vendored library objects compile clean under `-pedantic`.
See ADR-1113 for the decision and [`docs/api/pelorus-interop.md`](../api/pelorus-interop.md)
for the consumer-facing reference.
