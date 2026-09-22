- HISS-21 burn-down for the Python harness, the MCP server, the CI/dev
  scripts and the MATLAB MEX sources: 60 baselined invariant violations
  (59 HISS-04 oversized functions, 5 HISS-02 unbounded `while True`
  loops) are discharged by extraction, not by suppression. No baseline
  file, `# noqa`, `NOLINT` or ignore-list entry was touched, and no
  package, CLI flag, tool or public entry point was removed.
  - `compat/python-vmaf/`: the `VmafFeatureExtractor` /
    `VmafIntegerFeatureExtractor` option chains become lookup tables,
    the Krasula AUC and resolving-power routines in `perf_metric.py`
    split into per-stage helpers, `quality_runner.py` grows an
    `_optional()` accessor plus `_resolve_vmafexec_options()`,
    `result.py` shares one `_ordered_score_lists()` between `to_xml()`
    and `to_dict()`, and `routine.read_dataset()` resolves each asset
    field through a named helper. `scanf.py` and the PyPSNR frame loop
    now carry real loop conditions (the YUV loop is bounded by
    `YuvReader.num_frms`, which the reader already validates).
  - `mcp-server/vmaf-mcp/`: `_list_tools()` is assembled from per-tool
    declarations and `_scoring_extra_properties()` from per-section
    helpers; both emit a byte-identical catalogue, as the ADR-1117
    Go↔Python parity contract requires.
  - `compat/python-vmaf/matlab/`: the matlabPyrTools and STMAD MEX
    sources keep every index expression; the nine-section convolution
    bands, the `Extend()` reduce/expand halves and the mexFunction
    argument parsers move into `static` helpers, and the banned
    `strcpy()` default-edge copy becomes a bounded one.
