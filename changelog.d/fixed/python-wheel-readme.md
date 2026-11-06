- **The `vmafx` Python package now carries a long description.** The generated
  `bindings/python/pyproject.toml` declares `readme = "README.md"`, and the API generator now
  renders `bindings/python/README.md`. Without it, `twine check --strict` refused the wheel and
  source distribution, so the release dry run failed for every pull request that ran it.
