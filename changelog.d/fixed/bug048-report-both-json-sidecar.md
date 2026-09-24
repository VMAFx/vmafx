- Restore the complete `vmaf-tune report --format both` bundle: JSON, HTML,
  and Markdown are emitted together again. The regression test now invokes the
  production writer instead of duplicating its intended dispatch logic.
