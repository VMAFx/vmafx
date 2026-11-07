---
paths:
  - dev/Containerfile
  - .dockerignore
invariant: Library source path is core/, Python harness is compat/python-vmaf/; verify renames with targeted grep.
---
<!-- markdownlint-disable MD013 -->
# Source-tree paths after rename and sweep invariant

## Source-tree paths after ADR-0700 / ADR-0870

- Library source tree renamed from `libvmaf/` to `core/` per
  ADR-0700. Python harness split into `compat/python-vmaf/` package +
  `python/` shim. Containerfile reflects this:
  - `COPY core/        /build/vmaf/core/` (was `libvmaf/`).
  - `COPY compat/      /build/vmaf/compat/` (required for editable
    Python install through `python/` shim).
  - `meson setup core/build core` / `ninja -C core/build install`.
- **Rule**: any rebase picking up upstream patch touching old
  `libvmaf/` directory must rewrite path to `core/` before applying
  it to Containerfile's COPY/source/build paths. `.dockerignore`
  carries both `core/build*/` and legacy `libvmaf/build*/` siblings
  so pre-rename worktree still excludes its build dirs; do not
  delete legacy entries.
- See [ADR-0870](../../docs/adr/0870-helm-values-schema-and-container-rebuild-audit.md)
  for audit establishing this invariant after drift went undetected
  through several merge trains.

## Source-directory rename sweep invariant (ADR-0966)

After any rename of C source root (currently `core/`, formerly
`libvmaf/` per ADR-0700), run targeted grep across
`dev/Containerfile` before committing:

```bash
grep -n 'COPY.*libvmaf\|cd libvmaf\|/build/vmaf/libvmaf' dev/Containerfile
```

`libvmaf-build` stage name, `libvmaf.so`, and `--enable-libvmafx`
(or stock upstream FFmpeg `--enable-libvmaf`)
occurrences reference **library product name**, must stay unchanged.
Only `COPY`, `cd`, and destination-path occurrences referencing
*source directory* need to track rename.

ADR-0966 fixed three references that survived ADR-0700 rename,
caused `docker compose build dev-mcp` to fail at first COPY step.
Memory rule `feedback_fix_preexisting_bugs_too` (corollary: "Rename
greps must be exhaustive") applies here: single missed grep cost
full build-blockage incident. Run check above as part of any PR
renaming top-level source directory.
