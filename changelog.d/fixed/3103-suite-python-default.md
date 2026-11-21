- **Test suites:** `install.python` now defaults to `PYTHON_CI_VERSION`
  from `build-config.env`; the registry no longer repeats `3.14` in every
  suite, and the version checker rejects a redundant copy while allowing an
  explicit per-suite override (issue 3103).
