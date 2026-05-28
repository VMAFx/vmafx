- **Post-ADR-0700 path drift sweep**: fixed 9 stale `libvmaf/` and
  `python/vmaf/` directory references left behind after the
  `libvmaf/` → `core/` / `python/vmaf/` → `compat/python-vmaf/`
  rename. Affected files: `Makefile` (`LIBVMAF_DIR`),
  `Dockerfile` (`PATH` env), `.github/codeql-config.yml`
  (`paths`/`paths-ignore`), `.vscode/c_cpp_properties.json`,
  `.zed/settings.json`, `.claude/skills/add-gpu-backend/scaffold.sh`,
  `scripts/dev/project_modernization_audit.py`, `README.md` (logo
  image link), `AGENTS.md` (layout description),
  `.claude/skills/add-model/SKILL.md`.
