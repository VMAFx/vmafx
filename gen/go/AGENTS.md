<!-- markdownlint-disable MD013 -->
# gen/go — generated protobuf stubs

Directory contents: machine output. `buf generate` (see `buf.gen.yaml`) writes from `proto/`. `DO NOT EDIT` banner in each `*.pb.go`: hand edit survives until next regeneration, then vanishes without trace.

## Rebase-sensitive invariant: HISS-09 SAFETY proofs

`protoc-gen-go` emits two raw-descriptor aliases per file: `unsafe.Slice(unsafe.StringData(file_*_rawDesc), len(file_*_rawDesc))`, once in `file_*_rawDescGZIP`, once in `DescBuilder` literal of `file_*_init`. HISS-09 requires `// SAFETY:` proof directly above every `unsafe.*` use; upstream generator lacks hook.

Proof produced by post-generation pass, not hand:

```bash
buf generate                                          # writes gen/go/**
python3 scripts/proto/postprocess_gen_go.py           # re-applies the proofs
python3 scripts/proto/postprocess_gen_go.py --check   # verify only
```

Pass idempotent: run after regeneration reproduces committed tree byte-for-byte. **Do not add comments by hand, do not delete to "clean up" generated output** — change `scripts/proto/postprocess_gen_go.py`, re-run instead.

If future `protoc-gen-go` changes call shape, script `RAW_DESC_CALL` needle stops matching; `--check` passes while audit reports HISS-09. Mismatch signals updating needle, not suppressing finding.

## Rebase-sensitive invariant: OpenAPI stubs follow the contract

`gen/go/oapi/vmafx_server_v1.gen.go` = `oapi-codegen --config api/openapi/oapi-codegen.yaml api/openapi/vmafx-server-v1.yaml` (v2.7.0) + 8-line SPDX / regenerate header on top. Embedded spec served at `/openapi.json`. Any edit of YAML regenerates it in same PR; `cmd/vmafx-server/openapi_spec_drift_test.go::TestEmbeddedSpecMatchesContract` fails otherwise (operationId capitalisation of generator ignored). Contract texts naming default model checked by `scripts/ci/check-default-model-single-source.sh`.
