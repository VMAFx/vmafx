<!-- markdownlint-disable MD013 -->
# Research-0685: Modernization Audit Contract-Noise Follow-Up

## Question

How should the modernization audit keep finding real implementation gaps after
the latest backlog refresh without putting documented optional-runtime
contracts at the top of the queue?

## Evidence

- Fresh local run:
  `.workingdir2/modernization/audit-20260521.{md,json}`.
- The report produced 1242 total findings, 901 actionable rows, and 341
  blocked/deferred rows.
- The top actionable rows were dominated by documented optional-backend
  `-ENOSYS` prose, test-double stub prose, ABI-pinning disabled-build stub
  comments, Python type-stub package names, driver-stub diagnostics, and ADR
  allocator `.md.stub` reservation wording.

## Findings

- `HAVE_*` / `enable_*=false` optional-backend text is the same class of
  contract ADR-0659 already filters for DNN disabled-build paths.
- Unit-test stub prose is not a production scaffold. It should not outrank real
  `raise NotImplementedError(...)` or bare `return -ENOSYS;` rows.
- Type-stub packages, driver-stub diagnostics, and disabled-build ABI stub
  comments are not production scaffolds.
- ADR allocator `.md.stub` references are reservation mechanics, not unfinished
  documentation.
- HIP/ROCm source files use dual-path contracts: `enable_hipcc=true` compiles
  real kernels, while `enable_hipcc=false` intentionally keeps `-ENOSYS`
  fallback branches. Those branches must not be ranked as "HIP not
  implemented."
- Runtime error translators that map native "not supported" codes to POSIX
  `-ENOSYS` are not missing implementations.

## Decision

Extend the existing ADR-0659 context filters rather than adding file-level
allowlists. The audit remains read-only and still reports bare `return
-ENOSYS;` rows outside a recognized contract.

## Validation

```bash
.venv/bin/python -m pytest scripts/dev/test_project_modernization_audit.py -q
.venv/bin/python scripts/dev/project_modernization_audit.py \
  --out-json .workingdir2/modernization/audit-20260521-filtered.json \
  --out-md .workingdir2/modernization/audit-20260521-filtered.md \
  --max-findings 80
```

## References

- [ADR-0658](../adr/0658-project-modernization-audit.md)
- [ADR-0659](../adr/0659-modernization-audit-false-positive-filter.md)
- `req`: "and? we should have multiple backlogs now?? or where are the results of all the audits i wanted"
