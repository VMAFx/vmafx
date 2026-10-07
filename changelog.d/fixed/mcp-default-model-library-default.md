- **The MCP tools advertise and use the library's default model.** `vmaf_score`, `vmaf_score_encoded`
  and `describe_worst_frames` of both MCP servers declared `version=vmaf_v0.6.1` as the default of
  `model` and scored with it when the argument was omitted; they now use `vmaf_v1.0.16_3d0h`, the
  default of the library, the CLI and the server (ADR-1169). Pass `model` to keep scoring with
  another model. The controller's gRPC contract documented the same stale default and says
  `vmaf_v1.0.16_3d0h` now. The default-model gate reads the `version=` spelling and the controller
  contract, so the drift cannot return unnoticed.
