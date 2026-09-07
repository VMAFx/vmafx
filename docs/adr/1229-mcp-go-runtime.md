<!-- markdownlint-disable MD013 MD060 -->
# ADR-1229: The MCP server is the Go binary; the Python package is deprecated

- **Status**: Accepted
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: mcp, go, python, container, dependencies, fork-local

## Context

The fork carries **two complete MCP server implementations**:

- `mcp-server/vmaf-mcp/` — Python, 25 files, 15,091 lines, installed into the
  dev container's venv with `pip install -e` and attached as
  `docker exec -i vmaf-dev-mcp /opt/vmaf-venv/bin/vmaf-mcp`.
- `cmd/vmafx-mcp/` — Go, built by the container's `go-build` stage and **already
  installed at `/usr/local/bin/vmafx-mcp`** in every image.

Only the Python one was wired up. The Go binary shipped in the image and went
unused.

Measured equivalence, not assumed — both servers' `tools/list` output compared
directly:

| | Python | Go |
| --- | --- | --- |
| Tools exposed | 15 | 15 |
| Tool names | identical set | identical set |
| Tools with required args | 10 | 10, same required sets |
| No-arg tools | 5 | same 5 |

The names: `compare_models`, `describe_model`, `describe_worst_frames`,
`eval_model_on_split`, `list_backends`, `list_extractors`, `list_models`,
`probe_backend`, `run_benchmark`, `run_compare`, `run_ladder`,
`run_tune_per_shot`, `vmaf_score`, `vmaf_score_encoded`, `vmaf_version`.

The Go server was verified live over stdio: `initialize` returns
`{"name":"vmafx-mcp","version":"1.0.0"}` and `tools/list` returns all fifteen.
Its structured logs go to stderr, leaving stdout clean for JSON-RPC frames —
the property an stdio MCP server must have.

This matters beyond tidiness. The fork is heading into 1.0.0 with a stated goal
of reducing its Python surface, and this is the single largest removable block:
15k lines whose replacement already exists, already builds, and already ships.

## Decision

We will make `vmafx-mcp` (Go) the MCP server everywhere, and deprecate
`mcp-server/vmaf-mcp/` rather than deleting it in the same change.

- `dev/scripts/dev-mcp-entrypoint.sh` advertises
  `docker exec -i vmaf-dev-mcp vmafx-mcp`.
- `dev/Containerfile` drops `pip install -e /build/vmaf/mcp-server/vmaf-mcp`.
  The Go binary needs no install step — the `go-build` stage already copies
  `/out/` to `/usr/local/bin/`.
- The Python tree stays in the repository for one release, marked deprecated,
  so a regression can be diagnosed against a working reference instead of a
  deleted directory.

**Deletion is a follow-up, deliberately.** The dev container is the project's
canonical work surface (CLAUDE.md §12 rule 15); breaking it breaks everything.
Swapping the runtime is reversible in one line while the source is still there.
Deleting 15k lines in the same commit is not.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Swap and delete in one PR | Reaches the goal immediately; no deprecated tree lingering | If the Go server has a behavioural gap the tool-schema comparison cannot see — an error-shape difference, a timeout, an env assumption — the reference implementation is gone in the same commit that broke the container | Rejected; deletion follows once the swap has soaked |
| Keep both, pick per environment | Maximum safety | Two implementations of the same fifteen tools drift, and the Python one is the 15k lines this is meant to remove | Rejected — that is today's state, and it is the problem |
| Delete the Go server, keep Python | No migration work | Contradicts the pre-1.0.0 goal of shrinking the Python surface, and discards a complete, working, already-shipped implementation | Rejected |
| Port the Python server's extras to Go first | Nothing could regress | There are no extras: the tool sets are identical, verified name-by-name and required-arg-by-required-arg | Not applicable |

## Consequences

- **Positive**: removes a `pip install -e` layer from the dev container image;
  the MCP server becomes a static binary with no venv or interpreter
  dependency; sets up the removal of 15,091 lines of Python.
- **Negative**: anyone with muscle memory or a saved client config pointing at
  `/opt/vmaf-venv/bin/vmaf-mcp` must switch to `vmafx-mcp`. The path is
  advertised by the entrypoint banner on every container start, and the docs
  are updated in this PR.
- **Neutral / follow-ups**: `mcp-server/vmaf-mcp/` is deprecated, not deleted —
  a follow-up removes it along with its `pyproject.toml` entry, its CI
  references and its `docs/mcp/` mentions. The `add-mcp-tool` skill still
  scaffolds Python; it is retargeted at `cmd/vmafx-mcp/` in that follow-up.

## References

- req: the user asked to reduce the fork's Python surface as much as possible
  before 1.0.0.
- [ADR-0496](0496-prefer-dev-mcp-container-rule.md) — the docker-exec-i attach pattern
  this keeps.
- `cmd/vmafx-mcp/AGENTS.md` — the Go server's own invariants.
