`core/src/feature/feature_extractor.c` renamed to `.cpp` (ADR-0772). Six
`void *` cast fixups, one `atomic_load` arithmetic guard, and `extern "C"`
guards added to `feature_extractor.h`. No public ABI change.
