# Research-0659: Modernization audit false-positive filter

## Question

How should the project-modernization audit keep finding real backlog without
ranking historical closeout text as fresh implementation debt?

## Sources Reviewed

- `scripts/dev/project_modernization_audit.py`
- `scripts/dev/test_project_modernization_audit.py`
- `docs/development/project-modernization-audit.md`
- `.workingdir2/modernization/audit-20260520.md`
- ADR-0658 project-modernization audit design

## Findings

- The initial scanner correctly caught live `raise NotImplementedError(...)`
  rows, but it also matched prose that said a scaffold was already replaced.
- Python exception handlers such as `except NotImplementedError as exc` and
  custom exception subclasses are normal control-flow or API shape, not missing
  implementations.
- `-ENOSYS` often marks an intentional optional-build contract. API docs,
  workflow comments, and DNN fallback stubs that explain the contract should not
  appear ahead of real implementation work.
- File-level suppressions would make the report quieter, but they would also
  hide new real debt in files that currently contain only historical wording.

## Decision Support

The useful filter boundary is local line context:

- keep live `raise NotImplementedError(...)`;
- drop historical wording such as "replaced", "previously", or "no longer";
- drop Python handler / custom-exception mentions.
- drop documented `-ENOSYS` disabled-build contracts while keeping bare
  `return -ENOSYS;` rows outside that context.

This preserves the audit's role as a queue-shaping tool while avoiding stale
QAT, Phase-B, and documented disabled-build rows.

## Validation

```bash
.venv/bin/python -m pytest scripts/dev/test_project_modernization_audit.py -q
python3 scripts/dev/project_modernization_audit.py \
  --out-md .workingdir2/modernization/audit-20260520-filtered.md
```
