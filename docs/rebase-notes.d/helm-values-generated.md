## Helm values and schema generated from the platform definition (2026-10-08)

`rc4/api-wp17-helm`, [ADR-2350](adr/2350-cloud-native-platform.md) D13.
`deploy/helm/vmafx/values.yaml` and `values.schema.json` are written by
`scripts/codegen/vmafx-api.py` from the `[[chart]]`, `[chart_root]` and
`[[chart_defs]]` tables of `api/vmafx-platform.toml`; the Kubernetes types of
the schema come from `api/kubernetes/openapi-subset.json`
(`scripts/codegen/k8s_openapi.py`, release and digests in `build-config.env`).
A change that edits either chart file by hand moves into the definition
instead; on a conflict in a generated file take either side and run the
generator. A new values key is a new `[[chart]]` entry at its place in the
file. `test_vmafx_api_generated_current` guards both files,
`test_k8s_openapi_subset_current` the subset. no upstream file.
