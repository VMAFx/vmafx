Restored strict RFC-8259 serialization across AI manifests, evaluation reports,
legacy corpus caches, and stdout summaries, plus `vmaf-tune` conformal,
auto-plan, and ladder artifacts. Non-finite diagnostics now become `null` while
file artifacts retain atomic writes.
