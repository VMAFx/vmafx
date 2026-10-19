- **The Go binaries' environment, its documentation and the chart's
  `VMAFX_*` entries are generated from one definition
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).** The `[[config]]`
  entries of `api/vmafx-platform.toml` list every environment variable
  `vmafx-controller`, `vmafx-server`, `vmafx-node`, `vmafx-operator`,
  `vmafx-mcp` and `vmafx-tune` read, the framework's `VMAFX_LOG_*` and
  `VMAFX_OTEL_*` keys included. They generate each binary's golusoris
  CompoundKeys, one environment table per binary (on its page and in
  `docs/usage/env-vars.md`, with the chart values that set each variable),
  and `deploy/helm/vmafx/templates/_config.gen.tpl`, which writes every
  `VMAFX_*` entry of every chart workload, conditions and Secret references
  included. `helm template` output is unchanged for the CI, end-to-end and
  documented values sets; topic pages link to the generated tables instead of
  repeating rows. `controllerclient.CompoundKeys` is deprecated: the binaries
  no longer read it.
