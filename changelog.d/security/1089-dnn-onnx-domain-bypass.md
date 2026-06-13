### Security

- **fix(dnn): block non-standard ONNX operator domains in wire scanner** (ADR-1089)

  The ONNX Runtime dispatches operators by `(domain, op_type)` tuple, not by
  `op_type` alone. The wire-format scanner previously checked only
  `NodeProto.op_type` against the allowlist, leaving a bypass path: a crafted
  model could set `op_type = "Conv"` (allowlisted) with `domain = "com.evil"`,
  causing ORT to dispatch to an arbitrary custom op registered under that domain.

  The scanner now also reads `NodeProto.domain` (field 7) at every node level
  (including control-flow subgraphs). Any domain that is not the empty string
  `""` or `"ai.onnx"` is rejected with `-EPERM` before ORT instantiates a
  session. Five new unit tests cover: absent domain, explicit `"ai.onnx"`,
  custom domain rejection, field-order independence, and `"ai.onnx.ml"` rejection.
