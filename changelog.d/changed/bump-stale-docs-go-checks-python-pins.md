### docs(versions): bump stale Go version, required-checks count, and Python/AI dep pins

- `docs/development/languages.md`: Go minimum version updated from 1.23 to 1.25
  (go.mod declares `go 1.26.4`; toolchain 1.26.4).
- `docs/mcp/index.md`: build comment updated from "Go 1.23+" to "Go 1.25+".
- `docs/development/release.md`: required-status-checks count updated from 19 to 25
  with full enumeration matching `.github/workflows/required-aggregator.yml`.
- `docs/architecture/c4-context.md`: "CI runs 19 required checks" updated to 25.
- `CLAUDE.md` §12 rule 3: "19 required status checks" updated to 25.
- `docs/getting-started/install/windows.md`: Python version note split to clarify
  3.11+ for core use and 3.14.5+ required for `ai/`.
- `docs/ai/training.md`: PyTorch/Lightning pins corrected to `torch>=2.12.0,<3.0`
  + `pytorch-lightning>=2.6.5,<3.0`; notes the `lightning` → `pytorch-lightning`
  PyPI rename from 2026-04-30.
