## Praetor pin c565cba50b57 (2026-10-19)

`chore/praetor-refresh-c565cba5`. A rebase or sync keeps `PRAETOR_REF` at `c565cba50b57...` in
`.github/workflows/standards-gate.yml`. The engine's HISS-01 scan flags a one-line `goto` that is
not a forward jump to the function's cleanup label: a sync that brings a backward `goto`, or one to
a label that is not a cleanup path, fixes it or records a dated exception.
