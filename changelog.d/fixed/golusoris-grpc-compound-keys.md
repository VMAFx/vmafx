- **Fixed — `vmafx-server`/`vmafx-node` `VMAFX_GRPC_*` env overrides silently
  ignored.** golusoris' `grpc.Module` reads four underscore-bearing leaf keys
  (`grpc.cert_file`, `grpc.key_file`, `grpc.max_recv_size`, `grpc.max_send_size`),
  but the server/node config used a bare `config.Options` with no `CompoundKeys`.
  Because the env transform splits *every* underscore on the delimiter,
  `VMAFX_GRPC_MAX_RECV_SIZE` mapped to `grpc.max.recv.size` and never bound — the
  4 MiB defaults were silently un-overridable, and TLS `cert_file`/`key_file`
  could not be set via env at all. Added `serverEnvOptions`/`nodeEnvOptions`
  declaring the four leaves as `CompoundKeys` (mirroring the operator/controller
  pattern), plus `TestServerEnvOptionsContract`/`TestNodeEnvOptionsContract` and
  end-to-end env→config bind round-trip tests so a future golusoris key addition
  regresses loudly.
