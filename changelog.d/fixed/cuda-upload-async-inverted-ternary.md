- Fixed inverted ternary in `vmaf_cuda_buffer_upload_async` and
  `vmaf_cuda_buffer_download_async` (`core/src/cuda/common.c:388,416`):
  the condition `c_stream == 0 ? c_stream : cu_state->str` silently
  overrode a non-NULL caller-supplied stream with the state stream, and
  passed NULL through when the caller supplied no stream — the opposite of
  the intended "use caller's stream; fall back to state stream" contract.
  Changed to `c_stream != 0 ? c_stream : cu_state->str`. No live callers
  were affected (both functions are currently uncalled), but any new caller
  passing a custom stream would have had its choice silently discarded
  (flagged in cuda-reviewer pass for PR #702, deferred to this PR).
