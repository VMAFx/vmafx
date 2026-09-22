- HISS-21 burn-down across the Go tree (`pkg/**`, `gen/go`): 84 of the
  85 open invariant violations in those packages are closed, with no
  change to any numeric result, row order, argv, predicate string or
  error message.
  - **Bounded loops (HISS-02).** Every upward filesystem walk is now
    bounded by the separator count of its own starting path, the two
    rclone readiness polls by the poll count their timeout allows, the
    parquet page and value loops by the chunk's and page's own value
    counts, the libvmaf direct-path frame loop at 2^20 frames, and the
    `vmaf-perShot` CSV reader by the payload length. Each reports
    exhaustion as an error instead of spinning.
  - **Checked errors (HISS-07).** The blanked `_ = f.Close()` /
    `_ = os.Remove(...)` sites are handled: a write handle's `Close` is
    propagated (it is the last chance to report a failed flush), and a
    read handle's `Close` or a best-effort unlink is reported through
    `slog` rather than dropped.
  - **Acyclic control flow (HISS-01).** `pyjson`'s encoder unwraps
    interface and pointer indirection in a bounded loop,
    `ResolveSentinels` walks decoded JSON with an explicit work stack,
    and `normalPPF`'s upper-half reflection calls the lower-half core
    directly.
  - **Complexity bounds (HISS-04).** Twenty-one oversized functions are
    split into named steps, including `auto.RunAuto`, whose ten stages
    now share one `autoRun` value instead of eight locals — which
    retires the `//nolint:funlen,gocyclo` that stood in for the fix.
  - **Proven `unsafe` (HISS-09).** Every `unsafe.Pointer` /
    `unsafe.Slice` / `unsafe.Sizeof` / `unsafe.StringData` use in
    `pkg/libvmaf` carries a `// SAFETY:` proof. The two generated
    protobuf packages get theirs from a new post-generation pass,
    `scripts/proto/postprocess_gen_go.py`, so the proofs survive the
    next `buf generate` instead of being hand-edited back in; see
    `gen/go/AGENTS.md`.
