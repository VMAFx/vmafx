- **`.gitignore` stale-rule cleanup** (ADR-0905): removed two undocumented
  rules with no matching artefacts in the tree (`.gradle/`, `.pypirc`),
  rewired three rule families to the post-ADR-0700 paths
  (`python/vmaf/matlab/**` → `compat/python-vmaf/matlab/**` MEX
  blocklist; legacy `python/.gitignore` test-resource scope replaced
  with a documentation stub; new `compat/python-vmaf/core/adm_dwt2_cy.{c,cpp}`
  rule pinning the Cython-generated C from the relocated `.pyx`).
  Rules from PR #321 (`libvmaf/subprojects/` → `core/subprojects/`)
  and PR #330 (Go binary roots) are left untouched — no overlap.
- **`.github/workflows/` audit**: 24 workflows reviewed; all are active
  or correctly dormant. The five workflows with zero runs at audit
  time (`supply-chain.yml`, `upstream-watcher.yml`,
  `upstream-ffmpeg-hip-hwdec-watcher.yml`,
  `upstream-netflix-645-hdr-model-watcher.yml`,
  `upstream-netflix-955-watcher.yml`) were all added on 2026-05-30
  with legitimate cron / release-published triggers that have not
  yet fired. No removals.
