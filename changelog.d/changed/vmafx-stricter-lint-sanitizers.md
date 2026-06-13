### CI — tighter clang-tidy + required sanitizer gates (ADR-0694)

### clang-tidy

Two CERT-C checks are now explicitly tracked as Phase 2B / ADR-0686
enforcement additions in `.clang-tidy`:

- **`cert-err33-c`** (advisory): every non-void return value must be checked
  or explicitly `(void)`-cast. Already mandated by CLAUDE.md §6; now surfaces
  in CI lint reports. A follow-up sweep PR will fix 394 pre-existing
  violations and promote the check to `WarningsAsErrors`.
- **`bugprone-not-null-terminated-result`** (advisory): catches
  `strncpy`/`strlcpy` results that may not be null-terminated. Zero
  pre-existing violations; included for completeness.

Both checks were already reachable via the `bugprone-*` / `cert-*` globs;
explicit listing documents intent and enables targeted promotion.

### Sanitizers

Confirmed that the three sanitizer matrix jobs —
`Sanitizers — ASan + UBSan + MSan (address/undefined/thread)` — already run on
every non-draft PR without `continue-on-error`, and are listed in the Required
Checks Aggregator (ADR-0313). This ADR closes the audit gap and documents the
intentional gate semantics. No workflow changes were required.

**Follow-up required**: `T-CERT-ERR33-SWEEP` — fix 394 `cert-err33-c` violations
and promote the check to `WarningsAsErrors`.
