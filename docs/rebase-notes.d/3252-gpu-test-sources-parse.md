## GPU test sources parse under Semgrep (2026-11-08)

no rebase impact: the change touches fork-added GPU test sources,
`core/test/test_sycl_sub_group_size_contract.py` and the fork-only
`core/src/feature/sycl/sycl_compat.h`; no file shared with Netflix/vmaf
changes. Keep the parser-readable spellings listed in
`docs/research/3402-semgrep-gpu-test-parse.md` when editing these
files, or the float-identity rule stops reading them.
