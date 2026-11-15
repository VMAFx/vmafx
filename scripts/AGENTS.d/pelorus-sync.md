---
paths:
  - scripts/sync-pelorus-interop.sh
invariant: Pin 40-char commit via git show; fail closed without object; manifest + test together.
---
<!-- markdownlint-disable MD013 MD038 -->
# Pelorus interop mirror guard (ADR-1113, ADR-1276)

## Rebase-sensitive invariants

### `sync-pelorus-interop.sh` reads one exact source object (ADR-1113, ADR-1276)

Pelorus mirror guard pins full 40-character released commit and reads
every source with `git show`. non-Git source directory or checkout missing
that object fails closed; never restore fallback to working-tree bytes. Re-pin
for reviewed ABI additions and released parser correctness/security fixes,
even when ABI 1.3 is unchanged. conformance fixture is canonical
VMAFx-authored prefix (including pin and ABI version) plus transformed
Pelorus body; complete file is byte-sensitive through EOF. guard also
requires every tracked path in lint-exempt Pelorus namespaces to appear in its
manifest. Keep lint/format policy outside shared body, and keep guard,
its hermetic test, and required Pre-Commit workflow checkout in one change.
