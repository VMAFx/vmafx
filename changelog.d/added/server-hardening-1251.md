- `vmafx-server` raises two framework defaults that did not fit scoring: the
  gRPC receive limit is now 64 MiB (a 1080p `ScoreStream` frame pair is 6.2 MB
  and the old 4 MiB limit rejected it) and the HTTP write timeout is 15 minutes
  (a synchronous `POST /v1/score` that took more than 60 s lost its
  connection). `VMAFX_GRPC_MAX_RECV_SIZE` and `VMAFX_HTTP_TIMEOUTS_WRITE`
  still override both. New page `docs/server/configuration.md` documents the
  source precedence (environment over file over defaults), the underscore rule
  of the environment transform and every server limit (#1251).
- Server log lines share one field set (`request_id`, `rpc`, `route`, `model`,
  `backend`, `duration_s`, `error`) defined in `pkg/observability`; the legacy
  `POST /v1/score` path logs it today (#1251).
