**vmaf-tune compare/report `--format both` now writes the JSON artefact.**
Previously, `--format both` wrote only `.html` and `.md`; the `.json` file
was silently dropped. Both emission sites are fixed: `_write_compare_profile_report`
(compare subcommand) and `_run_report` (report subcommand).

**vmaf-tune ladder `--encoder libsvtav1` no longer exits 2 on the default CRF sweep.**
`DEFAULT_SAMPLER_CRF_SWEEP` started at CRF 18, below libsvtav1's Phase A lower
bound of 20. The sweep is updated to `(20, 25, 30, 35, 40)` — uniform step-5
spacing, starting at the highest lower bound across all supported adapters.
