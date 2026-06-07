- **queue.ListAll tenant_id regression**: `ListAll` omitted `tenant_id` from
  both its SQL `SELECT` clauses and the matching `rows.Scan` call, causing
  `Job.TenantID` to always be `""` in every result returned by `StreamJobs`.
  Fixed by adding `COALESCE(tenant_id,'')` to both query paths and scanning
  into `job.TenantID`. Regression guard: `TestListAll_TenantIDRoundTrip`
  (`queue/queue_listall_test.go`).

- **auth/grpc coverage**: Added tests for `GRPCStreamInterceptor` (valid,
  missing metadata, non-Bearer prefix, Disabled mode), `GRPCUnaryInterceptor`
  Disabled mode, `RequireGRPCRole` (allowed, denied, no-claims paths),
  `AssertTenantOwns` (match / mismatch / empty-tenant), `AssertHTTPTenantOwns`
  (mismatch / empty-tenant), `MarshalPublicKeyPEM` PEM round-trip, and
  `checkAudience` with a JSON-array `aud` claim (both present and absent).
