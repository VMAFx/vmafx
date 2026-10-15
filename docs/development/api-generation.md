# API generation

One definition, `core/api/vmafx.toml`, describes the VMAFx C API, and a
generator writes every surface from it
([ADR-1852](../adr/1852-vmafx-api-redesign.md)): the public headers, the
linker version script and export lists, the ABI layout test, the libvmaf
compatibility shims, the Python binding and the reference pages. The generated
files are committed; a test fails when any of them differs from what the
definition produces, so a hand edit of a generated file fails the build's
tests.

A second definition, `api/vmafx-platform.toml`, describes the gRPC services and
messages of the controller and the scoring server, and the Kubernetes custom
resources of the platform
([ADR-2350](../adr/2350-cloud-native-platform.md) D13); the same generator
writes their protobuf files, Go types and the Helm chart's values and schema
([Platform definition](#platform-definition),
[Kubernetes resources](#kubernetes-resources),
[Helm chart values](#helm-chart-values)).

## Change the API

1. Edit `core/api/vmafx.toml`. A new entry names its header (`header`) and the
   ABI minor that introduces it (`since`); raise `[api] abi_version` (see
   [ABI rules](#abi-rules) for which part).
2. Regenerate:

    ```bash
    python3 scripts/codegen/vmafx-api.py --write
    ```

3. Implement what the definition declares (hand-written C lives in
   `core/src/vmafx/`; [ADR-1906](../adr/1906-vmafx-core-api-semantics.md)
   records the rules it follows for logging, struct sizes and references),
   build, and run the tests:

    ```bash
    python3 scripts/ci/run_meson_test.py -- -C build test_vmafx_abi_layout \
        test_vmafx_api_slice test_vmafx_api_generated_current \
        test_vmafx_api_abi_append_only test_vmafx_api_generator \
        test_vmafx_python_binding check_exported_symbols \
        test_vmafx_context test_vmafx_model test_vmafx_frame test_vmafx_score \
        test_vmafx_bitexact test_vmafx_lifetime test_vmafx_sha256
    ```

4. Check that the new definition is an append-only successor of the one you
   started from, and draft the changelog fragment:

    ```bash
    python3 scripts/codegen/vmafx-api.py --abi-check --against-ref origin/master
    python3 scripts/codegen/vmafx-api.py --changelog origin/master
    ```

    `--changelog` prints the text of `changelog.d/added/api-<slug>.md` (the
    symbols, types, fields, constants and options the definition gained) and
    of `changelog.d/changed/api-<slug>.md` (entries newly deprecated); pick
    the slug, edit the wording and commit the fragments yourself.

The generator needs Python 3.11 or newer and nothing else (`tomllib` from the
standard library). It never runs during a normal build.

## ABI rules

- **Append-only within an ABI major** (HISS-14): new functions, new fields at
  the end of a struct with `sized = true`, new constants, new flag bits and
  new options. Removing or renaming anything, changing a parameter list or a
  callback signature, reordering, retyping or resizing a field, renumbering a
  constant or a bit, changing `since`, changing an option's type or proto field
  number, or growing a struct that another struct embeds by value is a break.
- **Before `v1.0.0`** the ABI is 0.x (ADR-1852 decision D2): a break needs a
  higher minor (`0.1.0` to `0.2.0`), and the pull request lists every break
  `--abi-check` reports as accepted. From `1.0.0` a break needs a higher major,
  `!` in the pull request title and a `Migration:` footer.
- **Additions** raise `abi_version`. A function's `since` selects its linker
  version node (`VMAFX_0.1`, `VMAFX_0.2`, ...). Within 0.x an addition may join
  the current minor's node (a patch bump such as `0.1.0` to `0.1.1` is
  enough), so work packages that add functions in parallel do not each claim
  a minor; it may never join an older node. From `1.0.0` a node that shipped
  is frozen: an addition's `since` is a newer minor than the ABI it is
  compared with. [ADR-1897](../adr/1897-vmafx-abi-0x-numbering.md) records
  this rule and the alternative of a new minor per addition.
- **Deprecation**: `deprecated = { since, replacement, removal }` adds the
  `VMAFX_DEPRECATED("...")` attribute to a function (define
  `VMAFX_NO_DEPRECATION_WARNINGS` to silence it), an `@deprecated` line to the
  header documentation and a note to the reference page. `replacement` must
  name a declared function, type, constant, field (`Struct.field`) or option.

## What the definition holds

| Table | Generates | Rules the generator enforces |
| --- | --- | --- |
| `[api]` | ABI version macros (`version_header`), export and deprecation macros and status codes (`base_header`), `hide_unlisted` for the version script | `schema = 2`; `abi_version` is `MAJOR.MINOR.PATCH`; both named headers are `core` headers |
| `[[headers]]` | One public header each under `core/include/`, its install line and its reference page | `group` is `umbrella` (exactly one; includes every `core` header and declares nothing), `core`, or `optional` (never included by the umbrella; `includes` names outside headers it needs) |
| `[[status]]` | `VmafxStatus` codes, `vmafx_status_name()`, the status-to-errno map of the compat layer | The first code is the OK code; names and values unique |
| `[[enums]]` | C enums, Python `IntEnum`s | Names and values unique; fields carry them as `u32` with `enum = "..."` |
| `[[flags]]` | `#define NAME (UINT32_C(1) << bitU)` constants, Python `IntFlag`s | `type` is `u32` or `u64`; bits unique and inside the width; fields carry them with `flags = "..."` |
| `[[handles]]` | Opaque types | Each names its declared release function |
| `[[callbacks]]` | Function-pointer typedefs | Parameters are inputs; the last one is `user` of type `ptr` (`void *user`) |
| `[[structs]]` | Structs, `*_INIT` macros, layout asserts | `sized = true` adds `struct_size` first; fields are fixed-width scalars, `ptr`, `cstr`, `size`, `uptr`, nested structs, handles or callbacks; `count = N` makes a fixed array (1 to 64); `const = true` on a handle field; a sized struct is never embedded in an unsized one; no by-value cycle |
| `[[functions]]` | Declarations, version-script and export lines, Python signatures, reference rows | Known types and pass modes; the `VmafxError **` parameter is last and the result is a status; the Python method is the C name without `vmafx_context_` / `vmafx_`, or `python = "<name>"`, and two functions with one method name on one Python class stop generation (the later one would hide the earlier) |
| `[[compat]]` | `libvmaf.h` function bodies on the new API | The target exists; a `build` field map covers every field of the struct it fills |
| `[[option_groups]]` | Every scoring surface: the `vmaf` option table and usage, the MCP schemas and argument-vector spec, `ScoreOptions`, the OpenAPI components, the `vmafx` filter's AVOption table, the documentation option tables ([Option groups](#option-groups)) | `surfaces` is a subset of `cli`, `ffmpeg`, `mcp`, `proto`; an option's surfaces are a subset of its group's and it has a spelling for each; defaults inside their `range`, `enum` or `choices`; spellings, short letters and proto field numbers (per message) unique; option names unique across groups |

Every top-level entry carries `header` (except status codes and option groups)
and `since`. Enum values, flag bits, struct fields and options inherit their
parent's `since` unless they name a later one. Each header includes the
headers whose types its declarations use, computed from the definition; an
include cycle stops generation.

Parameter pass modes: `in` (value; `const T *` for a struct; `T *` for a
handle), `in_const` (`const T *`), `out` (`T *`), `out_handle` (`T **`),
`error` (`VmafxError **`). Scalar types: `u32`, `u64`, `i32`, `i64`, `f32`,
`f64`, `uptr` (`uintptr_t`), `size` (`size_t`), `ptr` (`void *`), `cstr`
(`const char *`), `status`.

[`scripts/codegen/tests/fixtures/full.toml`](../../scripts/codegen/tests/fixtures/full.toml)
uses every feature of the format; the generator tests render it and compile
its C output.

## Outputs

| File | Content |
| --- | --- |
| `core/include/vmafx/*.h` | Public headers: the umbrella `vmafx.h`, the core headers of the design's header layout, and optional headers such as `libvmaf_bridge.h` |
| `core/include/vmafx/meson.build` | Install list of those headers |
| `core/src/vmafx.map` | Linker version script, one node per ABI minor; libvmaf links it on ELF targets |
| `core/src/vmafx.def` | Windows export list, for the shared library split |
| `core/src/vmafx_symbols.txt` | Exported symbols and their version nodes, read by `check_exported_symbols` |
| `core/src/vmafx/status_gen.c`, `status_gen.h` | Status names and errno maps |
| `core/src/compat/libvmaf/libvmaf_gen.c`, `status_errno_gen.c` | The generated (`shim`, `glue`) libvmaf functions of the compat library `libvmaf.so.3` and its status-to-errno map |
| `core/src/vmafx/engine_names_gen.h` | Forced on every engine translation unit: the engine's own libvmaf bodies compile as `vmaf_engine_<stem>` |
| `core/src/vmafx_legacy_<backend>.map`, `core/src/libvmaf_symbols.txt` | Version node of the libvmaf functions a built backend keeps in the engine (declared exceptions), and which library exports each libvmaf function in which build |
| `core/test/compat_conformance_gen.h`, `compat_conformance_table_gen.c` | The two function tables (old libvmaf, compat library) and the coverage rule of `test_compat_conformance` |
| `core/test/test_vmafx_abi_layout.c` | `_Static_assert` of every struct size, alignment, field offset, array length and constant |
| `bindings/python/vmafx/_api.py` | ctypes binding; checks its layouts at import |
| `docs/api/vmafx/reference.md` and one page per header | [Reference index](../api/vmafx/reference.md) |
| `core/tools/cli_options.gen.inc` | The `vmaf` short option string, long-only identifiers, `long_opts[]` and usage lines |
| `pkg/scoreopts/options.gen.json`, `mcp-server/vmaf-mcp/src/vmaf_mcp/options.gen.json` | MCP tool input schemas, server-side options, argument-vector spec, command-line flags, library defaults (one text, two packages) |
| `proto/vmafx/v1/vmafx_api.proto` | `ScoreOptions` and the messages of structs that name `proto` (`Provenance`) |
| `api/vmafx/v1/groupversion_info.go`, `api/vmafx/v1/<kind>_types.go` | From `api/vmafx-platform.toml`: the Go types of the custom resources with their kubebuilder markers ([Kubernetes resources](#kubernetes-resources)) |
| `deploy/helm/vmafx/values.yaml`, `deploy/helm/vmafx/values.schema.json` | From `api/vmafx-platform.toml`: the chart's default values with their comments, and their JSON schema ([Helm chart values](#helm-chart-values)) |
| `cmd/<binary>/config_keys.gen.go`, `deploy/helm/vmafx/templates/_config.gen.tpl`, the environment tables of the binaries' pages and `docs/usage/env-vars.md` | From `api/vmafx-platform.toml`: the Go binaries' CompoundKeys, their environment tables and the chart's `VMAFX_*` entries ([Environment of the Go binaries](#environment-of-the-go-binaries)) |
| `api/openapi/components.gen.yaml` | The same messages as OpenAPI 3.0 schemas; also spliced into `api/openapi/vmafx-server-v1.yaml` |
| `ffmpeg-patches/src/vf_vmafx_options.h` | The `vmafx` filter's context fields, AVOption table and value-name lists |
| Regions of `docs/usage/cli.md`, `docs/usage/ffmpeg.md`, `docs/mcp/tools.md`, `docs/server/api-contract.md` | Option tables between `BEGIN GENERATED` / `END GENERATED` markers |

Every generated C file is already in the repository's clang-format style (long
`*_INIT` macros sit between `clang-format off` / `on` markers) and the Python
file in black's style, so formatter hooks never rewrite them.

Since the library split (ADR-1852 decision D3,
[ADR-2094](../adr/2094-libvmaf-compat-library-split.md)), `hide_unlisted = true`:
`libvmafx.so.1` exports the `vmafx_*` functions and nothing the script does
not list. The libvmaf functions live in `libvmaf.so.3`, generated (`shim`,
`glue`) or hand-written (`manual`, in `core/src/compat/libvmaf/`) from the
`[[compat]]` table; a backend's functions that still live in the engine are
`engine` entries, exported through `core/src/vmafx_legacy_<backend>.map` in
builds with that backend.

### The compat table

| Key | Meaning |
| --- | --- |
| `kind` | `shim` / `glue` (body generated from `null_checks`, `context`, `build`, `out_handle`, `out_struct`, `args`, `post` (asserted after success), `store`, `return_expr` or `body`), `manual` (hand-written), `engine` (no compat definition) |
| `target`, `calls` | The VMAFx function the entry is built on; for `manual`, every `vmafx_*` function its file calls (a test compares them with the source) |
| `file` | `manual`: the source under `core/src/compat/libvmaf/` |
| `engine_with`, `until` | A backend whose builds keep the engine's own definition, and what ends that exception |
| `when` | A build feature the function exists in (`mcp`) |
| `platform` | An operating system the function exists on (`windows`: declared under `#ifdef _WIN32`); its name stays out of the ELF version scripts and its row in `core/src/libvmaf_symbols.txt` ends in `@windows` |
| `note` | One line for the [migration table](../api/vmafx/compat.md) |

Every engine translation unit is compiled with the generated
`core/src/vmafx/engine_names_gen.h` (`vmaf_engine_name_args`), which names
the engine's own libvmaf bodies `vmaf_engine_<stem>`; code that uses the
libvmaf API (the compat library, the tools, black-box tests) defines
`VMAF_PUBLIC_NAMES`. `test_compat_conformance` compares every compat function
with its engine body.

## Option groups

Every scoring option of every surface is one entry of an option group
([ADR-2044](../adr/2044-vmafx-option-groups-scoring-contract.md)). Groups and
options are TOML array tables:

```toml
[[option_groups]]
name = "threads"
since = "0.1"
doc = "Worker threads of the feature extractors."
surfaces = ["cli", "ffmpeg", "mcp", "proto"]
mcp_tools = ["vmaf_score", "vmaf_score_encoded"]

[[option_groups.options]]
name = "threads"
type = "uint"
doc = "Worker threads of the feature extractors, capped to the hardware threads (0: score in the calling thread)."
cli = ["--threads"]
ffmpeg = "threads"
aliases = ["n_threads"]
mcp = "threads"
proto = { field = 14 }
```

| Key | Meaning |
| --- | --- |
| `type` | `bool`, `int`, `uint`, `float`, `string`, `enum` (`enum = [...]`) or `flags` (several values of `enum = [...]`) |
| `default` | The value an unset option means; omitted when the consumer's own behaviour applies (documented in `doc`) |
| `default_macro` | The default is the library's, named by this C macro (`VMAF_DEFAULT_MODEL_VERSION`); never a literal. Surfaces that cannot read a header get its value from the generated file, so the drift check fails when the header moves |
| `default_form` | How the MCP tool schemas spell a `default_macro` default: `"version={}"` advertises `version=<VMAF_DEFAULT_MODEL_VERSION>` as the `model` default of every scoring tool, the value read from the header when the file is generated |
| `surface_defaults` | Per-surface defaults (`{ mcp = "json" }`) |
| `range`, `choices` | `[min]` or `[min, max]` of a number; the integers an integer option takes |
| `surfaces` | The surfaces of this option (default: the group's) |
| `repeat` | `true` or the surfaces on which the option is given several times |
| `cli`, `cli_short`, `cli_meta`, `cli_id`, `cli_values` | Long spellings (the first is the usage name), short letter, usage placeholder, getopt identifier (default `'x'` for a short option, `ARG_<NAME>` otherwise), one switch per enum value (`{ xml = "--xml" }`) |
| `ffmpeg`, `aliases` | Filter option name and upstream aliases |
| `mcp`, `mcp_required` | MCP argument name; required in the tools of the group's `mcp_tools` |
| `proto` | `{ field = N }`, unique in the group's `proto_message` (default `ScoreOptions`) |
| `argv`, `argv_flag`, `model_suffix` | How a server passes it to `vmaf`: `core` (placed by the server), `extra` (appended in definition order) or `none`; the flag when not the first long spelling; a model-spec suffix (`":adm.adm_norm_view_dist={}"`) instead of a flag |
| `reserved` | Every surface accepts only the default; the text says why |

Order matters: `vmaf --help` lists options in definition order and both MCP
servers append the `extra` flags in that order. A struct with
`proto = "Name"` becomes a proto message and an OpenAPI schema with one field
per struct field (numbers in field order, so they never change); a field
with no proto form (a handle, a pointer, a callback) stops generation.

The generated proto needs `buf generate proto` afterwards (the template in
`buf.gen.yaml`, then `python3 scripts/proto/postprocess_gen_go.py`) and the
OpenAPI change needs the server stubs regenerated
(`gen/go/AGENTS.md`). `buf breaking proto --against '.git#ref=refs/remotes/origin/master,subdir=proto'`
must stay clean.

## Platform definition

`api/vmafx-platform.toml` holds the services of the platform: the scoring
service `VmafxScoring` (package `vmafx.v1`) and the controller `VmafxController`
(package `vmafx.controller.v1`), and the custom resources of the operator
([Kubernetes resources](#kubernetes-resources)). The generator writes one
protobuf file per `[[files]]` entry, and buf turns those into the Go bindings
under `gen/go`:

| Table | What it declares |
| --- | --- |
| `[platform]` | `version = 1`, the format of the definition |
| `[[files]]` | A protobuf file: `name`, `path` (`proto/<package as directories>/<file>.proto`), `package`, `go_package` (the import path the bindings live under, then `;` and the Go package name), `doc` |
| `[[services]]` | A gRPC service in a file: `rpcs` with `name`, `request`, `response`, `doc` and `client_stream` / `server_stream` for streaming RPCs |
| `[[enums]]` | An enum: `values` with `name`, `number` and an optional `doc`; the first value is number 0 |
| `[[messages]]` | A message: `fields` with `name`, `type`, `number`, `doc`, and `repeated = true` or `oneof = "<group>"` |

A field's `type` is a protobuf scalar (`string`, `double`, `int64`, `bytes`,
...), `map<K, V>` (K an integer, `bool` or `string`), a message or enum of the
platform definition, or a message the core definition generates: an option
group's `proto_message` (`ScoreOptions`) or a struct's `proto` (`Provenance`).
A type from another file is imported, a type from another package is written
with its package. Documentation keeps its own line breaks and is wrapped at 96
columns.

Names and numbers are the wire contract. Within a `v1` package a change only
adds: a message, an RPC, a field or an enum value with a new number. Renaming or
renumbering is a break, which `buf breaking` refuses (see below); a break needs
a `v2` package beside `v1`. Names that predate these rules and do not follow
buf's style (the service names without `Service`, `JobStatus` values without
the `JOB_STATUS_` prefix, `Job` as the answer of two RPCs) are exempted by name
in `buf.yaml` and stay as they are.

### Change a service or message

1. Edit `api/vmafx-platform.toml`.
2. Regenerate the protobuf files, then the Go bindings:

    ```bash
    python3 scripts/codegen/vmafx-api.py --write
    python3 scripts/codegen/proto_generate.py --write
    ```

    `proto_generate.py` runs one `buf generate` with the repository's
    `buf.yaml` and `buf.gen.yaml`. buf is the release `BUF_VERSION` in
    `build-config.env`, run through `go run` so the Go module checksum database
    verifies it; the `protoc-gen-go` and `protoc-gen-go-grpc` plugins are the
    versions `go.mod` pins as tools. `go run` of a versioned package asks the
    module proxy on every run, so a CI job installs that release once with
    `go install` (`scripts/ci/prefetch_go_modules.py`, into
    `~/.cache/vmafx-tools/buf-<version>/`) and `proto_generate.py` runs the
    installed copy when it reports the pinned version. It then adds the
    `// SAFETY:` proofs
    (`scripts/proto/postprocess_gen_go.py`) and writes every `*.pb.go` under
    `gen/go`, removing stale ones. The `module=` option puts each binding
    under the path of its `go_package`: `gen/go` for `vmafx.v1`,
    `gen/go/controller` for `vmafx.controller.v1`.

3. Check wire compatibility against the branch you started from:

    ```bash
    python3 scripts/codegen/proto_generate.py --check --breaking-against origin/master
    ```

    `buf breaking` runs with the `WIRE_JSON` rules: what clients send and
    receive in binary and JSON form must stay readable. Moving a file or its Go
    package is not a break; a removed or renumbered field, a changed type or a
    renamed enum value is.

    CI runs `buf lint` and `buf breaking` on every ready pull request and
    every `master` push: the `Buf Lint + Breaking` job of
    [`lint-and-format.yml`](../../.github/workflows/lint-and-format.yml)
    installs the same pinned `BUF_VERSION` and compares the protobuf files with
    the pull request's base commit (on a push, the previous `master` commit).
    A red job names the file, line and rule; fix the proto definition in
    `api/vmafx-platform.toml` or `core/api/vmafx.toml` and regenerate, rather
    than editing `proto/` by hand.

### Kubernetes resources

The same file declares the custom resources of the `vmafx.dev/v1` API group:
`VmafxJob`, `VmafxNode`, `VmafxModelTraining` and `VmafxTenant`. A `[[messages]]`
or `[[enums]]` entry that names a `group` instead of a protobuf `file` is a
Kubernetes type.

| Table | What it declares |
| --- | --- |
| `[[groups]]` | An API group version: `name` (what entries refer to), `group`, `version`, `path` (the Go package directory), `go_package`, `doc` |
| `[[resources]]` | A custom resource: `kind`, `group`, `doc`, `spec` and `status` (message names), `short_names`, `scope` (`Namespaced`, the default, or `Cluster`), `status_subresource` (default true), `spec_required` (default false), `printer_columns` with `name`, `type` (`string`, `number`, `integer`, `boolean`, `date`) and `json_path` |
| `[[enums]]` with `group` | A string enum: `values` with `name` and an optional `doc` |
| `[[messages]]` with `group` | A struct: `fields` with `name` (the JSON name, lowerCamelCase), `type`, `doc`, and optionally `go`, `optional`, `repeated`, `pointer`, `default` and validation keys |

A field's `type` is `string`, `bool`, `int`, `int32`, `int64`, `float64`,
`Time` (`*metav1.Time`), `map<string, string>`, or an enum or message of the
same group. The Go name is derived from the JSON name (`tenantId` becomes
`TenantID`; `id`, `gpu`, `uri`, `url`, `oidc` and `rbac` are written in
capitals); `go` overrides it. `pointer = true` makes a single value a pointer,
so that an explicit `false` or `0` survives a default. A field with a
`default` must be `optional`. The validation keys become kubebuilder markers
and from those the CRD schema: `enum`, `items_enum`, `minimum`, `min_length`,
`items_min_length`, `max_items`, `pattern`, `format`. An enum type carries its
own values, so a field of that type takes no `enum`.

Within `v1` a resource only grows. Adding a resource, an optional field, an
enum value or a short name, or widening a bound, is compatible; removing a
field, version, short name or the status subresource, adding a required
field, narrowing an enum, a bound, a type or a format, or changing a pattern
or a default is not, and needs a new version.

### Change a custom resource

1. Edit `api/vmafx-platform.toml`.
2. Regenerate the Go types, then the deepcopy code, the CRDs and the RBAC
   role:

    ```bash
    python3 scripts/codegen/vmafx-api.py --write
    python3 scripts/codegen/crd_generate.py --write
    ```

    `vmafx-api.py` writes `api/vmafx/v1/groupversion_info.go` and one
    `<kind>_types.go` per resource, gofmt-clean. `crd_generate.py` runs
    controller-gen, the version `go.mod` pins as a tool, three times: `object`
    writes `api/vmafx/v1/zz_generated.deepcopy.go`, `crd` writes
    `deploy/helm/vmafx/crds/vmafx.dev_<plural>.yaml` (the only CRD tree: the
    chart installs it and the operator's envtest suite loads it), and `rbac`
    writes `config/rbac/role.yaml` from the `+kubebuilder:rbac` markers of
    `cmd/vmafx-operator`. It adds the SPDX header and removes stale files.

3. Check compatibility against the branch you started from:

    ```bash
    python3 scripts/codegen/crd_generate.py --check --compat-against origin/master
    ```

4. A new RBAC marker on a reconciler changes `config/rbac/role.yaml`; the
   chart's `deploy/helm/vmafx/templates/operator-rbac.yaml` must then grant it
   too, which `scripts/ci/tests/test_helm_operator_rbac.py` checks.

### Helm chart values

The same file holds the chart's values: one `[[chart]]` entry per key of
`deploy/helm/vmafx/values.yaml`, in the order of that file. The generator
writes `values.yaml` and `values.schema.json` from them; neither is edited by
hand.

| Key | What it holds |
| --- | --- |
| `path` | The dotted key, such as `controller.store.backend`; a key that contains a dot is written in double quotes (`matchLabels."kubernetes.io/metadata.name"`) |
| `value` | The default. Absent for a mapping whose keys follow as their own entries, and for a key only the schema has |
| `lead` | The comment and blank lines written above the key, verbatim with their indentation |
| `note`, `note_column` | A comment on the key's line and the column of its `#`; without a column, two spaces after the value |
| `quoted` | Write a string in double quotes although YAML would read it plain; strings that need quotes get them anyway |
| `literal` | Write a multi-line string as a `\|` block |
| `[chart.schema]` | The key's JSON schema, every keyword but `properties`, which the generator builds from the entries below the key |
| `schema_order` | The order of a mapping's schema properties, where it differs from the order of `values.yaml` |

`[chart_root]` holds the schema's root keywords (`[chart_root.schema]`), the
order of the top-level properties and the comment lines after the last key
(`values_tail`); `[[chart_defs]]` are the schema's own `$defs`. The schema file
has one layout: an object or array that fits on its line within 100 columns
is written on one line, any other is expanded.

A `$ref` of `k8s:<name>` refers to a Kubernetes type, such as
`k8s:io.k8s.api.core.v1.Toleration`, as the chart's minimum supported
Kubernetes minor defines it. `build-config.env` pins that release
(`K8S_SCHEMA_VERSION`, the last patch of the minor the
[deployment guide](k8s-deployment.md#prerequisites) names) and the SHA-256 of
each OpenAPI v3 file of its tag. `scripts/codegen/k8s_openapi.py --write`
downloads them, checks the digests, and writes every type the definition names,
and every type those reach, to `api/kubernetes/openapi-subset.json`, without
descriptions and `x-kubernetes-*` extensions. The generator copies the types a
schema reaches into its `$defs`. Kubernetes accepts fields it does not know in
these types, so the schema does too; a known field must have its type, and a
required field must be there.

To change a value or its schema:

1. Edit the entry in `api/vmafx-platform.toml`. A new key gets a new entry at
   its place in the file.
2. If the entry names a Kubernetes type the subset does not hold yet, run
   `python3 scripts/codegen/k8s_openapi.py --write`.
3. Regenerate: `python3 scripts/codegen/vmafx-api.py --write`.

### Environment of the Go binaries

The same file lists every environment variable the Go binaries read and how
the chart sets it.

| Table | What it holds |
| --- | --- |
| `[[config_binaries]]` | A Go binary: `name`, package `dir`, the `page` with its environment table, and the chart `workloads` that run it |
| `[[config]]` | One variable: `env` (its name), `key` (the golusoris configuration key it maps to; none for a variable the binary reads directly), `binaries`, `type`, `default`, `doc`, `secret`; `[config.<binary>]` overrides `type`, `default` or `doc` for one binary |
| `[[chart_workloads]]` | An environment list in the chart's templates: `name` and the `indent` of its entries |
| `[[chart_env]]` | One `VMAFX_*` entry of those lists, in order: `env`, `workloads`, the YAML `comment` lines above it, a `when` condition (a template expression), and one value source: `value` (a template expression), `field_ref` or `secret_ref` (`name` and `key` expressions) |
| `[[chart_maps]]` | A value helper: `source` compared with each of `cases`, `default` otherwise (the backend of each GPU vendor, `vmafx.backendEnvValue`) |

The generator writes:

- `cmd/<binary>/config_keys.gen.go`: the binary's golusoris CompoundKeys, every
  `key` with an underscore inside a segment. golusoris turns each underscore of
  a `VMAFX_` name into the key delimiter except in these keys.
- One table per binary, with the same columns everywhere, into the binary's
  page and into the Go services section of the
  [environment reference](../usage/env-vars.md#go-services-golusoris). The
  chart value column lists the values the chart's entries read.
- `deploy/helm/vmafx/templates/_config.gen.tpl`: a helper `vmafx.env.<workload>`
  per workload holding its `VMAFX_*` entries with their comments and
  conditions at the list's indentation, and the value helpers. A template
  includes the helper where its environment list holds them; no other template
  writes a `VMAFX_*` entry.

To add or change a variable: edit its `[[config]]` entry (and the `[[chart_env]]`
entry when the chart sets it), then run `python3 scripts/codegen/vmafx-api.py
--write`. A variable a binary reads but the definition lacks is a missing row
in every table; the binary's environment tests (`env_test.go`,
`main_test.go`) read the generated `compoundKeys`.

## Gates

| Gate | Fails when | Shown failing by |
| --- | --- | --- |
| `test_vmafx_api_generated_current` (Meson, `fast`) | A generated file differs from the definition | The generator tests edit a generated header, delete the binding and a reference page, and rewrite the version script in a copy |
| `test_vmafx_abi_layout` (Meson, `fast`) | The compiler lays a struct out differently from the definition, an array has another length, or a constant changed | The generator tests compile the fixture's layout test, then a copy with `plane[3]` made `plane[4]` and one with a field widened |
| `test_vmafx_api_abi_append_only` (Meson, `fast`) | The definition is not an append-only successor of the one at the merge base with `origin/master`; skipped (exit 77, reason printed) without git, without that ref, or when the merge base has no definition | The generator tests run it in a scratch repository with a renumbered constant |
| `--abi-check` | A break without the version bump the ABI rules ask for; an addition in a version node that shipped; an addition without a version bump | Planted changes in `test_vmafx_api_generator.py` and `test_vmafx_api_abi_features.py` |
| `check_exported_symbols` (Meson, `fast`) | The library exports a `vmafx_` symbol missing from `vmafx_symbols.txt`, does not export a listed one, or exports one in another version node | `test_vmafx_api_symbols.py` builds a small library and plants each defect |
| Link of `libvmaf.so` | The version script names a function no source defines (`--no-undefined-version`) | `test_vmafx_api_symbols.py` links without one listed function |
| Validation (every run) | Any rule in the table above | Planted definitions in the generator tests |
| `test_vmafx_api_generator` (Meson, `fast`) | Any of the generator tests fails | Runs `scripts/codegen/tests/test_*.py` |
| `test_cli_option_table` (Meson, `fast`) | A long spelling or short option the hand-written CLI table accepted is gone, takes or drops an argument, or no longer reaches its short option | Removing `--tiny_model` from the definition |
| `test_vmafx_score_contract` (Meson, `contract`) | The CLI, the C API, gRPC `Score` and `POST /v1/score` disagree on a score or on the library build | A server mapping that drops `subsample` |
| `TestEmbeddedFileEqualsThePythonServerCopy` (`go test ./pkg/scoreopts`) | The two copies of `options.gen.json` differ | An edited copy |
| `test_proto_generated_current` (Meson, `fast`) | `buf lint` reports a protobuf file, or a `*.pb.go` under `gen/go` differs from what `buf generate` and the proofs produce; skipped (exit 77, reason printed) without Go or the module proxy | A hand edit of `gen/go/vmafx.pb.go`, and a `buf.yaml` without the exemption of the service names |
| `proto_generate.py --breaking-against REF` | `buf breaking` (`WIRE_JSON`) finds an incompatible change against REF | A renumbered field in a copy |
| Platform validation (every run) | A duplicate name or number, an unknown type or file, a field number outside 1 to 536870911 or in 19000 to 19999, an enum without value 0, a map key that is not an integer, `bool` or `string`, a repeated oneof member, a path outside its package directory | `scripts/codegen/tests/test_vmafx_platform.py` |
| Kubernetes validation (every run) | An unknown type or group, a duplicate type, field or Go name, a default on a required field, a pointer to a list or map, `enum` on an enum-typed field, a scope other than `Namespaced` or `Cluster`, a spec or status that is not a message, an unknown printer column type, a name that is both a protobuf and a Kubernetes type | `scripts/codegen/tests/test_vmafx_platform_kube.py` |
| `test_crd_generated_current` (Meson, `fast`) | `zz_generated.deepcopy.go`, a CRD under `deploy/helm/vmafx/crds` or `config/rbac/role.yaml` differs from what controller-gen writes, or a generated file is missing or left over; skipped (exit 77, reason printed) without Go | A hand edit of a CRD, an extra and a missing file, in `scripts/codegen/tests/test_crd_generate.py` |
| `test_crd_compat` (Meson, `fast`) | A generated CRD narrows the one at the merge base with `origin/master` (see [Kubernetes resources](#kubernetes-resources)); skipped (exit 77, reason printed) without Go, git or that ref | Every narrowing planted in `test_crd_generate.py`, and `max_items = 16` for the tenant roots in the definition |
| `test_helm_operator_rbac.py` (Helm Chart workflow) | The chart does not bind the operator's service account to every rule of `config/rbac/role.yaml` | A chart without the lease rule, and a marker for a resource the chart does not grant |
| Chart validation (every run) | A path twice or below a missing parent, a key with a value above keys with values, an entry with neither a value nor a schema, a `lead` with a non-comment line, `quoted` or `literal` on a non-string, a `note` without a value, `properties` written by hand, a `$ref` that is neither a `chart_defs` name nor `k8s:<type>`, a `schema_order` that is not a permutation | `scripts/codegen/tests/test_vmafx_platform_chart.py` |
| `test_k8s_openapi_subset_current` (Meson, `fast`) | `api/kubernetes/openapi-subset.json` differs from the types the definition reaches in the pinned release, or a downloaded file has another SHA-256; skipped (exit 77, reason printed) when the files cannot be downloaded | A stale subset and a wrong digest in `scripts/codegen/tests/test_k8s_openapi.py` |
| `test_helm_values_schema.py` (Helm Chart workflow) | The chart renders a value Kubernetes 1.26 refuses in a Kubernetes-typed key, or refuses a valid one | 14 planted values (a toleration with `tolerationSeconds: "60"`, a spread constraint without `topologyKey`, a volume without a name, ...), each accepted by the schema before the Kubernetes types |

The compiler, `clang-format` and linker cases of the generator tests skip,
with the reason, when the tool is not installed.

The format case compares against one clang-format major only: the one the
`clang-format` hook pins in `.pre-commit-config.yaml`, because other releases
align macros differently (clang-format 18.1.3, the ubuntu-24.04 runner's, and
17 reject output that 18.1.8 and 19 to 23 accept). It uses `VMAFX_CLANG_FORMAT`
when set, which must be that major or the test fails, else
`clang-format-<major>` or `clang-format` on `PATH`; any other major makes it
skip and name the version it found. The Tooling Tests job installs the pinned
release from `requirements/locks/tooling-tests.txt` and sets
`VMAFX_CLANG_FORMAT`, so the case always runs there. To run it locally with
another system version:

```bash
python3 -m venv /var/tmp/cf && /var/tmp/cf/bin/pip install clang-format==23.1.2
VMAFX_CLANG_FORMAT=/var/tmp/cf/bin/clang-format \
    python3 -m pytest scripts/codegen/tests/test_vmafx_api_layout.py
```
