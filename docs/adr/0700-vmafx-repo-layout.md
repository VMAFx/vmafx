# ADR-0700: VMAFX Repo Layout

Move `libvmaf/` → `core/` and `python/vmaf/` →
`compat/python-vmaf/`.

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `build`, `workspace`, `meta`, `vmafx`

## Context

The VMAFX rebrand (ADR-0686 umbrella) modernizes the fork's identity
away from the Netflix/vmaf naming conventions. The two directories
most visibly tied to the old name are:

1. `libvmaf/` — the C library and build root. The directory name
   echoes the upstream library name but adds noise now that the fork
   is rebranding to VMAFX. The *output* artifact (`libvmaf.so`,
   `libvmaf.pc`, `<libvmaf/...>` install headers) must remain
   unchanged for ABI compatibility.

2. `python/vmaf/` — the Python harness package lives one level inside
   `python/` with the package name `vmaf`. Moving it to
   `compat/python-vmaf/` signals its role: a compatibility layer
   bridging the legacy Netflix Python API to the VMAFX C core.

Neither move touches the public ABI, public install-path headers, or
the `libvmaf.so` output artifact name — those are frozen by
ADR-0686's ABI-preservation constraint.

## Decision

We will rename the source directories:

- `libvmaf/` → `core/` (C library and build root)
- `python/vmaf/` → `compat/python-vmaf/` (Python harness package)

The Python package name remains `vmaf` (the `__init__.py` lives
inside `compat/python-vmaf/`). A `compat/vmaf` symlink (pointing to
`python-vmaf/`) makes `import vmaf` resolve when `compat/` is on
`sys.path`. CI `PYTHONPATH` is updated to include `compat/`. A
compatibility shim at `python/vmaf/__init__.py` re-exports from the
real package for callers that set only `python/` on `sys.path`.

The `library('vmaf', ...)` declaration in `core/src/meson.build` is
unchanged — Meson derives the output name `libvmaf.so` from the first
argument to `library()`, not from the enclosing directory name.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep both directories unchanged | Zero migration risk | Fork identity stays tied to upstream naming | Rejected — rebrand requires layout consistency |
| Rename only `libvmaf/` → `core/` | Simpler | Leaves Python harness with no compat signal | Partial; user confirmed "Both" |
| Rename module to `python_vmaf` everywhere | Clean import namespace | Requires touching 90+ test files including Netflix golden-gate assertions (CLAUDE.md §8) | Too high risk to golden gate |
| Rename `core/` to `vmafx-core/` | Stronger brand signal | Hyphens in directory names complicate shell scripts | Rejected for shorter `core/` |

## Consequences

- **Positive**: source layout matches the fork's new identity;
  `compat/` corner clearly signals which code is legacy-bridging
  vs. the new C core.
- **Negative**: every open PR that touches `libvmaf/` will need a
  rebase after this PR merges. One-time cost; see
  `docs/rebase-notes.md` for the exact rebase recipe.
- **Neutral / follow-ups**:
  - All `libvmaf/build-*` directories (gitignored) remain valid after
    the move; Meson re-configures cleanly from `core/`.
  - CI workflows, Makefile, and hook scripts that reference
    `libvmaf/` as a source-tree path are updated in this PR.
  - The `python/setup.py` `"../libvmaf/src"` include path updates to
    `"../core/src"`.
  - `docs/` references to `libvmaf/src/…` source paths update to
    `core/src/…`; prose references to the *library* named `libvmaf`
    stay untouched.

## References

- Umbrella ADR: ADR-0686 (vmafx-rebrand-umbrella, in-flight PR #1546)
- req: "Both: libvmaf/ → core/ AND python/vmaf/ →
  compat/python-vmaf/" (verbatim user popup answer, 2026-05-28)
- Related PR: #1546 (umbrella ADR PR)
