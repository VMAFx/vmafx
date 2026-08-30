Add research-0734: CUDA 13.3 non-blog findings digest covering two silent-data-corruption
fixes (thread-reconvergence pass regression since 12.8; `__mul24()` UB since 11.1),
C++23 nvcc support (unblocks ADR-0732), new managed-memory remote CPU-to-GPU mapping,
`cuStreamBeginRecaptureToGraph()` API, NPP legacy API removal, and Nsight Compute 2026.2
register-dependency profiling. Five do-now action items and four wait-for-X items
documented with verbatim quotes from the full release notes.
