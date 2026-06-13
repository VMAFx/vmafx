Fix `pytest ai/tests/` failure for second-opinion and saliency batch materializer
tests by adding `pythonpath = ["scripts"]` to `[tool.pytest.ini_options]` in
`ai/pyproject.toml`; add a committed smoke-run scaffold under
`ai/testdata/smoke-second-opinion-batch/` with synthetic fixture tables and a
`README.md` reproducer command (ADR-0991).
