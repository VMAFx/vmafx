Made local `vmaf-tune sidecar` state persistence use the shared strict JSON
writer so non-finite local corrections are persisted as `null` and rejected as
cold-start on reload instead of producing non-standard JSON tokens.
