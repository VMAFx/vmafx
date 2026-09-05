- **docs/development**: Replace impossible repo-root `pip install -e ".[dev]"`
  recipe in `languages.md` with verified per-package editable installs and
  explicit `meson==1.12.0` pinning matching `dev/Containerfile`. Add note on
  recovering virtualenvs broken by the legacy tracked `.venv` symlink.
