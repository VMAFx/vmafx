- MCP server (`mcp-server/vmaf-mcp/`): fix `list_extractors` returning
  an empty list after the `libvmaf/` → `core/` rename (ADR-0700);
  wire the `subsample` parameter on `vmaf_score_encoded` through to
  `--subsample N` on the vmaf CLI (was silently dropped); narrow the
  `bitdepth` JSON schema from `[8,10,12,16]` to `[8,10,12]` to match
  libvmaf's actual support; remove the dead no-arg `_run_benchmark`
  definition shadowed by the progress-aware version; replace bare
  `except Exception: pass` in `_send_progress` with a DEBUG-level log,
  and log VLM candidate load failures at WARNING level in `_load_vlm`
  (ADR-0774).
