# Server configuration

This page explains how `vmafx-server` (and the other Go binaries) read their
settings, which source wins when several are present, and how an environment
variable name turns into a configuration key. The per-setting reference for the
server is the table in [gRPC service: Configuration](grpc.md#configuration).

## Precedence

A setting is resolved from the first of these sources that defines it, highest
first:

1. **Environment variables** with the `VMAFX_` prefix.
2. **A configuration file**, if the binary was given one.
3. **Built-in defaults** (the golusoris framework defaults plus the vmafx
   defaults listed under [Limits](#limits-and-timeouts)).

No flags take part. The server's only command-line switch is `--version`.

No vmafx binary passes a configuration file to the loader today
(`serverEnvOptions` in `cmd/vmafx-server/main.go` leaves `Files` empty, and the
controller, node, operator, MCP and tune binaries do the same), so in practice
the order is **environment, then defaults**. The loader itself supports YAML and
JSON files and applies them in the order listed, later files over earlier ones,
with the environment loaded on top of all of them
(`core/config/config.go`, `New`, in golusoris `core` v0.9.2). The test
`TestConfigPrecedenceDefaultFileEnv` pins the order file over default and
environment over file, and `TestServerLoadsNoConfigFileOrFlags` pins the fact
that the server loads no file.

!!! note "File reloads"
    When a watched file changes, the loader merges the file back into the live
    configuration (`Config.reload`), so a key present in the file takes effect
    again even if an environment variable overrides it at start-up. This only
    matters once a binary is given a file.

## How an environment variable becomes a key

The loader removes the `VMAFX_` prefix, lowercases the rest and replaces
**every** underscore with the `.` separator:

| Environment variable | Configuration key |
| --- | --- |
| `VMAFX_HTTP_ADDR` | `http.addr` |
| `VMAFX_MAX_CONCURRENT_SCORES` | `max.concurrent.scores` |
| `VMAFX_MODEL_DIR` | `model.dir` |
| `VMAFX_HTTP_TIMEOUTS_WRITE` | `http.timeouts.write` |

Two consequences matter when you add or migrate a setting:

- Code must read the dotted key. `cfg.Int("max_concurrent_scores")` never sees
  `VMAFX_MAX_CONCURRENT_SCORES`; `TestConfigEnvNameSplitsEveryUnderscore` fails
  on that spelling.
- A key whose last segment contains an underscore (`grpc.max_recv_size`) cannot
  be reached by the plain transform, because `VMAFX_GRPC_MAX_RECV_SIZE` would
  become `grpc.max.recv.size`. The binary must declare such a key as a
  *compound key* in its `config.Options`. Each binary's list is generated
  (`config_keys.gen.go` in its package) from the `[[config]]` entries of
  `api/vmafx-platform.toml`, so a new key is an entry there, not a line of Go;
  see [API generation](../development/api-generation.md#environment-of-the-go-binaries).
  `TestServerEnvOptionsContract` pins the server's four `grpc.*` keys and
  `TestControllerEnvReachesGrpcKeys` the controller's. Prefer single-word leaf
  names (`http.timeouts.write`) for new keys so no declaration is needed.

## Limits and timeouts

The HTTP server and the gRPC server are hardened by default, and every limit is
overridable through the environment. The variables and their defaults are in
the [server's environment table](grpc.md#configuration). Two defaults are the
server's own rather than the golusoris framework's
(`cmd/vmafx-server/hardening.go`):

- `VMAFX_HTTP_TIMEOUTS_WRITE` is `15m`. The framework default of `60s` closes
  the connection under any clip that scores for longer than a minute.
- `VMAFX_GRPC_MAX_RECV_SIZE` is 64 MiB. The framework default of 4 MiB rejects
  a 1080p `FramePair` (6.2 MB); 64 MiB holds a 4K 16-bit pair.

`POST /v1/score` caps its own body at 1 MiB and answers `413` above it, below
the server-wide `VMAFX_HTTP_LIMITS_BODY`. `VMAFX_MAX_CONCURRENT_SCORES` is
shared by HTTP and gRPC; over the cap, HTTP answers `429`.
`TestServerDefaultsFitScoring` and `TestServerDefaultsYieldToOperator` pin the
effective values of the production graph.

## Readiness

`GET /readyz` answers `503` while the scorer is missing, while the `vmaf`
binary (`VMAFX_VMAF_BINARY`, or `vmaf` on `PATH`) is absent or not an
executable file, or while `VMAFX_MODEL_DIR` is set and is not a directory. The
checks run on every probe and use only `stat`, so a binary that disappears
after start-up takes the pod out of rotation on the next probe.
`GET /livez` stays `200` while the process runs, and `GET /startupz` reports the
scorer check only.
