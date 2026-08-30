**fix(threading)**: Harden `VmafFeatureExtractor.prev_ref` thread-safety in
batch-threading and pool dispatch paths. Rename the shared-extractor pointer in
`threaded_extract_batch_func` to `const shared_fex`, add an `assert` that the
per-thread deep-copy is a distinct heap object, and add ADR-0795 citations at
both write sites. No semantic change — the race did not exist in the current
code; this makes the invariant machine-checked and self-documenting.
