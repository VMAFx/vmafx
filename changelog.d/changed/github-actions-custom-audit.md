- **docs(ci):** Audit the `.github/` tree for custom composite/JS
  actions and reusable workflows. None exist on master; all 24
  workflow files use only SHA-pinned external actions. Two
  abstraction candidates (composite `setup-build-deps`, reusable
  `meson-cpu-build.yml`) documented as deferred opportunities for
  future opportunistic pickup rather than a big-bang migration.
  (ADR-0951)
