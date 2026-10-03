- The pre-push documentation gate (`scripts/git-hooks/pre-push-mkdocs-strict.sh`)
  now blocks a push whose `mkdocs build --strict` warns. It ran the build with
  `--quiet`, which hides the warnings `--strict` counts, so broken anchors passed
  it. `scripts/ci/tests/test_pre_push_mkdocs_strict.py`, run by the docs
  workflow, fails on the quiet form.
