# Research: ROCm Version Audit (2026-05-19)

**Context**: user direction to bump ROCm from 7.2.3 to "current version 7.13"
in `dev/Containerfile` and add a Renovate customManager to auto-track future
releases.

## Findings

### AMD apt repository contents

Queried `https://repo.radeon.com/rocm/apt/` directly (live, 2026-05-19):

- Latest entry in the standard apt repo: **7.2.3**
- `repo.radeon.com/rocm/apt/7.13/` returns HTTP 404
- `repo.radeon.com/rocm/apt/7.13.0/` returns HTTP 404

### ROCm 7.13.0 status

`https://rocm.docs.amd.com/en/latest/` banner text (verbatim):

> The ROCm 7.13.0 technology preview release documentation is available at
> ROCm Preview documentation. **For production use, continue to use ROCm
> 7.2.3 documentation.**

The 7.13.0 preview docs reference the "TheRock" architecture — a
reorganised build system and distribution channel that is not yet
mirrored into the standard `repo.radeon.com/rocm/apt/` tree. No
installable apt packages exist for 7.13.0 as of this audit.

### Conclusion

**7.2.3 is both the current pin and the current production-stable release.**
The Containerfile does not need a version bump. The Renovate customManager
is still valuable: when AMD promotes a future stable release to the apt
repo, Renovate will open a PR automatically instead of requiring a manual
audit.

### Renovate customManager feasibility

The Containerfile uses:

```dockerfile
ARG ROCM_VER=7.2.3
... https://repo.radeon.com/rocm/apt/${ROCM_VER} ...
```

A `customType: "regex"` manager can match both the `ARG ROCM_VER=` line and
the repo URL path, providing two match strings that Renovate normalises to a
single dependency. The `customDatasources` block queries the apt index page
via `format: "html"` — Renovate's html datasource parses directory-listing
pages and extracts version strings from `<a href="…">` anchor text.

The `datasourceTemplate: "custom.rocm"` + `depNameTemplate: "rocm"` pair
routes through the custom datasource without requiring a package-registry API.
