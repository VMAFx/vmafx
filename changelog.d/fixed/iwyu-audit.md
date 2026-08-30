- IWYU (include-what-you-use) audit on fork-added C/C++ code. Removed
  10 dead `#include` directives across 8 files and added 12 direct
  includes for symbols previously picked up transitively through wrapper
  headers, across 16 fork-authored translation units under `core/`.
  Run via `clang-include-cleaner` (LLVM 22, Clang-native IWYU
  successor); audit log + triage rationale at
  `docs/research/0776-iwyu-audit-2026-05-30.md`. DNN-gated files and
  GPU-backend files deferred to a follow-up audit inside the
  `vmaf-dev-mcp` container (host lacks ONNX Runtime + GPU SDKs).
  Build + fast test suite verified clean.
