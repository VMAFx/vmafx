## Go binaries' environment and the chart's VMAFX_* entries generated (2026-10-08)

`rc4/api-wp17-config`, [ADR-2350](adr/2350-cloud-native-platform.md) D13.
`[[config_binaries]]`, `[[config]]`, `[[chart_workloads]]`, `[[chart_env]]`
and `[[chart_maps]]` of `api/vmafx-platform.toml` generate each binary's
`config_keys.gen.go` (its golusoris CompoundKeys), the environment tables of
the binaries' pages and of `docs/usage/env-vars.md` (between `BEGIN/END
GENERATED` markers), and `deploy/helm/vmafx/templates/_config.gen.tpl`. A
template includes `vmafx.env.<workload>` where its environment list holds the
`VMAFX_*` entries; `_helpers.tpl` keeps no environment mapping. A change that
adds a variable, edits a CompoundKeys list, an environment table row or a
`VMAFX_*` entry of a template by hand moves into the definition instead; on a
conflict in a generated file or region take either side and run
`scripts/codegen/vmafx-api.py --write`. `test_vmafx_api_generated_current`
guards the generated files and regions, `scripts/ci/tests/test_helm_config_env.py`
that no other template writes a `VMAFX_*` entry, and
`cmd/vmafx-controller/env_test.go` that the controller's `grpc.*` variables
reach their keys. no upstream file.
