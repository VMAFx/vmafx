- **The root `pyproject.toml` explains why black targets Python 3.10.** Black's target is the
  oldest interpreter any package of the tree supports, not the root `requires-python`: a 3.14
  target writes `except A, B:` clauses that the packages supporting Python 3.10 to 3.13 cannot
  parse. The change also resubmits the root dependency graph, which still listed setuptools
  84.0.0 and 84.2.0 after every manifest moved to 84.2.1.
