- **Deduplicate training argv/runner and fake download helpers in `ai/tests`.**
  Argv building and invocation across
  `test_vmaf_tiny_train_run_provenance.py` are consolidated into `_train_argv`
  and `_run_main`, and identical fake curl download branches in
  `test_konvid_150k.py` and `test_lsvq.py` share `fake_curl` from
  `_corpus_download_fakes.py`.
