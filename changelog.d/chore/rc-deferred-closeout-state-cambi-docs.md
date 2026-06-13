Close T-DOC-LEGACY-RUNNER-MISSING-DEPRECATION state.md drift; remove stale Vulkan reference from cambi docs.

`docs/state.md`: Move T-DOC-LEGACY-RUNNER-MISSING-DEPRECATION from Open to
Recently Closed — the deprecation entry was already present in
`docs/development/deprecations.md` since PR #852 but the state row was not
updated, leaving a false-positive Open entry.

`docs/metrics/cambi.md`: Remove stale Vulkan GPU section and build instructions
that referenced the dropped Vulkan backend (ADR-0726). The GPU support section
now documents only the CUDA backend.
