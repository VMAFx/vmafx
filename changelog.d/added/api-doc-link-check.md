- **Link check for the C API pages (issue 2436)**: `scripts/docs/check_doc_links.py`
  verifies that every relative link and heading anchor under `docs/api/`
  resolves, including links into the source tree that `mkdocs build --strict`
  only reports at INFO; `make docs-fragments-check` runs it. See
  [Documentation site design](docs/development/docs-site-design.md#link-check).
