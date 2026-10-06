<!-- markdownlint-disable MD013 MD060 -->
# ADR-2044: VMAFx option groups generate every scoring surface, and the scoring API is a versioned contract

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer (RC4 work package 8 brief; popups 2026-10-06); RC4 work package 8
- **Tags**: api, rc4, cli, mcp, grpc, openapi, ffmpeg, server, provenance

## Context

[ADR-1852](1852-vmafx-api-redesign.md) makes `core/api/vmafx.toml` the one
definition of the VMAFx API and asks for its option groups to generate the
command-line table, the MCP schemas, the proto messages, the OpenAPI
components, the FFmpeg AVOption table and the documentation tables
([Research-2158](../research/2158-vmafx-api-redesign.md), sections 3.4, 3.5
and 4). Before this change every scoring option was spelled by hand in six
places plus the FFmpeg patches, and the parity tests between the two MCP
servers existed because those spellings drifted: both MCP servers defaulted
to the `vmaf_v0.6.1` model while the library default is
`VMAF_DEFAULT_MODEL_VERSION` ([ADR-1169](1169-default-model-v1-0-16.md)), and
the scoring server could not score a raw `.yuv` pair at all because its
request had no geometry.

The 1.0.0 scope adds issue #2155 (a versioned scoring API whose requests give
the same scores through the CLI, the C API and the server, with provenance in
every response) and the server-mode and observability work of issue #1251.
Device-targeted scoring (decided in PR #2185, implemented in RC5) needs its
target size, scaling, viewing distance and display height to be option data
now, so the RC5 profile table is a set of option values and needs no new
emitter.

## Decision

We extend the option groups and generate every scoring surface from them:

1. **Option data.** An option names the surfaces it is on, its type (`bool`,
   `int`, `uint`, `float`, `string`, `enum`, `flags`), range or allowed
   values, per-surface defaults, its command-line form (long spellings, short
   letter, placeholder, getopt identifier, one switch per enum value) and how
   a server turns it into `vmaf` flags (`argv` stage, flag, model-spec
   suffix). A default the library owns is named by its C macro
   (`default_macro`), never written as a literal; the generator reads the
   macro's value from the public headers where a surface needs the value.
   An option whose implementation lands later is `reserved`: every surface
   accepts only its default and refuses anything else with the reason.
2. **Emitters.** `core/tools/cli_options.gen.inc` (getopt table, identifiers,
   usage lines; `cli_parse.cpp` keeps only the value parsers);
   `options.gen.json`, one document written into the Go package
   `pkg/scoreopts` and into the Python MCP package, holding the MCP tool input
   schemas (JSON Schema 2020-12), every server-side option and the
   argument-vector spec; `proto/vmafx_api.proto` (one flat `ScoreOptions`
   message plus messages generated from structs that name a `proto` message,
   here `Provenance`); `api/openapi/components.gen.yaml` and the same schemas
   spliced into `api/openapi/vmafx-server-v1.yaml`;
   `ffmpeg-patches/src/vf_vmafx_options.h` for the `vmafx` filter; and option
   tables between markers in four documentation pages.
3. **One mapping per server.** Both MCP servers serve the generated schemas
   and build their `vmaf` argument vectors from the spec; the scoring server
   turns `ScoreOptions` into flags through `pkg/scoreopts`. gRPC `Score`,
   `POST /v1/score` and the REST adapter share one scoring path.
4. **Provenance.** The CLI's JSON report carries the `VmafxProvenance` record
   of the run; every scoring response carries `ScoreProvenance` (that record,
   the model the server loaded with its SHA-256, the backend receipt, the
   precision), the stream aggregate included. A report without the record
   fails the request rather than answering without provenance.
5. **The contract.** `vmafx.v1` is additive within the major: new RPCs,
   messages, fields with new numbers, new values; nothing renamed, removed,
   renumbered or retyped (`buf breaking` and the definition's append-only
   checker). The server returns lossless scores unless asked otherwise, so a
   score it returns is the CLI's and the C API's bit for bit, which
   `test_vmafx_score_contract` checks on the Netflix 576x324 pair.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Generated JSON spec read by the servers (chosen) | One file, one meaning, both MCP servers and the server agree by construction | The Go package embeds a copy of a file the Python package also ships | Chosen; the two copies are byte-identical generated outputs and a test compares them |
| Generate Go and Python source per server | No data file at run time | Two more emitters, and the servers' parity again depends on two generated programs agreeing | More code for the same guarantee |
| One proto message per option group, nested in `ScoreOptions` | Mirrors the definition's grouping | Clients write `options.threads.threads`; REST bodies nest for no reason | Flat fields are what every caller writes; numbers stay unique per message |
| Generated proto at `proto/vmafx/v1/vmafx_api.proto` (the brief's path) | Satisfies buf's directory-per-package lint | The service file sits at `proto/vmafx.proto`; two directories of one package produce two Go packages under `paths=source_relative` and moving the service file is a breaking FILE change | Kept next to the service file; the lint finding is the service file's, unchanged |
| `components.gen.yaml` referenced with `$ref` from the server spec | No duplicated schemas | The server's code generator and its embedded `/openapi.json` need a self-contained document | Spliced as well; both copies come from one emitter function |
| Keep `vmaf_v0.6.1` as the MCP default model | Old MCP numbers unchanged | A literal default model in a surface, against ADR-1169 and the brief | The library default; callers pin `version=vmaf_v0.6.1` to reproduce old numbers |
| Server keeps the CLI's `%.6f` default | Response bytes unchanged | Server scores differ from the C API in the seventh digit; the contract cannot hold | Lossless by default, `precision` still selectable |
| Leave target size and scaling out until RC5 | No refused options in the schemas | The RC5 profile table would need a new emitter or new surface fields then | Declared now as reserved options |

## Consequences

- **Positive**: no scoring option is spelled by hand on any surface; a new
  option is one definition entry plus its implementation; the MCP servers can
  no longer drift from each other or from the CLI; the server scores raw
  `.yuv` input and every response says how its score was made.
- **Negative**: MCP callers that relied on the `vmaf_v0.6.1` default get the
  library default; MCP validation is stricter where it was lax (a non-string
  `feature` entry is refused, not dropped) and laxer where it was arbitrary
  (`threads` 0, the CLI's single-thread value, is accepted); the server's
  default scores carry 17 significant digits.
- **Neutral / follow-ups**: WP9 builds the `vmafx` filter on the generated
  table; WP5 moves the provenance record into the library's report writer,
  which then replaces the CLI's splice; on the rebase onto master the
  `--check-sample-range` and `--list-backends` options of master's CLI join
  the definition; `/v1/ready` should read the same readiness check as
  `/readyz`.

## References

- `req` (RC4 work package 8 brief, 2026-10-05): "Option groups in the definition for every scoring option: model, feature, backend, device, threads, subsample, pool, precision / score format, output, window (`n_stats` / `n_stats_frames`), tiny model, masks, perceptual weight, provenance."; "default model = `VMAF_DEFAULT_MODEL_VERSION`, never a literal"; "Keep every existing CLI spelling (HISS-14); new names only additive."; "Device-target options as data (ADR-1880, RC5 fills the profile table): target size and scaling, ADM `nvd` / `rdh` reachable through option groups".
- `Q` (maintainer popup 2026-10-06, location of the generated proto): "Accept, next to vmafx.proto (Recommended)".
- `Q` (maintainer popup 2026-10-06, OpenAPI components spliced into the server spec): "Accept the splice (Recommended)".
- `req` (RC4 work package index, rows added 2026-10-06): "Versioned scoring API contract: proto / OpenAPI from the definition, contract tests CLI = C API = server, provenance in every response; server mode + observability hardening" (#2155, #1251).
- [ADR-1852](1852-vmafx-api-redesign.md), [ADR-1897](1897-vmafx-abi-0x-numbering.md), [ADR-1169](1169-default-model-v1-0-16.md), [ADR-1117](1117-mcp-tiny-ai-feature-coverage.md), [Research-2158](../research/2158-vmafx-api-redesign.md) sections 3.4, 3.5, 4 and 5.2; issues #2155, #1251, #2138, #2142; PR #2185.
