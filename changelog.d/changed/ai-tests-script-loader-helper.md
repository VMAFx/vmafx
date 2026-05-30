- Extract the duplicated `importlib.util.spec_from_file_location → module_from_spec → exec_module`
  script-loader boilerplate that lived in 34 files under `ai/tests/` into two shared helpers,
  `load_ai_script` and `load_ai_module`, in `ai/tests/conftest.py`. Tests now call
  `load_ai_script("name")` instead of carrying ~8 lines of preamble each. Net delta:
  ~250 LOC removed, 1 helper added. No behavior change — every refactored test passes
  with the same fixtures, mocks, and assertions as before.
