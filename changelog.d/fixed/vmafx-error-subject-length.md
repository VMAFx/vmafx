- **A VMAFx error names a long path in full.** `VmafxError` kept 95 bytes of
  its subject, so a model file whose path was longer was named by a cut-off
  path; subjects now keep 1023 bytes and messages 1023.
