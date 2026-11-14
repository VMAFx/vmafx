## SYCL graph registration has one entry point (2026-11-14)

`fix/issue-3365-sycl-register`. `vmaf_sycl_graph_register_flags()` is the
only combined-graph registration function; do not reintroduce the flagless
forwarding shim during an upstream sync. `core/test/test_sycl_init_unwind.cpp`
wraps only `_flags`, and `core/src/sycl/AGENTS.md` keeps the combined-queue
allocation before the extractor-table increment. Live comments in
`core/src/feature/sycl/` name the flags entry point; accepted ADRs and the
changelog archive keep the historical name.
